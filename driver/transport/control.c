/* SPDX-License-Identifier: MS-PL
 * Open LDAC Windows control layer for the Microsoft-derived transport.
 * See ../LICENSE-MS-PL and ../UPSTREAM.md. */
#include "clisrv.h"
#include "device.h"

C_ASSERT(sizeof(ol_native_request) == 8);
C_ASSERT(sizeof(ol_native_status) == 64);
C_ASSERT(sizeof(ol_native_diagnostics) == 128);

VOID OlRecordEvent(PBTHECHOSAMPLE_CLIENT_CONTEXT Device, enum ol_diag_event Event)
{
    NT_ASSERT(Event < OL_EVENT_COUNT);
    InterlockedIncrement64(&Device->Events[Event]);
}

VOID OlCancelRequest(PBTHECHOSAMPLE_CLIENT_CONTEXT Device, WDFREQUEST Request)
{
    OlRecordEvent(Device, OL_EVENT_CANCEL_CALL);
    (void)WdfRequestCancelSentRequest(Request);
}

VOID OlMarkRemoved(WDFDEVICE Device)
{
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(Device);
    /* Exactly one caller stops the local target. LeaveSentIoPending never waits
     * in a Bluetooth callback; framework removal retires already sent I/O.
     * A concurrent Send can queue, but cannot be delivered after target stop. */
    if (!InterlockedCompareExchange(&dev->Removed, 1, 0)) {
        OlRecordEvent(dev, OL_EVENT_REMOVAL);
        if (dev->Header.IoTarget != NULL)
            WdfIoTargetStop(dev->Header.IoTarget, WdfIoTargetLeaveSentIoPending);
    }
}

_Use_decl_annotations_
VOID OlEvtSurpriseRemoval(WDFDEVICE Device)
{
    /* Framework forwards removal; never submit CLOSE from this notification. */
    OlMarkRemoved(Device);
}

VOID BthEchoCliEvtDeviceFileCreate(WDFDEVICE Device, WDFREQUEST Request,
                                 WDFFILEOBJECT FileObject)
{
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(Device);
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file = GetFileContext(FileObject);
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = FileObject;
    status = WdfSpinLockCreate(&attributes, &file->Lock);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    if (InterlockedIncrement(&dev->ActiveFiles) > (LONG)OL_MAX_CHANNELS) {
        InterlockedDecrement(&dev->ActiveFiles);
        WdfRequestComplete(Request, STATUS_TOO_MANY_OPENED_FILES);
        return;
    }
    file->Counted = TRUE;
    /* Deliberately no SDP or RF operation on an ordinary file open. */
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID OlEvtFileCleanup(WDFFILEOBJECT FileObject)
{
    OlFileDetach(FileObject);
}

VOID OlEvtDeviceDestroy(WDFOBJECT Device)
{
    PINTERFACE interface = &GetClientDeviceContext(Device)->Header.ProfileDrvInterface.Interface;
    if (interface->InterfaceDereference != NULL) {
        interface->InterfaceDereference(interface->Context);
        interface->InterfaceDereference = NULL;
    }
}

VOID OlEvtDeviceIoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                          size_t InputLength, ULONG IoControlCode)
{
    NTSTATUS status;
    ol_native_request *input;
    ol_native_status *output;
    WDFFILEOBJECT object = WdfRequestGetFileObject(Request);
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file;
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(WdfIoQueueGetDevice(Queue));
    if (object == NULL || InputLength != sizeof(*input)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*input), (PVOID *)&input, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    if (input->size != sizeof(*input) || input->version != OL_NATIVE_ABI) {
        WdfRequestComplete(Request, STATUS_REVISION_MISMATCH);
        return;
    }
    file = GetFileContext(object);
    if (IoControlCode == OL_IOCTL_DIAGNOSTICS) {
        ol_native_diagnostics *diagnostics;
        ULONG event;
        if (OutputLength < sizeof(*diagnostics)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*diagnostics), (PVOID *)&diagnostics, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        RtlZeroMemory(diagnostics, sizeof(*diagnostics));
        diagnostics->size = sizeof(*diagnostics);
        diagnostics->version = OL_DIAGNOSTICS_ABI;
        diagnostics->count = OL_EVENT_COUNT;
        if (InterlockedCompareExchange(&dev->Removed, 0, 0)) diagnostics->flags = OL_DIAG_REMOVED;
        for (event = 0; event < OL_EVENT_COUNT; ++event)
            diagnostics->events[event] = (ol_u64)InterlockedCompareExchange64(&dev->Events[event], 0, 0);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*diagnostics));
        return;
    }
    if (IoControlCode == OL_IOCTL_QUERY) {
        if (OutputLength < sizeof(*output)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        RtlZeroMemory(output, sizeof(*output));
        output->size = sizeof(*output);
        output->version = OL_NATIVE_ABI;
        output->flags = OL_FLAG_TRANSPORT_ONLY | OL_FLAG_ENCRYPTION_REQUIRED;
        output->max_packet = OL_MAX_PACKET;
        output->max_channels = OL_MAX_CHANNELS;
        output->max_in_flight = OL_MAX_INFLIGHT;
        output->in_flight = (ol_u32)InterlockedCompareExchange(&dev->InFlight, 0, 0);
        output->bytes_sent = (ol_u64)InterlockedCompareExchange64(&dev->BytesSent, 0, 0);
        output->bytes_received = (ol_u64)InterlockedCompareExchange64(&dev->BytesReceived, 0, 0);
        output->connections = (ol_u32)InterlockedCompareExchange(&dev->Connections, 0, 0);
        output->retained_connections = (ol_u32)InterlockedCompareExchange(&dev->RetainedConnections, 0, 0);
        if (output->retained_connections) output->flags |= OL_FLAG_RETAINED_CONTEXT;
        {
            PBTHECHO_CONNECTION c = OlFileBorrow(object);
            if (c != NULL) {
                WdfSpinLockAcquire(c->ConnectionLock);
                output->connection_state = c->Life.retired ? OL_STATE_RETIRED :
                    c->Life.close_wanted ? OL_STATE_CLOSING :
                    c->Life.opening ? OL_STATE_OPENING :
                    c->Life.opened ? OL_STATE_OPEN : OL_STATE_IDLE;
                output->outbound_mtu = c->OutMTU;
                output->inbound_mtu = c->InMTU;
                WdfSpinLockRelease(c->ConnectionLock);
                OlConnectionLeave(c);
            }
        }
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*output));
        return;
    }
    if (IoControlCode == OL_IOCTL_CONNECT) {
        if (OutputLength != 0 || InterlockedCompareExchange(&file->Closing, 0, 0) ||
            InterlockedCompareExchange(&file->ConnectIssued, 1, 0)) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            return;
        }
        /* Bound XM6 Audio Sink address and fixed AVDTP PSM only: no arbitrary PSM IOCTL. */
        OlConnectChannel(WdfIoQueueGetDevice(Queue), Request, object);
        return;
    }
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

NTSTATUS OlBeginTransfer(WDFREQUEST Request, size_t Length, ULONG Direction)
{
    WDFFILEOBJECT object = WdfRequestGetFileObject(Request);
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev;
    PBTHECHO_CONNECTION c;
    OL_REQUEST_CONTEXT *request = GetOlRequestContext(Request);
    NTSTATUS status = STATUS_CONNECTION_DISCONNECTED;
    USHORT mtu;
    ULONG slot;
    if (object == NULL || Length == 0 || Length > OL_MAX_PACKET)
        return STATUS_INVALID_BUFFER_SIZE;
    c = OlFileBorrow(object);
    if (c == NULL) return STATUS_INVALID_DEVICE_STATE;
    dev = GetClientDeviceContext(c->DevCtxHdr->Device);
    WdfSpinLockAcquire(c->ConnectionLock);
    if (InterlockedCompareExchange(&dev->Removed, 0, 0)) goto done;
    if (!ol_life_can_io(&c->Life)) goto done;
    mtu = Direction == ACL_TRANSFER_DIRECTION_OUT ? c->OutMTU : c->InMTU;
    if ((Direction == ACL_TRANSFER_DIRECTION_OUT && Length > mtu) ||
        (Direction == ACL_TRANSFER_DIRECTION_IN && Length < mtu)) {
        status = STATUS_INVALID_BUFFER_SIZE; goto done;
    }
    status = STATUS_DEVICE_BUSY;
    if (InterlockedIncrement(&dev->InFlight) > (LONG)OL_MAX_INFLIGHT) {
        InterlockedDecrement(&dev->InFlight); goto done;
    }
    for (slot = 0; slot < OL_MAX_INFLIGHT && c->Transfers[slot] != NULL; ++slot) {}
    if (slot == OL_MAX_INFLIGHT || !ol_life_take(&c->Life, OL_LEASE_IO)) {
        InterlockedDecrement(&dev->InFlight); goto done;
    }
    request->Connection = c;
    request->Slot = slot;
    request->Direction = Direction;
    InterlockedExchange(&request->CancelWanted, 0);
    c->Transfers[slot] = Request;
    InterlockedExchange(&request->TransferActive, 1);
    status = STATUS_SUCCESS;
done:
    WdfSpinLockRelease(c->ConnectionLock);
    OlConnectionLeave(c);
    return status;
}

VOID OlFinishTransfer(WDFREQUEST Request, NTSTATUS Status, size_t Bytes)
{
    OL_REQUEST_CONTEXT *request = GetOlRequestContext(Request);
    if (InterlockedExchange(&request->TransferActive, 0)) {
        PBTHECHO_CONNECTION c = request->Connection;
        PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(c->DevCtxHdr->Device);
        OlConnectionEnter(c); /* I/O lease protects entry until the slot is cleared. */
        WdfSpinLockAcquire(c->ConnectionLock);
        NT_ASSERT(request->Slot < OL_MAX_INFLIGHT && c->Transfers[request->Slot] == Request);
        c->Transfers[request->Slot] = NULL;
        (void)ol_life_drop(&c->Life, OL_LEASE_IO);
        WdfSpinLockRelease(c->ConnectionLock);
        request->Connection = NULL;
        OlRecordEvent(dev, OL_EVENT_TRANSFER_COMPLETE);
        if (NT_SUCCESS(Status)) {
            if (request->Direction == ACL_TRANSFER_DIRECTION_OUT)
                InterlockedExchangeAdd64(&dev->BytesSent, (LONG64)Bytes);
            else InterlockedExchangeAdd64(&dev->BytesReceived, (LONG64)Bytes);
        }
        InterlockedDecrement(&dev->InFlight);
        OlConnectionLeave(c);
    }
}
