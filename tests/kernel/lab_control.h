/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OL_KERNEL_LAB_CONTROL_H
#define OL_KERNEL_LAB_CONTROL_H
#include "native_transport.h"
/* Test-only control ABI. Never included in the production transport project. */
#define OL_LAB_IOCTL ((OL_DEVICE_TYPE << 16) | (3u << 14) | (0xf00u << 2))
#define OL_LAB_ABI 2u
enum ol_lab_command {
    OL_LAB_FAIL_OPEN = 1, OL_LAB_FAIL_CLOSE, OL_LAB_KEEP_CALLBACKS,
    OL_LAB_RELEASE_CALLBACKS, OL_LAB_REMOTE_DISCONNECT, OL_LAB_REMOVE, OL_LAB_RESET,
    OL_LAB_HOLD_OPEN, OL_LAB_COMPLETE_OPEN, OL_LAB_QUERY
};
typedef struct { ol_u32 size, version, command, value; } ol_lab_request;
/* Hold=1: framework-owned cancelable manual queue. Hold=2: provider owns the
 * request until COMPLETE_OPEN, including after cancellation was requested.
 * COMPLETE_OPEN value=0 succeeds, value=1 fails. Pending test callbacks are
 * retained until RELEASE_CALLBACKS (or CLOSE), independently of completion. */
typedef struct {
    ol_u32 size, version, pending_open, held_cancel_seen, active_files, reserved;
    ol_u64 open_deferred, open_cancelled, open_control_completed;
} ol_lab_status;
#endif
