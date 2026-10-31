/* SPDX-License-Identifier: MS-PL
 * Derived from Microsoft bthecho; Open LDAC Windows modifications, 2026-10-31.
 * See driver/UPSTREAM.md and driver/LICENSE-MS-PL. */
/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    Device.h

Abstract:

    Conext and function declarations for bthecho client device

Environment:

    Kernel mode only


--*/

#include "clisrv.h"

typedef struct _BTHECHOSAMPLE_CLIENT_CONTEXT
{
    //
    // Context common to client and server
    //
    BTHECHOSAMPLE_DEVICE_CONTEXT_HEADER  Header;

    //
    // Server address
    //
    BTH_ADDR                                ServerBthAddress;
    volatile LONG ActiveFiles;
    volatile LONG Connections;
    volatile LONG RetainedConnections;
    volatile LONG InFlight;
    volatile LONG64 BytesSent;
    volatile LONG64 BytesReceived; 

} BTHECHOSAMPLE_CLIENT_CONTEXT, *PBTHECHOSAMPLE_CLIENT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTHECHOSAMPLE_CLIENT_CONTEXT, GetClientDeviceContext)

typedef struct _BTHECHOSAMPLE_CLIENT_FILE_CONTEXT
{
    //
    // Connection to server opened for this file
    //
    PBTHECHO_CONNECTION Connection;

    //
    // Server PSM
    //
    WDFSPINLOCK Lock;
    volatile LONG ConnectIssued;
    volatile LONG Closing;
    BOOLEAN Counted;    
} BTHECHOSAMPLE_CLIENT_FILE_CONTEXT, *PBTHECHOSAMPLE_CLIENT_FILE_CONTEXT;

//
// Context for requests
//
typedef struct _OL_REQUEST_CONTEXT {
    BRB Brb;
    volatile LONG TransferActive;
    ULONG Direction;
    volatile LONG CancelWanted;
    PBTHECHO_CONNECTION Connection;
    ULONG Slot;
} OL_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OL_REQUEST_CONTEXT, GetOlRequestContext);
#define GetRequestContext(Request) (&GetOlRequestContext(Request)->Brb)    

//
// Context for file objects
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTHECHOSAMPLE_CLIENT_FILE_CONTEXT, GetFileContext);

NTSTATUS
FORCEINLINE
BthEchoCliContextInit(
    PBTHECHOSAMPLE_CLIENT_CONTEXT context,
    WDFDEVICE Device
    )
{
    return BthEchoSharedDeviceContextHeaderInit(&context->Header, Device);
}

EVT_WDF_DRIVER_DEVICE_ADD BthEchoCliEvtDriverDeviceAdd;

EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT BthEchoCliEvtDeviceSelfManagedIoInit;

EVT_WDF_DEVICE_FILE_CREATE BthEchoCliEvtDeviceFileCreate;

EVT_WDF_FILE_CLOSE BthEchoCliEvtFileClose;
EVT_WDF_FILE_CLEANUP OlEvtFileCleanup;
EVT_WDF_OBJECT_CONTEXT_DESTROY OlEvtDeviceDestroy;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL OlEvtDeviceIoControl;
VOID OlConnectChannel(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ WDFFILEOBJECT FileObject);
NTSTATUS OlBeginTransfer(WDFREQUEST Request, size_t Length, ULONG Direction);
VOID OlFinishTransfer(WDFREQUEST Request, NTSTATUS Status, size_t Bytes);

