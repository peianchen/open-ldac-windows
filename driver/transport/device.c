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

    Device.c

Abstract:

    Device and file object related functionality for bthecho client device

Environment:

    Kernel mode only


--*/

#include "clisrv.h"
#include "device.h"
#include "client.h"
#include "queue.h"

#define INITGUID
#include "public.h"

#if defined(EVENT_TRACING)
#include "device.tmh"
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, BthEchoCliEvtDriverDeviceAdd)
#pragma alloc_text (PAGE, BthEchoCliEvtFileClose)
#endif

NTSTATUS
BthEchoCliEvtDriverDeviceAdd(
    _In_ WDFDRIVER  Driver,
    _Inout_ PWDFDEVICE_INIT  DeviceInit
)
/*++
Routine Description:

    BthEchoCliEvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent a new instance of the device. All the software resources
    should be allocated in this callback.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                        status;
    WDFDEVICE                       device;    
    WDF_OBJECT_ATTRIBUTES           deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDF_FILEOBJECT_CONFIG           fileobjectConfig;
    WDF_OBJECT_ATTRIBUTES           fileAttributes, requestAttributes;
    WDF_IO_QUEUE_CONFIG             ioQueueConfig;
    WDFQUEUE                        queue;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    PBTHECHOSAMPLE_CLIENT_CONTEXT   devCtx;
    
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    WdfDeviceInitSetDeviceType(DeviceInit, OL_DEVICE_TYPE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, TRUE);

    //
    // Configure Pnp/power callbacks
    //
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDeviceSelfManagedIoInit = BthEchoCliEvtDeviceSelfManagedIoInit;

    WdfDeviceInitSetPnpPowerEventCallbacks(
       DeviceInit,
       &pnpPowerCallbacks
       );

    //
    // Configure file callbacks
    //    

    WDF_FILEOBJECT_CONFIG_INIT(
        &fileobjectConfig,
        BthEchoCliEvtDeviceFileCreate,
        BthEchoCliEvtFileClose,
        OlEvtFileCleanup
        );

    //
    // Inform framework to create context area in every fileobject
    // so that we can track information per open handle by the
    // application.
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, BTHECHOSAMPLE_CLIENT_FILE_CONTEXT);
    fileAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    WdfDeviceInitSetFileObjectConfig(
        DeviceInit,
        &fileobjectConfig,
        &fileAttributes
        );

    //
    // Inform framework to create context area in every request object.
    //
    // We make BRB as the context since we need BRB for all the requests
    // we handle (Create, Read, Write).
    //

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &requestAttributes,
        OL_REQUEST_CONTEXT
        );

    WdfDeviceInitSetRequestAttributes(
        DeviceInit,
        &requestAttributes
        );

    //
    // Set device attributes
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, BTHECHOSAMPLE_CLIENT_CONTEXT);
    deviceAttributes.EvtDestroyCallback = OlEvtDeviceDestroy;
    
    status = WdfDeviceCreate(
        &DeviceInit,
        &deviceAttributes,
        &device
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, 
            "WdfDeviceCreate failed with Status code 0x%08lx\n", status);

        goto exit;
    }

    devCtx = GetClientDeviceContext(device);

    //
    // Initialize our context
    //
    status = BthEchoCliContextInit(devCtx, device);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, 
            "Initialization of context failed with Status code 0x%08lx\n", status);

        goto exit;       
    }

    status = BthEchoCliBthQueryInterfaces(devCtx);        
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    //
    // Set up our queue
    //

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &ioQueueConfig,
        WdfIoQueueDispatchParallel
        );

    ioQueueConfig.PowerManaged = WdfTrue;
    ioQueueConfig.Settings.Parallel.NumberOfPresentedRequests = OL_MAX_INFLIGHT;
    ioQueueConfig.EvtIoDeviceControl = OlEvtDeviceIoControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    queueAttributes.SynchronizationScope = WdfSynchronizationScopeQueue;
    ioQueueConfig.EvtIoRead     = BthEchoCliEvtQueueIoRead;
    ioQueueConfig.EvtIoWrite    = BthEchoCliEvtQueueIoWrite;
    ioQueueConfig.EvtIoStop     = BthEchoCliEvtQueueIoStop;

    status = WdfIoQueueCreate(
        device,
        &ioQueueConfig,
        &queueAttributes,
        &queue
        );
    
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                            "WdfIoQueueCreate failed  0x%08lx\n", status);
        goto exit;
    }

    //
    // Enable device interface so that app can open a handle to our device
    // and talk to it
    //
    
    status = WdfDeviceCreateDeviceInterface(
        device,
        &BTHECHOSAMPLE_DEVICE_INTERFACE,
        NULL
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, 
            "Enabling device interface failed with Status code 0x%08lx\n", status);

        goto exit;       
    }

exit:    
    //
    // We don't need to worry about deleting any objects on failure
    // because all the object created so far are parented to device and when
    // we return an error, framework will delete the device and as a 
    // result all the child objects will get deleted along with that.
    //
    return status;
}

NTSTATUS
BthEchoCliEvtDeviceSelfManagedIoInit(
    _In_ WDFDEVICE  Device
    )
/*++

Description:

    This routine is called by the framework only once
    and hence we use it for our one time initialization.

    Bth addresses for both our local device and server device
    do not change, hence we retrieve them here.

    Features that the local device supports also do not change,
    so checking for local L2cap support is done here and saved in the
    device context header

    Please note that retrieveing server bth address does not
    require presence of the server. It is remembered from
    the installation time when the client gets installed for a
    specific server.

Arguments:

    Device - Framework device object

Return Value:

    NTSTATUS Status code.

--*/
{
    NTSTATUS status;
    PBTHECHOSAMPLE_CLIENT_CONTEXT devCtx = GetClientDeviceContext(Device);

    status = BthEchoSharedRetrieveLocalInfo(&devCtx->Header);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    status = BthEchoCliRetrieveServerBthAddress(devCtx);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

exit:
    return status;
}

VOID
BthEchoCliEvtFileClose(
    _In_ WDFFILEOBJECT  FileObject
    )
/*++
Description:

    This routine is invoked by Framework when I/O manager sends Close
    IRP for a file.

    In response we close the remote connection to the server.

Arguments:

    FileObject - File object corresponding to Close

--*/
{    
    PBTHECHOSAMPLE_CLIENT_CONTEXT devCtx;

    PAGED_CODE();

    devCtx = GetClientDeviceContext(WdfFileObjectGetDevice(FileObject));

    OlFileDetach(FileObject); /* Idempotent, nonblocking. */
    if (GetFileContext(FileObject)->Counted) {
        GetFileContext(FileObject)->Counted = FALSE;
        InterlockedDecrement(&devCtx->ActiveFiles);
    }
}

