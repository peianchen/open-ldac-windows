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
    BOOLEAN releaseDevice = FALSE;
    UNREFERENCED_PARAMETER(Parameters);
    OlConnectionEnter(c);
    if (Indication == IndicationAddReference) {
        WdfSpinLockAcquire(c->ConnectionLock);
        if (ol_life_take(&c->Life, OL_LEASE_CALLBACK)) ObReferenceObject(device);
        WdfSpinLockRelease(c->ConnectionLock);
    } else if (Indication == IndicationReleaseReference) {
        WdfSpinLockAcquire(c->ConnectionLock);
        releaseDevice = ol_life_drop(&c->Life, OL_LEASE_CALLBACK) ? TRUE : FALSE;
        WdfSpinLockRelease(c->ConnectionLock);
    } else if (Indication == IndicationRemoteDisconnect) {
        WdfSpinLockAcquire(c->ConnectionLock);
        ol_life_close_wanted(&c->Life);
        WdfSpinLockRelease(c->ConnectionLock);
        OlConnectionCancelTransfers(c);
        OlConnectionClose(c);
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
    WdfRequestComplete(Request, status);
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
    if (WdfRequestIsCanceled(Request)) {
        WdfRequestComplete(Request, STATUS_CANCELLED); return;
    }
    status = BthEchoConnectionObjectCreate(&dev->Header, NULL, &object);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    c = GetConnectionObjectContext(object);
    status = OlFileAttach(FileObject, c);
    if (!NT_SUCCESS(status)) {
        OlConnectionDropOwner(c);
        WdfRequestComplete(Request, status); return;
    }
    WdfObjectReference(Request);
    WdfSpinLockAcquire(c->ConnectionLock);
    began = !WdfRequestIsCanceled(Request) && ol_life_open_begin(&c->Life);
    if (began) c->OpenRequest = Request;
    WdfSpinLockRelease(c->ConnectionLock);
    if (!began) {
        WdfRequestComplete(Request, STATUS_CANCELLED);
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
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(Request, OlOpenComplete, brb);
        if (!WdfRequestSend(Request, dev->Header.IoTarget, NULL))
            status = WdfRequestGetStatus(Request);
        else if (InterlockedCompareExchange(&GetOlRequestContext(Request)->CancelWanted, 0, 0))
            (void)WdfRequestCancelSentRequest(Request);
    }
    if (!NT_SUCCESS(status)) {
        WdfSpinLockAcquire(c->ConnectionLock);
        c->OpenRequest = NULL;
        (void)ol_life_open_end(&c->Life, 0);
        WdfSpinLockRelease(c->ConnectionLock);
        WdfRequestComplete(Request, status);
    }
    OlConnectionLeave(c); /* Protects inline completion until Send returned. */
    WdfObjectDereference(Request);
}
