/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OL_NATIVE_TRANSPORT_H
#define OL_NATIVE_TRANSPORT_H
#if defined(_KERNEL_MODE)
/* ntddk.h is already included. Do not pull user-mode CRT headers into a driver. */
typedef ULONG ol_u32;
typedef ULONGLONG ol_u64;
#else
#include <stdint.h>
typedef uint32_t ol_u32;
typedef uint64_t ol_u64;
#endif
#define OL_NATIVE_ABI 2u
#define OL_DEVICE_TYPE 0x8337u
#define OL_MAX_PACKET 1008u
#define OL_MAX_CHANNELS 2u
#define OL_MAX_INFLIGHT 8u
#define OL_IOCTL_QUERY ((OL_DEVICE_TYPE << 16) | (1u << 14) | (0x800u << 2))
#define OL_IOCTL_CONNECT ((OL_DEVICE_TYPE << 16) | (3u << 14) | (0x801u << 2))
#define OL_IOCTL_DIAGNOSTICS ((OL_DEVICE_TYPE << 16) | (1u << 14) | (0x802u << 2))
#define OL_NATIVE_INTERFACE_GUID {0x8cbef89d,0x00a0,0x4694,{0xa3,0x2e,0x22,0x0c,0xb9,0x6c,0x01,0x91}}
#define OL_FLAG_TRANSPORT_ONLY 1u
#define OL_FLAG_ENCRYPTION_REQUIRED 2u
#define OL_FLAG_RETAINED_CONTEXT 4u
#define OL_STATE_IDLE 0u
#define OL_STATE_OPENING 1u
#define OL_STATE_OPEN 2u
#define OL_STATE_CLOSING 3u
#define OL_STATE_RETIRED 4u
typedef struct {
    ol_u32 size;
    ol_u32 version;
} ol_native_request;
typedef struct {
    ol_u32 size, version, flags, connection_state;
    ol_u32 outbound_mtu, inbound_mtu, max_packet, max_channels;
    ol_u32 in_flight, max_in_flight, connections, retained_connections;
    ol_u64 bytes_sent, bytes_received;
} ol_native_status;
#define OL_DIAGNOSTICS_ABI 1u
#define OL_DIAG_REMOVED 1u
enum ol_diag_event {
    OL_EVENT_OPEN_SEND, OL_EVENT_OPEN_COMPLETE, OL_EVENT_OPEN_FAILURE,
    OL_EVENT_CALLBACK_ADD, OL_EVENT_CALLBACK_RELEASE, OL_EVENT_CONTRACT_FAULT,
    OL_EVENT_REMOTE_DISCONNECT, OL_EVENT_CLOSE_SEND, OL_EVENT_CLOSE_COMPLETE,
    OL_EVENT_CLOSE_FAILURE, OL_EVENT_CANCEL_CALL, OL_EVENT_TRANSFER_COMPLETE,
    OL_EVENT_ROOT_DESTROY, OL_EVENT_REMOVAL, OL_EVENT_COUNT
};
typedef struct {
    ol_u32 size, version, flags, count;
    ol_u64 events[OL_EVENT_COUNT];
} ol_native_diagnostics;
/* Per-device cumulative atomic counters; snapshot is NOT a coherent event trace.
 * SEND counts attempts, FAILURE includes failed local submissions/completions.
 * No counter or zero-counter snapshot alone proves provider/Verifier safety. */
/* The transport only exchanges packets. No codec/quality/audio endpoint is implied. */
#endif
