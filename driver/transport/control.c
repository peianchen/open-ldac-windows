/* SPDX-License-Identifier: MS-PL
 * Open LDAC Windows control layer for the Microsoft-derived transport.
 * See ../LICENSE-MS-PL and ../UPSTREAM.md. */
#include "clisrv.h"
#include "device.h"

C_ASSERT(sizeof(ol_native_request) == 8);
C_ASSERT(sizeof(ol_native_status) == 64);

VOID BthEchoCliEvtDeviceFileCreate(WDFDEVICE Device, WDFREQUEST Request,
                                 WDFFILEOBJECT FileObject)
{
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(Device);
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file = GetFileContext(FileObject);
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
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file = GetFileContext(FileObject);
    InterlockedExchange(&file->Closing, 1);
    if (file->Connection != NULL) {
        /* Closing the L2CAP channel unblocks pending reads without polling. */
        BthEchoConnectionObjectRemoteDisconnect(file->Connection->DevCtxHdr,
                                                file->Connection);
    }
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
        output->in_flight = (uint32_t)InterlockedCompareExchange(&dev->InFlight, 0, 0);
        output->bytes_sent = (uint64_t)InterlockedCompareExchange64(&dev->BytesSent, 0, 0);
        output->bytes_received = (uint64_t)InterlockedCompareExchange64(&dev->BytesReceived, 0, 0);
        if (file->Connection != NULL) {
            WdfSpinLockAcquire(file->Connection->ConnectionLock);
            output->connection_state = (uint32_t)file->Connection->ConnectionState;
            output->outbound_mtu = file->Connection->OutMTU;
            output->inbound_mtu = file->Connection->InMTU;
            WdfSpinLockRelease(file->Connection->ConnectionLock);
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
        /* Bound remote address, Audio Sink SDP and AVDTP PSM only: no arbitrary PSM IOCTL. */
        OlConnectChannel(WdfIoQueueGetDevice(Queue), Request, object);
        return;
    }
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

NTSTATUS OlBeginTransfer(WDFREQUEST Request, size_t Length, ULONG Direction)
{
    WDFFILEOBJECT object = WdfRequestGetFileObject(Request);
    PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT file;
    PBTHECHOSAMPLE_CLIENT_CONTEXT dev;
    PBTHECHO_CONNECTION connection;
    OL_REQUEST_CONTEXT *request = GetOlRequestContext(Request);
    USHORT mtu;
    if (object == NULL || Length == 0 || Length > OL_MAX_PACKET)
        return STATUS_INVALID_BUFFER_SIZE;
    file = GetFileContext(object);
    connection = file->Connection;
    if (connection == NULL || InterlockedCompareExchange(&file->Closing, 0, 0))
        return STATUS_INVALID_DEVICE_STATE;
    WdfSpinLockAcquire(connection->ConnectionLock);
    if (connection->ConnectionState != ConnectionStateConnected) {
        WdfSpinLockRelease(connection->ConnectionLock);
        return STATUS_CONNECTION_DISCONNECTED;
    }
    mtu = Direction == ACL_TRANSFER_DIRECTION_OUT ? connection->OutMTU : connection->InMTU;
    WdfSpinLockRelease(connection->ConnectionLock);
    if ((Direction == ACL_TRANSFER_DIRECTION_OUT && Length > mtu) ||
        (Direction == ACL_TRANSFER_DIRECTION_IN && Length < mtu))
        return STATUS_INVALID_BUFFER_SIZE;
    dev = GetClientDeviceContext(WdfFileObjectGetDevice(object));
    if (InterlockedIncrement(&dev->InFlight) > (LONG)OL_MAX_INFLIGHT) {
        InterlockedDecrement(&dev->InFlight);
        return STATUS_DEVICE_BUSY;
    }
    request->Direction = Direction;
    InterlockedExchange(&request->TransferActive, 1);
    return STATUS_SUCCESS;
}

VOID OlFinishTransfer(WDFREQUEST Request, NTSTATUS Status, size_t Bytes)
{
    OL_REQUEST_CONTEXT *request = GetOlRequestContext(Request);
    if (InterlockedExchange(&request->TransferActive, 0)) {
        PBTHECHOSAMPLE_CLIENT_CONTEXT dev = GetClientDeviceContext(
            WdfFileObjectGetDevice(WdfRequestGetFileObject(Request)));
        if (NT_SUCCESS(Status)) {
            if (request->Direction == ACL_TRANSFER_DIRECTION_OUT)
                InterlockedExchangeAdd64(&dev->BytesSent, (LONG64)Bytes);
            else InterlockedExchangeAdd64(&dev->BytesReceived, (LONG64)Bytes);
        }
        InterlockedDecrement(&dev->InFlight);
    }
}
