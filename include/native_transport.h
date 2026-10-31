/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OL_NATIVE_TRANSPORT_H
#define OL_NATIVE_TRANSPORT_H
#include <stdint.h>
#define OL_NATIVE_ABI 1u
#define OL_DEVICE_TYPE 0x8337u
#define OL_MAX_PACKET 1008u
#define OL_MAX_CHANNELS 2u
#define OL_MAX_INFLIGHT 8u
#define OL_IOCTL_QUERY ((OL_DEVICE_TYPE << 16) | (1u << 14) | (0x800u << 2))
#define OL_IOCTL_CONNECT ((OL_DEVICE_TYPE << 16) | (3u << 14) | (0x801u << 2))
#define OL_NATIVE_INTERFACE_GUID {0x8cbef89d,0x00a0,0x4694,{0xa3,0x2e,0x22,0x0c,0xb9,0x6c,0x01,0x91}}
#define OL_FLAG_TRANSPORT_ONLY 1u
#define OL_FLAG_ENCRYPTION_REQUIRED 2u
typedef struct {
    uint32_t size;
    uint32_t version;
} ol_native_request;
typedef struct {
    uint32_t size, version, flags, connection_state;
    uint32_t outbound_mtu, inbound_mtu, max_packet, max_channels;
    uint32_t in_flight, max_in_flight, reserved0, reserved1;
    uint64_t bytes_sent, bytes_received;
} ol_native_status;
/* The transport only exchanges packets. No codec/quality/audio endpoint is implied. */
#endif
