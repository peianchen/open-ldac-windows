/* SPDX-License-Identifier: MS-PL
 * Derived from Microsoft bthecho; Open LDAC Windows modifications, 2026-10-31.
 * See driver/UPSTREAM.md and driver/LICENSE-MS-PL. */
/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

ModuleName:

    public.h

Abstract:

    Contains definitions used both by driver and application


--*/

#pragma once
#include "native_transport.h"

//
// Service GUID and name for the service published by our bth server device
//

/* Standard Bluetooth Audio Sink service UUID. */
DEFINE_GUID(BTHECHOSAMPLE_SVC_GUID, 0x0000110b, 0x0000, 0x1000, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb);
extern __declspec(selectany) const PWSTR BthEchoSampleSvcName = L"Open LDAC Audio Sink";

//
// Device interface exposed by our bth client device
//

/* 8cbef89d-00a0-4694-a32e-220cb96c0191 — Open LDAC native transport */
DEFINE_GUID(BTHECHOSAMPLE_DEVICE_INTERFACE, 0x8cbef89d, 0x00a0, 0x4694, 0xa3, 0x2e, 0x22, 0x0c, 0xb9, 0x6c, 0x01, 0x91);


