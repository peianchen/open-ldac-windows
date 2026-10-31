/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OL_KERNEL_LAB_CONTROL_H
#define OL_KERNEL_LAB_CONTROL_H
#include "native_transport.h"
/* Test-only control ABI. Never included in the production transport project. */
#define OL_LAB_IOCTL ((OL_DEVICE_TYPE << 16) | (3u << 14) | (0xf00u << 2))
#define OL_LAB_ABI 1u
enum ol_lab_command {
    OL_LAB_FAIL_OPEN = 1, OL_LAB_FAIL_CLOSE, OL_LAB_KEEP_CALLBACKS,
    OL_LAB_RELEASE_CALLBACKS, OL_LAB_REMOTE_DISCONNECT, OL_LAB_REMOVE, OL_LAB_RESET
};
typedef struct { ol_u32 size, version, command, value; } ol_lab_request;
#endif
