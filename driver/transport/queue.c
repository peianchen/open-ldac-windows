/* SPDX-License-Identifier: MS-PL
 * Derived from Microsoft bthecho. Copyright (c) Microsoft Corporation.
 * Lifecycle redesign by Open LDAC Windows contributors, 2026-10-31.
 * See ../LICENSE-MS-PL and ../UPSTREAM.md. */
#include "clisrv.h"
#include "device.h"
#include "queue.h"

_Use_decl_annotations_
VOID BthEchoCliReadWriteCompletion(WDFREQUEST Request, WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    struct _BRB_L2CA_ACL_TRANSFER *brb = (struct _BRB_L2CA_ACL_TRANSFER *)Context;
    size_t bytes = NT_SUCCESS(Params->IoStatus.Status) ? brb->BufferSize : 0;
    UNREFERENCED_PARAMETER(Target);
    OlFinishTransfer(Request, Params->IoStatus.Status, bytes);
    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status, bytes);
}

static VOID Transfer(WDFREQUEST Request, size_t Length, ULONG Direction)
{
    NTSTATUS status;
    WDFMEMORY memory;
    OL_REQUEST_CONTEXT *context;
    PBTHECHO_CONNECTION c;
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev;
    struct _BRB_L2CA_ACL_TRANSFER *brb;
    /* Keeps request context valid across inline completion and post-send cancel. */
    WdfObjectReference(Request);
    status = OlBeginTransfer(Request, Length, Direction);
    if (!NT_SUCCESS(status)) goto failed;
    context = GetOlRequestContext(Request);
    c = context->Connection;
    dev = GetClientDeviceContext(c->DevCtxHdr->Device);
    status = Direction == ACL_TRANSFER_DIRECTION_OUT
        ? WdfRequestRetrieveInputMemory(Request, &memory)
        : WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) goto failed;
    brb = (struct _BRB_L2CA_ACL_TRANSFER *)GetRequestContext(Request);
    status = BthEchoConnectionObjectFormatRequestForL2CaTransfer(c, Request, &brb, memory,
        Direction == ACL_TRANSFER_DIRECTION_IN ? Direction | ACL_SHORT_TRANSFER_OK : Direction);
    if (!NT_SUCCESS(status)) goto failed;
    if (InterlockedCompareExchange(&context->CancelWanted, 0, 0) || WdfRequestIsCanceled(Request)) {
        status = STATUS_CANCELLED; goto failed;
    }
    if (InterlockedCompareExchange(&dev->Removed, 0, 0)) {
        status = STATUS_DEVICE_NOT_CONNECTED; goto failed;
    }
    WdfRequestSetCompletionRoutine(Request, BthEchoCliReadWriteCompletion, brb);
    if (!WdfRequestSend(Request, c->DevCtxHdr->IoTarget, NULL)) {
        status = WdfRequestGetStatus(Request); goto failed;
    }
    /* Cleanup can race the check above while the request was not sent yet. */
    if (InterlockedCompareExchange(&context->CancelWanted, 0, 0) ||
        InterlockedCompareExchange(&dev->Removed, 0, 0))
        OlCancelRequest(dev, Request);
    WdfObjectDereference(Request);
    return;
failed:
    OlFinishTransfer(Request, status, 0);
    WdfRequestComplete(Request, status);
    WdfObjectDereference(Request);
}

_Use_decl_annotations_
VOID BthEchoCliEvtQueueIoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
    UNREFERENCED_PARAMETER(Queue);
    Transfer(Request, Length, ACL_TRANSFER_DIRECTION_OUT);
}
_Use_decl_annotations_
VOID BthEchoCliEvtQueueIoRead(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
    UNREFERENCED_PARAMETER(Queue);
    Transfer(Request, Length, ACL_TRANSFER_DIRECTION_IN);
}
_Use_decl_annotations_
VOID BthEchoCliEvtQueueIoStop(WDFQUEUE Queue, WDFREQUEST Request, ULONG ActionFlags)
{
    UNREFERENCED_PARAMETER(ActionFlags);
    WdfObjectReference(Request);
    InterlockedExchange(&GetOlRequestContext(Request)->CancelWanted, 1);
    OlCancelRequest(GetClientDeviceContext(WdfIoQueueGetDevice(Queue)), Request);
    /* The actual completion, not the cancel return value, retires the I/O lease. */
    WdfObjectDereference(Request);
}
