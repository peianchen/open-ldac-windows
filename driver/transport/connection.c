/* SPDX-License-Identifier: MS-PL
 * Derived from Microsoft bthecho. Copyright (c) Microsoft Corporation.
 * Lifecycle redesign by Open LDAC Windows contributors, 2026-10-31.
 * See ../LICENSE-MS-PL and ../UPSTREAM.md. */
#include "clisrv.h"
#include "device.h"

VOID OlConnectionEnter(PBTHECHO_CONNECTION c)
{
    int taken;
    WdfSpinLockAcquire(c->ConnectionLock);
    taken = ol_life_take(&c->Life, OL_LEASE_CALL);
    NT_ASSERT(taken);
    UNREFERENCED_PARAMETER(taken);
    WdfSpinLockRelease(c->ConnectionLock);
}

VOID OlConnectionLeave(PBTHECHO_CONNECTION c)
{
    int dispose;
    WdfSpinLockAcquire(c->ConnectionLock);
    (void)ol_life_drop(&c->Life, OL_LEASE_CALL);
    dispose = ol_life_claim_dispose(&c->Life);
    WdfSpinLockRelease(c->ConnectionLock);
    if (dispose) WdfObjectDelete(c->Object); /* Never touch c after this. */
}

_Use_decl_annotations_
VOID OlConnectionCleanup(WDFOBJECT Object)
{
    PBTHECHO_CONNECTION c = GetConnectionObjectContext(Object);
    /* Child requests/memory are deleted by WDF only after every lease retired.
     * There is deliberately no blocking wait or timeout-based forced release. */
    NT_ASSERT(!c->Ready || (c->Life.disposing && ol_life_ready(&c->Life)));
    UNREFERENCED_PARAMETER(c);
}

_Use_decl_annotations_
VOID OlConnectionDestroy(WDFOBJECT Object)
{
    PBTHECHO_CONNECTION c = GetConnectionObjectContext(Object);
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev;
    WDFDEVICE device;
    WDFIOTARGET target;
    PDEVICE_OBJECT wdm;
    BOOLEAN deviceHeld = c->DeviceHeld, targetHeld = c->TargetHeld, wdmHeld = c->WdmHeld;
    if (c->DevCtxHdr == NULL) return; /* Partial WdfObjectCreate failure. */
    device = c->DevCtxHdr->Device;
    target = c->DevCtxHdr->IoTarget;
    dev = GetClientDeviceContext(device);
    wdm = WdfDeviceWdmGetDeviceObject(device);
    if (c->RetainedCounted) InterlockedDecrement(&dev->RetainedConnections);
    if (c->Counted) InterlockedDecrement(&dev->Connections);
    if (targetHeld) WdfObjectDereference(target);
    if (deviceHeld) WdfObjectDereference(device);
    if (wdmHeld) ObDereferenceObject(wdm);
}

NTSTATUS BthEchoConnectionObjectCreate(PBTHECHOSAMPLE_DEVICE_CONTEXT_HEADER Header,
                                      WDFOBJECT UnusedParent, WDFOBJECT *Object)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    PBTHECHO_CONNECTION c;
    WDFOBJECT object;
    NTSTATUS status;
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(Header->Device);
    UNREFERENCED_PARAMETER(UnusedParent);
    if (InterlockedIncrement(&dev->Connections) > (LONG)OL_MAX_CHANNELS) {
        InterlockedDecrement(&dev->Connections);
        return STATUS_DEVICE_BUSY;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, BTHECHO_CONNECTION);
    /* A file close or device cleanup must not delete an uncertain callback root. */
    attributes.ParentObject = WdfGetDriver();
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.EvtCleanupCallback = OlConnectionCleanup;
    attributes.EvtDestroyCallback = OlConnectionDestroy;
    status = WdfObjectCreate(&attributes, &object);
    if (!NT_SUCCESS(status)) { InterlockedDecrement(&dev->Connections); return status; }
    c = GetConnectionObjectContext(object);
    c->Object = object; c->DevCtxHdr = Header; c->Counted = TRUE;
    ol_life_init(&c->Life);
    WdfObjectReference(Header->Device); c->DeviceHeld = TRUE;
    WdfObjectReference(Header->IoTarget); c->TargetHeld = TRUE;
    ObReferenceObject(WdfDeviceWdmGetDeviceObject(Header->Device)); c->WdmHeld = TRUE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = object;
    status = WdfSpinLockCreate(&attributes, &c->ConnectionLock);
    if (!NT_SUCCESS(status)) goto failed;
    status = WdfRequestCreate(&attributes, Header->IoTarget, &c->CloseRequest);
    if (!NT_SUCCESS(status)) goto failed;
    /* Reserve close descriptor storage BEFORE opening any Bluetooth channel. */
    status = WdfMemoryCreatePreallocated(&attributes, &c->CloseBrb,
                                          sizeof(c->CloseBrb), &c->CloseMemory);
    if (!NT_SUCCESS(status)) goto failed;
    c->Ready = TRUE;
    *Object = object;
    return STATUS_SUCCESS;
failed:
    WdfObjectDelete(object); /* No BRB or provider callback was registered. */
    return status;
}

PBTHECHO_CONNECTION OlFileBorrow(WDFFILEOBJECT File)
{
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file = GetFileContext(File);
    PBTHECHO_CONNECTION c = NULL;
    WdfSpinLockAcquire(file->Lock);
    if (!file->Closing && file->Connection != NULL) {
        c = file->Connection;
        WdfSpinLockAcquire(c->ConnectionLock);
        {
            int taken = ol_life_take(&c->Life, OL_LEASE_CALL);
            WdfSpinLockRelease(c->ConnectionLock);
            if (!taken) c = NULL;
        }
    }
    WdfSpinLockRelease(file->Lock);
    return c;
}

NTSTATUS OlFileAttach(WDFFILEOBJECT File, PBTHECHO_CONNECTION c)
{
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file = GetFileContext(File);
    NTSTATUS status = STATUS_CANCELLED;
    WdfSpinLockAcquire(file->Lock);
    if (!file->Closing && file->Connection == NULL) {
        WdfSpinLockAcquire(c->ConnectionLock);
        if (ol_life_take(&c->Life, OL_LEASE_CALL)) {
            file->Connection = c;
            status = STATUS_SUCCESS; /* Returns with a dispatch lease. */
        }
        WdfSpinLockRelease(c->ConnectionLock);
    }
    WdfSpinLockRelease(file->Lock);
    return status;
}

VOID OlConnectionCancelTransfers(PBTHECHO_CONNECTION c)
{
    WDFREQUEST requests[OL_MAX_INFLIGHT + 1];
    ULONG count = 0, i;
    WdfSpinLockAcquire(c->ConnectionLock);
    if (c->OpenRequest != NULL) {
        WDFREQUEST request = c->OpenRequest;
        InterlockedExchange(&GetOlRequestContext(request)->CancelWanted, 1);
        WdfObjectReference(request);
        requests[count++] = request;
    }
    for (i = 0; i < OL_MAX_INFLIGHT; ++i) {
        if (c->Transfers[i] != NULL) {
            WDFREQUEST request = c->Transfers[i];
            InterlockedExchange(&GetOlRequestContext(request)->CancelWanted, 1);
            WdfObjectReference(request); /* Complete/cancel may race after unlock. */
            requests[count++] = request;
        }
    }
    WdfSpinLockRelease(c->ConnectionLock);
    for (i = 0; i < count; ++i) {
        (void)WdfRequestCancelSentRequest(requests[i]);
        WdfObjectDereference(requests[i]);
    }
}

_Use_decl_annotations_
VOID OlCloseComplete(WDFREQUEST Request, WDFIOTARGET Target,
                     PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    PBTHECHO_CONNECTION c = (PBTHECHO_CONNECTION)Context;
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target);
    OlConnectionEnter(c); /* Close-pending lease protects this entry. */
    WdfSpinLockAcquire(c->ConnectionLock);
    (void)ol_life_close_end(&c->Life, NT_SUCCESS(Params->IoStatus.Status));
    WdfSpinLockRelease(c->ConnectionLock);
    /* Success/failure does not impersonate IndicationReleaseReference. */
    OlConnectionLeave(c);
}

VOID OlConnectionClose(PBTHECHO_CONNECTION c)
{
    NTSTATUS status;
    WDF_REQUEST_REUSE_PARAMS reuse;
    OlConnectionEnter(c);
    WdfSpinLockAcquire(c->ConnectionLock);
    ol_life_close_wanted(&c->Life);
    WdfSpinLockRelease(c->ConnectionLock);
    /* At most two immediate attempts. No timer, polling or unbounded retry. */
    for (;;) {
        int begin;
        WdfSpinLockAcquire(c->ConnectionLock);
        begin = ol_life_close_begin(&c->Life);
        WdfSpinLockRelease(c->ConnectionLock);
        if (!begin) break;
        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
        status = WdfRequestReuse(c->CloseRequest, &reuse);
        if (NT_SUCCESS(status)) {
            c->DevCtxHdr->ProfileDrvInterface.BthReuseBrb((PBRB)&c->CloseBrb, BRB_L2CA_CLOSE_CHANNEL);
            c->CloseBrb.BtAddress = c->RemoteAddress;
            c->CloseBrb.ChannelHandle = c->ChannelHandle;
            status = WdfIoTargetFormatRequestForInternalIoctlOthers(c->DevCtxHdr->IoTarget,
                c->CloseRequest, IOCTL_INTERNAL_BTH_SUBMIT_BRB, c->CloseMemory,
                NULL, NULL, NULL, NULL, NULL);
        }
        if (NT_SUCCESS(status)) {
            WdfRequestSetCompletionRoutine(c->CloseRequest, OlCloseComplete, c);
            if (WdfRequestSend(c->CloseRequest, c->DevCtxHdr->IoTarget, NULL)) break;
            status = WdfRequestGetStatus(c->CloseRequest);
        }
        UNREFERENCED_PARAMETER(status);
        WdfSpinLockAcquire(c->ConnectionLock);
        (void)ol_life_close_not_sent(&c->Life);
        WdfSpinLockRelease(c->ConnectionLock);
    }
    OlConnectionLeave(c);
}

VOID OlConnectionDropOwner(PBTHECHO_CONNECTION c)
{
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(c->DevCtxHdr->Device);
    OlConnectionEnter(c);
    WdfSpinLockAcquire(c->ConnectionLock);
    ol_life_detach(&c->Life);
    if (!c->RetainedCounted) {
        c->RetainedCounted = TRUE;
        InterlockedIncrement(&dev->RetainedConnections);
    }
    WdfSpinLockRelease(c->ConnectionLock);
    OlConnectionCancelTransfers(c);
    OlConnectionClose(c);
    OlConnectionLeave(c);
}

VOID OlFileDetach(WDFFILEOBJECT File)
{
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file = GetFileContext(File);
    PBTHECHO_CONNECTION c;
    if (file->Lock == NULL) return; /* Failed file create. */
    WdfSpinLockAcquire(file->Lock);
    file->Closing = 1;
    c = file->Connection;
    if (c != NULL) OlConnectionEnter(c); /* Owner lease still held under file lock. */
    file->Connection = NULL;
    WdfSpinLockRelease(file->Lock);
    if (c != NULL) {
        OlConnectionDropOwner(c);
        OlConnectionLeave(c);
    }
}

NTSTATUS FormatRequestWithBrb(WDFIOTARGET Target, WDFREQUEST Request, PBRB Brb, size_t Size)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFMEMORY memory;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Request;
    status = WdfMemoryCreatePreallocated(&attributes, Brb, Size, &memory);
    if (!NT_SUCCESS(status)) return status;
    return WdfIoTargetFormatRequestForInternalIoctlOthers(Target, Request,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB, memory, NULL, NULL, NULL, NULL, NULL);
}

NTSTATUS BthEchoConnectionObjectFormatRequestForL2CaTransfer(PBTHECHO_CONNECTION c,
    WDFREQUEST Request, struct _BRB_L2CA_ACL_TRANSFER **Brb, WDFMEMORY Memory, ULONG Flags)
{
    size_t size;
    struct _BRB_L2CA_ACL_TRANSFER *brb = *Brb;
    if (brb == NULL) return STATUS_INVALID_PARAMETER;
    c->DevCtxHdr->ProfileDrvInterface.BthReuseBrb((PBRB)brb, BRB_L2CA_ACL_TRANSFER);
    brb->Buffer = WdfMemoryGetBuffer(Memory, &size);
    if (!size || size > OL_MAX_PACKET) return STATUS_INVALID_BUFFER_SIZE;
    brb->BtAddress = c->RemoteAddress;
    brb->ChannelHandle = c->ChannelHandle;
    brb->BufferSize = (ULONG)size;
    brb->BufferMDL = NULL;
    brb->TransferFlags = Flags;
    return FormatRequestWithBrb(c->DevCtxHdr->IoTarget, Request, (PBRB)brb, sizeof(*brb));
}
