/* SPDX-License-Identifier: Apache-2.0
 * Isolated-VM-only real KMDF harness with a SYNTHETIC Bluetooth provider.
 * It compiles the production MS-PL client/connection/control/queue bodies.
 * This does NOT test BthPort, a real radio, Audio Sink PDO/PnP, or headset audio.
 */
#include "clisrv.h"
#include "device.h"
#include "queue.h"
#include "lab_control.h"
#include <wdmsec.h>
C_ASSERT(sizeof(ol_lab_status) == 48);

ULONG DebugLevel = TRACE_LEVEL_ERROR;
ULONG DebugFlag = 0xff;
static WDFDEVICE LabMain, LabPort;
static WDFWAITLOCK PortGate;
static WDFQUEUE PendingOpenQueue;
static volatile LONG FailOpen, FailClose, KeepCallbacks, HoldOpen, PendingOpen;
static volatile LONG64 OpenDeferred, OpenCancelled, OpenControlCompleted;

typedef struct {
    PFNBTHPORT_INDICATION_CALLBACK Callback;
    PVOID Context;
    PDEVICE_OBJECT Reference;
    volatile LONG Pending;
    WDFREQUEST HeldOpen; /* PortGate; uncompleted driver-owned request. */
} LAB_CHANNEL;
static LAB_CHANNEL Channels[OL_MAX_CHANNELS];
typedef struct { LAB_CHANNEL *Channel; } LAB_PORT_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LAB_PORT_REQUEST, GetPortRequest)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD LabUnload;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL LabPortIo;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL LabControl;
EVT_WDF_REQUEST_CANCEL LabCancelRead;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE LabCancelOpen;
EVT_WDF_FILE_CLOSE LabFileClose;

static BOOLEAN LabEnabled(VOID)
{
    DECLARE_CONST_UNICODE_STRING(path, L"\\Registry\\Machine\\SOFTWARE\\OpenLdacKernelLab");
    DECLARE_CONST_UNICODE_STRING(valueName, L"Enabled");
    OBJECT_ATTRIBUTES attributes;
    HANDLE key;
    UCHAR data[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)data;
    ULONG length, enabled = 0;
    NTSTATUS status;
    InitializeObjectAttributes(&attributes, (PUNICODE_STRING)&path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_QUERY_VALUE, &attributes);
    if (!NT_SUCCESS(status)) return FALSE;
    status = ZwQueryValueKey(key, (PUNICODE_STRING)&valueName, KeyValuePartialInformation,
        data, sizeof(data), &length);
    if (NT_SUCCESS(status) && info->Type == REG_DWORD && info->DataLength == sizeof(enabled))
        RtlCopyMemory(&enabled, info->Data, sizeof(enabled));
    ZwClose(key);
    return enabled == 1;
}

/* Same DDI signature as BthReuseBrb, but entirely synthetic. */
_IRQL_requires_same_
static VOID LabReuseBrb(_Inout_ PBRB Brb, _In_ BRB_TYPE Type)
{
    ULONG length = sizeof(BRB_HEADER);
    if (Type == BRB_L2CA_OPEN_CHANNEL) length = sizeof(struct _BRB_L2CA_OPEN_CHANNEL);
    else if (Type == BRB_L2CA_CLOSE_CHANNEL) length = sizeof(struct _BRB_L2CA_CLOSE_CHANNEL);
    else if (Type == BRB_L2CA_ACL_TRANSFER) length = sizeof(struct _BRB_L2CA_ACL_TRANSFER);
    RtlZeroMemory(Brb, length);
    Brb->BrbHeader.Length = length;
    Brb->BrbHeader.Type = (USHORT)Type;
}

/* PortGate serializes terminal provider retirement against other indications.
 * Never invoke more provider callbacks after clearing the channel callback. */
static VOID ReleaseChannel(_Inout_ LAB_CHANNEL *Channel)
{
    PFNBTHPORT_INDICATION_CALLBACK callback = Channel->Callback;
    PVOID context = Channel->Context;
    PDEVICE_OBJECT reference = Channel->Reference;
    INDICATION_PARAMETERS parameters = {0};
    Channel->Callback = NULL; Channel->Context = NULL; Channel->Reference = NULL;
    if (callback != NULL) callback(context, IndicationReleaseReference, &parameters);
    if (reference != NULL) ObDereferenceObject(reference);
}

static LAB_CHANNEL *ChannelFromHandle(L2CAP_CHANNEL_HANDLE Handle)
{
    ULONG_PTR index = (ULONG_PTR)Handle;
    return index && index <= OL_MAX_CHANNELS ? &Channels[index - 1] : NULL;
}

_Use_decl_annotations_
VOID LabCancelRead(WDFREQUEST Request)
{
    LAB_CHANNEL *channel = GetPortRequest(Request)->Channel;
    InterlockedDecrement(&channel->Pending);
    WdfRequestComplete(Request, STATUS_CANCELLED);
}

/* No wait lock in cancellation: this callback may run at DISPATCH_LEVEL.
 * The provider callback reference is deliberately retained independently.
 * Pending keeps the static channel slot unavailable through completion. */
_Use_decl_annotations_
VOID LabCancelOpen(WDFQUEUE Queue, WDFREQUEST Request)
{
    LAB_CHANNEL *channel = GetPortRequest(Request)->Channel;
    UNREFERENCED_PARAMETER(Queue);
    InterlockedIncrement64(&OpenCancelled);
    WdfRequestComplete(Request, STATUS_CANCELLED);
    InterlockedDecrement(&channel->Pending);
    InterlockedDecrement(&PendingOpen);
}

static NTSTATUS CompleteOpen(_In_ BOOLEAN Fail)
{
    WDFREQUEST request = NULL;
    LAB_CHANNEL *channel = NULL;
    NTSTATUS status;
    ULONG i;
    status = WdfWaitLockAcquire(PortGate, NULL);
    if (!NT_SUCCESS(status)) return status;
    for (i = 0; i < OL_MAX_CHANNELS; ++i) {
        if (Channels[i].HeldOpen != NULL) {
            channel = &Channels[i]; request = channel->HeldOpen;
            channel->HeldOpen = NULL;
            break;
        }
    }
    if (request == NULL) {
        status = WdfIoQueueRetrieveNextRequest(PendingOpenQueue, &request);
        if (NT_SUCCESS(status)) channel = GetPortRequest(request)->Channel;
    }
    WdfWaitLockRelease(PortGate);
    if (request == NULL) return status;
    /* Both ownership paths registered this context before deferral. Read it
     * from the now-owned request, rather than relying on correlated branches. */
    channel = GetPortRequest(request)->Channel;
    InterlockedIncrement64(&OpenControlCompleted);
    /* Success can synchronously submit CLOSE. Never hold PortGate here. */
    WdfRequestComplete(request, Fail ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS);
    InterlockedDecrement(&channel->Pending);
    InterlockedDecrement(&PendingOpen);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID LabPortIo(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
               size_t InputLength, ULONG Code)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request));
    PBRB brb = (PBRB)stack->Parameters.Others.Argument1;
    LAB_CHANNEL *channel;
    NTSTATUS status;
    ULONG i;
    UNREFERENCED_PARAMETER(Queue); UNREFERENCED_PARAMETER(OutputLength); UNREFERENCED_PARAMETER(InputLength);
    if (Code != IOCTL_INTERNAL_BTH_SUBMIT_BRB || brb == NULL || brb->BrbHeader.Length < sizeof(BRB_HEADER)) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (brb->BrbHeader.Type == BRB_L2CA_OPEN_CHANNEL &&
        brb->BrbHeader.Length == sizeof(struct _BRB_L2CA_OPEN_CHANNEL)) {
        struct _BRB_L2CA_OPEN_CHANNEL *open = (struct _BRB_L2CA_OPEN_CHANNEL *)brb;
        INDICATION_PARAMETERS parameters = {0};
        status = WdfWaitLockAcquire(PortGate, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        channel = NULL;
        for (i = 0; i < OL_MAX_CHANNELS; ++i)
            if (Channels[i].Callback == NULL && !Channels[i].Pending) { channel = &Channels[i]; break; }
        if (channel == NULL || open->ReferenceObject == NULL || open->Callback == NULL) {
            WdfWaitLockRelease(PortGate); WdfRequestComplete(Request, STATUS_DEVICE_BUSY); return;
        }
        channel->Callback = open->Callback; channel->Context = open->CallbackContext;
        channel->Reference = (PDEVICE_OBJECT)open->ReferenceObject;
        ObReferenceObject(channel->Reference);
        channel->Callback(channel->Context, IndicationAddReference, &parameters);
        open->ChannelHandle = (L2CAP_CHANNEL_HANDLE)(ULONG_PTR)(i + 1);
        open->OutResults.Params.Mtu = OL_MAX_PACKET; open->InResults.Params.Mtu = OL_MAX_PACKET;
        if (HoldOpen) {
            GetPortRequest(Request)->Channel = channel;
            InterlockedIncrement(&channel->Pending);
            InterlockedIncrement(&PendingOpen);
            InterlockedIncrement64(&OpenDeferred);
            if (HoldOpen == 2) {
                channel->HeldOpen = Request;
                WdfWaitLockRelease(PortGate);
                return; /* Explicit completion retains ownership, even on cancel. */
            }
            status = WdfRequestForwardToIoQueue(Request, PendingOpenQueue);
            WdfWaitLockRelease(PortGate);
            if (!NT_SUCCESS(status)) {
                WdfRequestComplete(Request, status);
                InterlockedDecrement(&channel->Pending);
                InterlockedDecrement(&PendingOpen);
            }
            return; /* Framework queue or LabCancelOpen owns the request. */
        }
        status = InterlockedExchange(&FailOpen, 0) ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
        if (!NT_SUCCESS(status) && !KeepCallbacks) ReleaseChannel(channel);
        WdfWaitLockRelease(PortGate);
        /* OPEN completion may synchronously submit CLOSE: no PortGate held. */
        WdfRequestComplete(Request, status); return;
    }
    if (brb->BrbHeader.Type == BRB_L2CA_CLOSE_CHANNEL &&
        brb->BrbHeader.Length == sizeof(struct _BRB_L2CA_CLOSE_CHANNEL)) {
        struct _BRB_L2CA_CLOSE_CHANNEL *close = (struct _BRB_L2CA_CLOSE_CHANNEL *)brb;
        channel = ChannelFromHandle(close->ChannelHandle);
        if (channel == NULL) { WdfRequestComplete(Request, STATUS_INVALID_PARAMETER); return; }
        status = WdfWaitLockAcquire(PortGate, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        status = InterlockedExchange(&FailClose, 0) ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
        /* CLOSE completion itself submits no further port work. Retire before
         * unlocking so a reused channel can never receive a stale release. */
        WdfRequestComplete(Request, status);
        if (!KeepCallbacks) ReleaseChannel(channel);
        WdfWaitLockRelease(PortGate); return;
    }
    if (brb->BrbHeader.Type == BRB_L2CA_ACL_TRANSFER &&
        brb->BrbHeader.Length == sizeof(struct _BRB_L2CA_ACL_TRANSFER)) {
        struct _BRB_L2CA_ACL_TRANSFER *transfer = (struct _BRB_L2CA_ACL_TRANSFER *)brb;
        channel = ChannelFromHandle(transfer->ChannelHandle);
        if (channel == NULL || transfer->Buffer == NULL || transfer->BufferSize > OL_MAX_PACKET) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER); return;
        }
        if ((transfer->TransferFlags & ACL_TRANSFER_DIRECTION_IN) != 0) {
            GetPortRequest(Request)->Channel = channel;
            InterlockedIncrement(&channel->Pending);
            status = WdfRequestMarkCancelableEx(Request, LabCancelRead);
            if (!NT_SUCCESS(status)) {
                InterlockedDecrement(&channel->Pending);
                WdfRequestComplete(Request, status);
            }
            return; /* Actual WDF cancel callback owns a pending read. */
        }
        WdfRequestComplete(Request, STATUS_SUCCESS); return;
    }
    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

_Use_decl_annotations_
VOID LabFileClose(WDFFILEOBJECT File)
{
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file = GetFileContext(File);
    OlFileDetach(File);
    if (file->Counted) {
        file->Counted = FALSE;
        InterlockedDecrement(&GetClientDeviceContext(WdfFileObjectGetDevice(File))->ActiveFiles);
    }
}

_Use_decl_annotations_
VOID LabControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG Code)
{
    ol_lab_request *input;
    NTSTATUS status;
    ULONG i;
    if (Code != OL_LAB_IOCTL) {
        OlEvtDeviceIoControl(Queue, Request, OutputLength, InputLength, Code); return;
    }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*input), (PVOID *)&input, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    if (InputLength != sizeof(*input) || input->size != sizeof(*input) || input->version != OL_LAB_ABI) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER); return;
    }
    if (input->command == OL_LAB_COMPLETE_OPEN) {
        status = input->value > 1 ? STATUS_INVALID_PARAMETER : CompleteOpen(input->value != 0);
        WdfRequestComplete(Request, status); return;
    }
    status = WdfWaitLockAcquire(PortGate, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    switch (input->command) {
    case OL_LAB_HOLD_OPEN:
        if (input->value > 2 || PendingOpen) status = STATUS_INVALID_DEVICE_STATE;
        else InterlockedExchange(&HoldOpen, (LONG)input->value);
        break;
    case OL_LAB_QUERY: {
        ol_lab_status *output;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(output, sizeof(*output));
            output->size = sizeof(*output); output->version = OL_LAB_ABI;
            output->pending_open = (ol_u32)InterlockedCompareExchange(&PendingOpen, 0, 0);
            output->active_files = (ol_u32)InterlockedCompareExchange(&GetClientDeviceContext(LabMain)->ActiveFiles, 0, 0);
            for (i = 0; i < OL_MAX_CHANNELS; ++i)
                if (Channels[i].HeldOpen != NULL && WdfRequestIsCanceled(Channels[i].HeldOpen))
                    ++output->held_cancel_seen;
            output->open_deferred = (ol_u64)InterlockedCompareExchange64(&OpenDeferred, 0, 0);
            output->open_cancelled = (ol_u64)InterlockedCompareExchange64(&OpenCancelled, 0, 0);
            output->open_control_completed = (ol_u64)InterlockedCompareExchange64(&OpenControlCompleted, 0, 0);
            WdfWaitLockRelease(PortGate);
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*output)); return;
        }
        break;
    }
    case OL_LAB_FAIL_OPEN: InterlockedExchange(&FailOpen, input->value ? 1 : 0); break;
    case OL_LAB_FAIL_CLOSE: InterlockedExchange(&FailClose, input->value ? 1 : 0); break;
    case OL_LAB_KEEP_CALLBACKS: InterlockedExchange(&KeepCallbacks, input->value ? 1 : 0); break;
    case OL_LAB_RELEASE_CALLBACKS:
        for (i = 0; i < OL_MAX_CHANNELS; ++i) ReleaseChannel(&Channels[i]);
        break;
    case OL_LAB_REMOTE_DISCONNECT:
        if (input->value > HardwareRemoval) { status = STATUS_INVALID_PARAMETER; break; }
        for (i = 0; i < OL_MAX_CHANNELS; ++i) {
            INDICATION_PARAMETERS p = {0};
            p.Parameters.Disconnect.Reason = (L2CAP_DISCONNECT_REASON)input->value;
            if (Channels[i].Callback != NULL) {
                Channels[i].Callback(Channels[i].Context, IndicationRemoteDisconnect, &p);
                if (p.Parameters.Disconnect.CloseNow && !KeepCallbacks) ReleaseChannel(&Channels[i]);
            }
        }
        break;
    case OL_LAB_REMOVE: OlEvtSurpriseRemoval(LabMain); break;
    case OL_LAB_RESET:
        if (GetClientDeviceContext(LabMain)->Connections != 0) status = STATUS_DEVICE_BUSY;
        else {
            status = WdfIoTargetStart(GetClientDeviceContext(LabMain)->Header.IoTarget);
            if (NT_SUCCESS(status)) InterlockedExchange(&GetClientDeviceContext(LabMain)->Removed, 0);
            InterlockedExchange(&FailOpen, 0); InterlockedExchange(&FailClose, 0); InterlockedExchange(&KeepCallbacks, 0);
            InterlockedExchange(&HoldOpen, 0);
        }
        break;
    default: status = STATUS_INVALID_DEVICE_REQUEST; break;
    }
    WdfWaitLockRelease(PortGate);
    WdfRequestComplete(Request, status);
}

static NTSTATUS CreateControl(WDFDRIVER Driver, BOOLEAN Main)
{
    DECLARE_CONST_UNICODE_STRING(mainName, L"\\Device\\OpenLdacKernelLab");
    DECLARE_CONST_UNICODE_STRING(portName, L"\\Device\\OpenLdacSyntheticPort");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\OpenLdacKernelLab");
    PWDFDEVICE_INIT init;
    WDF_OBJECT_ATTRIBUTES attrs, files, requests;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    NTSTATUS status;
    init = WdfControlDeviceInitAllocate(Driver, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (init == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    WdfDeviceInitSetDeviceType(init, OL_DEVICE_TYPE);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    status = WdfDeviceInitAssignName(init, Main ? &mainName : &portName);
    if (!NT_SUCCESS(status)) { WdfDeviceInitFree(init); return status; }
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.SynchronizationScope = WdfSynchronizationScopeNone;
    if (Main) {
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, BTHECHOSAMPLE_CLIENT_CONTEXT);
        attrs.SynchronizationScope = WdfSynchronizationScopeNone;
        attrs.EvtDestroyCallback = OlEvtDeviceDestroy;
        WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, BthEchoCliEvtDeviceFileCreate, LabFileClose, OlEvtFileCleanup);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&files, BTHECHOSAMPLE_CLIENT_FILE_CONTEXT);
        files.ExecutionLevel = WdfExecutionLevelPassive;
        WdfDeviceInitSetFileObjectConfig(init, &fileConfig, &files);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requests, OL_REQUEST_CONTEXT);
    } else {
        /* This WDF macro expands to two statements. Keep both inside the
         * branch so the main device retains OL_REQUEST_CONTEXT, not the
         * synthetic port's smaller LAB_PORT_REQUEST context. */
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requests, LAB_PORT_REQUEST);
    }
    WdfDeviceInitSetRequestAttributes(init, &requests);
    status = WdfDeviceCreate(&init, &attrs, &device);
    if (!NT_SUCCESS(status)) { if (init != NULL) WdfDeviceInitFree(init); return status; }
    if (Main) LabMain = device; else LabPort = device;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    if (Main) {
        queueConfig.EvtIoDeviceControl = LabControl;
        queueConfig.EvtIoRead = BthEchoCliEvtQueueIoRead;
        queueConfig.EvtIoWrite = BthEchoCliEvtQueueIoWrite;
        queueConfig.EvtIoStop = BthEchoCliEvtQueueIoStop;
    } else queueConfig.EvtIoInternalDeviceControl = LabPortIo;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    attrs.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfIoQueueCreate(device, &queueConfig, &attrs, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    if (!Main) {
        WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
        queueConfig.PowerManaged = WdfFalse;
        queueConfig.EvtIoCanceledOnQueue = LabCancelOpen;
        status = WdfIoQueueCreate(device, &queueConfig, &attrs, &PendingOpenQueue);
        if (!NT_SUCCESS(status)) return status;
    }
    if (Main) {
        PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(device);
        WDF_IO_TARGET_OPEN_PARAMS open;
        dev->Header.Device = device;
        dev->Header.ProfileDrvInterface.BthReuseBrb = LabReuseBrb;
        WDF_OBJECT_ATTRIBUTES_INIT(&attrs); attrs.ParentObject = device;
        status = WdfIoTargetCreate(device, &attrs, &dev->Header.IoTarget);
        if (!NT_SUCCESS(status)) return status;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_EXISTING_DEVICE(&open, WdfDeviceWdmGetDeviceObject(LabPort));
        status = WdfIoTargetOpen(dev->Header.IoTarget, &open);
        if (!NT_SUCCESS(status)) return status;
        WdfDeviceWdmGetDeviceObject(device)->StackSize = WdfDeviceWdmGetDeviceObject(LabPort)->StackSize + 1;
        status = WdfDeviceCreateSymbolicLink(device, &link);
        if (!NT_SUCCESS(status)) return status;
    }
    WdfControlFinishInitializing(device);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID LabUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    if (LabMain != NULL) {
        NT_ASSERT(GetClientDeviceContext(LabMain)->Connections == 0);
        WdfObjectDelete(LabMain); LabMain = NULL;
    }
    if (LabPort != NULL) { WdfObjectDelete(LabPort); LabPort = NULL; }
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    WDF_OBJECT_ATTRIBUTES attrs;
    NTSTATUS status;
    if (!LabEnabled()) return STATUS_ACCESS_DENIED;
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = LabUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs); attrs.ParentObject = driver;
    status = WdfWaitLockCreate(&attrs, &PortGate);
    if (NT_SUCCESS(status)) status = CreateControl(driver, FALSE);
    if (NT_SUCCESS(status)) status = CreateControl(driver, TRUE);
    if (!NT_SUCCESS(status)) LabUnload(driver);
    return status;
}
