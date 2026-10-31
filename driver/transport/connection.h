/* SPDX-License-Identifier: MS-PL
 * Derived from Microsoft bthecho. Copyright (c) Microsoft Corporation.
 * Lifecycle redesign by Open LDAC Windows contributors, 2026-10-31.
 * See ../LICENSE-MS-PL and ../UPSTREAM.md. */
#pragma once
#include "lifetime.h"
typedef struct _BTHECHO_CONNECTION {
    PBTHECHOSAMPLE_DEVICE_CONTEXT_HEADER DevCtxHdr;
    WDFOBJECT Object;
    WDFSPINLOCK ConnectionLock;
    ol_lifetime Life;
    USHORT OutMTU, InMTU;
    L2CAP_CHANNEL_HANDLE ChannelHandle;
    BTH_ADDR RemoteAddress;
    struct _BRB_L2CA_CLOSE_CHANNEL CloseBrb;
    WDFREQUEST OpenRequest;
    WDFREQUEST CloseRequest;
    WDFMEMORY CloseMemory;
    WDFREQUEST Transfers[OL_MAX_INFLIGHT];
    BOOLEAN Ready, Counted, RetainedCounted, DeviceHeld, TargetHeld, WdmHeld;
} BTHECHO_CONNECTION, *PBTHECHO_CONNECTION;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTHECHO_CONNECTION, GetConnectionObjectContext)
NTSTATUS BthEchoConnectionObjectCreate(PBTHECHOSAMPLE_DEVICE_CONTEXT_HEADER Header,
                                     WDFOBJECT UnusedParent, WDFOBJECT *Object);
/* A caller must own an owner/open/I/O/provider lease before entering. */
VOID OlConnectionEnter(PBTHECHO_CONNECTION Connection);
VOID OlConnectionLeave(PBTHECHO_CONNECTION Connection);
VOID OlConnectionDropOwner(PBTHECHO_CONNECTION Connection);
VOID OlConnectionClose(PBTHECHO_CONNECTION Connection);
VOID OlConnectionCancelTransfers(PBTHECHO_CONNECTION Connection);
PBTHECHO_CONNECTION OlFileBorrow(WDFFILEOBJECT File);
VOID OlFileDetach(WDFFILEOBJECT File);
NTSTATUS OlFileAttach(WDFFILEOBJECT File, PBTHECHO_CONNECTION Connection);
NTSTATUS FormatRequestWithBrb(WDFIOTARGET Target, WDFREQUEST Request, PBRB Brb, size_t Size);
NTSTATUS BthEchoConnectionObjectFormatRequestForL2CaTransfer(PBTHECHO_CONNECTION Connection,
    WDFREQUEST Request, struct _BRB_L2CA_ACL_TRANSFER **Brb, WDFMEMORY Memory, ULONG Flags);
EVT_WDF_OBJECT_CONTEXT_CLEANUP OlConnectionCleanup;
EVT_WDF_OBJECT_CONTEXT_DESTROY OlConnectionDestroy;
EVT_WDF_REQUEST_COMPLETION_ROUTINE OlCloseComplete;
