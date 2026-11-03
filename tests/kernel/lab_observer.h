/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* Observe REAL WDF return values. This header is visible only in the lab build;
 * it does not inject a status or replace a framework method. */
enum ol_resource_site {
    OL_RES_DRIVER_CREATE, OL_RES_INIT_LOCK, OL_RES_INIT_DEVICE, OL_RES_INIT_QUEUE,
    OL_RES_INIT_TARGET, OL_RES_FILE_LOCK, OL_RES_ROOT_OBJECT, OL_RES_ROOT_LOCK,
    OL_RES_CLOSE_REQUEST, OL_RES_CLOSE_MEMORY, OL_RES_OPEN_MEMORY, OL_RES_OPEN_FORMAT,
    OL_RES_TRANSFER_MEMORY, OL_RES_TRANSFER_FORMAT, OL_RES_CLOSE_REUSE,
    OL_RES_CLOSE_FORMAT, OL_RES_OPEN_SEND, OL_RES_TRANSFER_SEND, OL_RES_CLOSE_SEND,
    OL_RES_PROVIDER_FINAL_RETIREMENT, OL_RES_INIT_ALLOC, OL_RES_INIT_NAME,
    OL_RES_INIT_TARGET_OPEN, OL_RES_INIT_LINK, OL_RES_SITE_COUNT
};
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID OlLabObserveResource(_In_ ULONG Site, _In_ NTSTATUS Status);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID OlLabResourceProgress(VOID);
#define OL_OBSERVE_RESULT(Site, Status) OlLabObserveResource((Site), (Status))
#define OL_OBSERVE_PROGRESS() OlLabResourceProgress()
