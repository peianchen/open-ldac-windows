/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WLDAC_H
#define WLDAC_H
#include <stddef.h>
#include <stdint.h>
#include "ldacBT.h"

/* User-mode encoder. Never compile the floating-point codec into the driver. */
typedef enum { WL_QUALITY, WL_BALANCED, WL_ECO } wl_profile;
typedef struct {
    unsigned sample_rate;
    int eqmid;
    unsigned target_batch_ms;
    unsigned max_queue_ms;
    int adaptation_allowed; /* Policy only; live ABR is not yet implemented. */
} wl_policy;
int wl_select_policy(wl_profile profile, unsigned source_rate, int allow_hires,
                     int on_battery, wl_policy *out);

/* Codec-specific portion ONLY: excludes AVDTP media type/codec type fields. */
int wl_ldac_select(const uint8_t *capabilities, size_t size, unsigned preferred_rate,
                   int allow_hires, uint8_t configuration[8], unsigned *chosen_rate);

typedef struct {
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    unsigned samples_per_frame;
    uint8_t payload_type; /* Dynamic RTP payload type, agreed by both ends. */
} wl_rtp_state;
/* No fragmentation in M0. Returns 0 without advancing state on failure. */
size_t wl_rtp_packet(wl_rtp_state *state, const uint8_t *frames, size_t bytes,
                     unsigned count, size_t mtu, uint8_t *out, size_t capacity);

typedef struct {
    HANDLE_LDAC_BT handle;
    unsigned rate;
    unsigned channels;
    unsigned sample_bytes;
    int library_error;
} wl_encoder;
typedef struct {
    uint8_t bytes[LDACBT_MAX_NBYTES];
    int size;
    int frames;
} wl_encoded;
int wl_encoder_open(wl_encoder *enc, unsigned rate, LDACBT_SMPL_FMT_T format,
                     int eqmid, unsigned l2cap_mtu);
/* Each non-NULL call supplies exactly 128 interleaved samples/channel. */
int wl_encoder_push(wl_encoder *enc, const void *pcm, wl_encoded *out);
void wl_encoder_close(wl_encoder *enc);
#endif
