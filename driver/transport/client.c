/* SPDX-License-Identifier: MS-PL
 * Derived from Microsoft bthecho. Copyright (c) Microsoft Corporation.
 * Lifecycle redesign by Open LDAC Windows contributors, 2026-10-31.
 * See ../LICENSE-MS-PL and ../UPSTREAM.md. */
#include "clisrv.h"
#include "device.h"
#include "client.h"

_Use_decl_annotations_
NTSTATUS BthEchoCliBthQueryInterfaces(PBTHECHOSAMPLE_CLIENT_CONTEXT DevCtx)
{
    return WdfFdoQueryForInterface(DevCtx->Header.Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
        (PINTERFACE)&DevCtx->Header.ProfileDrvInterface, sizeof(DevCtx->Header.ProfileDrvInterface),
        BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
}

_Use_decl_annotations_
NTSTATUS BthEchoCliRetrieveServerBthAddress(PBTHECHOSAMPLE_CLIENT_CONTEXT DevCtx)
{
    BTH_DEVICE_INFO info = {0};
    WDF_MEMORY_DESCRIPTOR memory;
    NTSTATUS status;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, &info, sizeof(info));
    status = WdfIoTargetSendInternalIoctlSynchronously(DevCtx->Header.IoTarget, NULL,
        IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &memory, NULL, NULL);
    if (NT_SUCCESS(status)) DevCtx->ServerBthAddress = info.address;
    return status;
}

VOID BthEchoCliIndicationCallback(PVOID Context, INDICATION_CODE Indication,
                                 PINDICATION_PARAMETERS Parameters)
{
    PBTHECHO_CONNECTION c = (PBTHECHO_CONNECTION)Context;
    PDEVICE_OBJECT device = WdfDeviceWdmGetDeviceObject(c->DevCtxHdr->Device);
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(c->DevCtxHdr->Device);
    BOOLEAN releaseDevice = FALSE;
    OlConnectionEnter(c);
    if (Indication == IndicationAddReference) {
        OlRecordEvent(dev, OL_EVENT_CALLBACK_ADD);
        WdfSpinLockAcquire(c->ConnectionLock);
        if (ol_life_take(&c->Life, OL_LEASE_CALLBACK)) ObReferenceObject(device);
        else OlRecordEvent(dev, OL_EVENT_CONTRACT_FAULT);
        WdfSpinLockRelease(c->ConnectionLock);
    } else if (Indication == IndicationReleaseReference) {
        OlRecordEvent(dev, OL_EVENT_CALLBACK_RELEASE);
        WdfSpinLockAcquire(c->ConnectionLock);
        releaseDevice = ol_life_drop(&c->Life, OL_LEASE_CALLBACK) ? TRUE : FALSE;
        if (!releaseDevice) OlRecordEvent(dev, OL_EVENT_CONTRACT_FAULT);
        WdfSpinLockRelease(c->ConnectionLock);
    } else if (Indication == IndicationRemoteDisconnect) {
        OlRecordEvent(dev, OL_EVENT_REMOTE_DISCONNECT);
        /* Do not send CLOSE after surprise removal. CloseNow is the documented
         * provider-owned shutdown response for every remote disconnect reason. */
        if (Parameters != NULL) {
            Parameters->Parameters.Disconnect.CloseNow = TRUE;
            if (Parameters->Parameters.Disconnect.Reason == HardwareRemoval)
                OlMarkRemoved(c->DevCtxHdr->Device);
        }
        WdfSpinLockAcquire(c->ConnectionLock);
        ol_life_remote_disconnect(&c->Life);
        WdfSpinLockRelease(c->ConnectionLock);
        OlConnectionCancelTransfers(c);
    }
    OlConnectionLeave(c);
    /* No access to c after dropping its final callback/dispatch lease. */
    if (releaseDevice) ObDereferenceObject(device);
}

EVT_WDF_REQUEST_COMPLETION_ROUTINE OlOpenComplete;
_Use_decl_annotations_
VOID OlOpenComplete(WDFREQUEST Request, WDFIOTARGET Target,
                    PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    struct _BRB_L2CA_OPEN_CHANNEL *brb = (struct _BRB_L2CA_OPEN_CHANNEL *)Context;
    PBTHECHO_CONNECTION c = (PBTHECHO_CONNECTION)brb->Hdr.ClientContext[0];
    NTSTATUS status = Params->IoStatus.Status;
    int mustClose;
    UNREFERENCED_PARAMETER(Target);
    OlConnectionEnter(c); /* Opening lease protects entry even after file cleanup. */
    OlRecordEvent(GetClientDeviceContext(c->DevCtxHdr->Device), OL_EVENT_OPEN_COMPLETE);
    if (!NT_SUCCESS(status)) OlRecordEvent(GetClientDeviceContext(c->DevCtxHdr->Device), OL_EVENT_OPEN_FAILURE);
    WdfSpinLockAcquire(c->ConnectionLock);
    if (NT_SUCCESS(status)) {
        c->OutMTU = brb->OutResults.Params.Mtu;
        c->InMTU = brb->InResults.Params.Mtu;
        c->ChannelHandle = brb->ChannelHandle;
        c->RemoteAddress = brb->BtAddress;
    }
    c->OpenRequest = NULL;
    (void)ol_life_open_end(&c->Life, NT_SUCCESS(status));
    if (WdfRequestIsCanceled(Request)) ol_life_close_wanted(&c->Life);
    mustClose = NT_SUCCESS(status) && (c->Life.close_wanted || !c->Life.owner);
    WdfSpinLockRelease(c->ConnectionLock);
    if (mustClose) {
        OlConnectionClose(c);
        status = STATUS_CANCELLED;
    }
    /* CONNECT has no output. The reused request can retain the lower BRB's
     * byte count (observed as 368); never expose it as public IOCTL data. */
    WdfRequestCompleteWithInformation(Request, status, 0);
    OlConnectionLeave(c);
}

VOID OlConnectChannel(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                      _In_ WDFFILEOBJECT FileObject)
{
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(Device);
    PBTHECHO_CONNECTION c;
    WDFOBJECT object;
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    NTSTATUS status;
    int began;
    if (InterlockedCompareExchange(&dev->Removed, 0, 0)) {
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_CONNECTED, 0); return;
    }
    if (WdfRequestIsCanceled(Request)) {
        WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0); return;
    }
    status = BthEchoConnectionObjectCreate(&dev->Header, NULL, &object);
    if (!NT_SUCCESS(status)) { WdfRequestCompleteWithInformation(Request, status, 0); return; }
    c = GetConnectionObjectContext(object);
    status = OlFileAttach(FileObject, c);
    if (!NT_SUCCESS(status)) {
        OlConnectionDropOwner(c);
        WdfRequestCompleteWithInformation(Request, status, 0); return;
    }
    WdfObjectReference(Request);
    WdfSpinLockAcquire(c->ConnectionLock);
    began = !WdfRequestIsCanceled(Request) && ol_life_open_begin(&c->Life);
    if (began) c->OpenRequest = Request;
    WdfSpinLockRelease(c->ConnectionLock);
    if (!began) {
        WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0);
        OlConnectionLeave(c);
        WdfObjectDereference(Request); return;
    }
    brb = (struct _BRB_L2CA_OPEN_CHANNEL *)GetRequestContext(Request);
    dev->Header.ProfileDrvInterface.BthReuseBrb((PBRB)brb, BRB_L2CA_OPEN_CHANNEL);
    brb->Hdr.ClientContext[0] = c;
    brb->BtAddress = dev->ServerBthAddress;
    /* AVDTP has a fixed assigned PSM. The PDO/INF is the XM6 Audio Sink service.
     * Avoid uncancelable synchronous per-open SDP requests and arbitrary PSMs. */
    brb->Psm = 0x0019;
    brb->ChannelFlags = CF_ROLE_EITHER | CF_LINK_ENCRYPTED;
    brb->ConfigOut.Flags = CFG_MTU;
    brb->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    brb->ConfigOut.Mtu.Max = OL_MAX_PACKET;
    brb->ConfigOut.Mtu.Preferred = OL_MAX_PACKET;
    brb->ConfigIn.Flags = CFG_MTU;
    brb->ConfigIn.Mtu.Min = L2CAP_MIN_MTU;
    brb->ConfigIn.Mtu.Max = OL_MAX_PACKET;
    brb->ConfigIn.Mtu.Preferred = OL_MAX_PACKET;
    brb->IncomingQueueDepth = 10;
    brb->CallbackFlags = CALLBACK_DISCONNECT;
    brb->Callback = BthEchoCliIndicationCallback;
    brb->CallbackContext = c;
    brb->ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(Device);
    status = FormatRequestWithBrb(dev->Header.IoTarget, Request, (PBRB)brb, sizeof(*brb));
    if (NT_SUCCESS(status) && (WdfRequestIsCanceled(Request) ||
        InterlockedCompareExchange(&GetOlRequestContext(Request)->CancelWanted, 0, 0)))
        status = STATUS_CANCELLED;
    if (NT_SUCCESS(status) && InterlockedCompareExchange(&dev->Removed, 0, 0))
        status = STATUS_DEVICE_NOT_CONNECTED;
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(Request, OlOpenComplete, brb);
        OlRecordEvent(dev, OL_EVENT_OPEN_SEND);
        if (!WdfRequestSend(Request, dev->Header.IoTarget, NULL))
            status = WdfRequestGetStatus(Request);
        else if (InterlockedCompareExchange(&GetOlRequestContext(Request)->CancelWanted, 0, 0) ||
                 InterlockedCompareExchange(&dev->Removed, 0, 0))
            OlCancelRequest(dev, Request);
        OL_OBSERVE_RESULT(OL_RES_OPEN_SEND, status);
    }
    if (!NT_SUCCESS(status)) {
        OlRecordEvent(dev, OL_EVENT_OPEN_FAILURE);
        WdfSpinLockAcquire(c->ConnectionLock);
        c->OpenRequest = NULL;
        (void)ol_life_open_end(&c->Life, 0);
        WdfSpinLockRelease(c->ConnectionLock);
        WdfRequestCompleteWithInformation(Request, status, 0);
    }
    OlConnectionLeave(c); /* Protects inline completion until Send returned. */
    WdfObjectDereference(Request);
}
