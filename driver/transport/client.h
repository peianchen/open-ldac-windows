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

    client.h

Abstract:

    Contains declarations for Bluetooth client functionality.
    
Environment:

    Kernel mode only


--*/

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
BthEchoCliBthQueryInterfaces(
    _In_ PBTHECHOSAMPLE_CLIENT_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
BthEchoCliRetrieveServerBthAddress(
    _In_ PBTHECHOSAMPLE_CLIENT_CONTEXT DevCtx
    );

