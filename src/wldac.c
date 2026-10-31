/* SPDX-License-Identifier: Apache-2.0 */
#include "wldac.h"
#include <string.h>

static uint8_t rate_bit(unsigned rate) {
    switch (rate) {
    case 44100: return 0x20;
    case 48000: return 0x10;
    case 88200: return 0x08;
    case 96000: return 0x04;
    default: return 0;
    }
}

int wl_select_policy(wl_profile profile, unsigned source_rate, int allow_hires,
                     int on_battery, wl_policy *out) {
    (void)on_battery; /* Unplugging alone must never silently lower quality. */
    if (!out || profile < WL_QUALITY || profile > WL_ECO) return -1;
    if (source_rate != 0 && !rate_bit(source_rate)) return -1;
    out->sample_rate = source_rate ? source_rate : 48000;
    if (out->sample_rate > 48000 && (!allow_hires || profile == WL_ECO))
        out->sample_rate = out->sample_rate == 88200 ? 44100 : 48000;
    out->eqmid = profile == WL_ECO ? LDACBT_EQMID_SQ : LDACBT_EQMID_HQ;
    out->target_batch_ms = 10; /* Design target; not a timer or measured optimum. */
    out->max_queue_ms = 40;
    out->adaptation_allowed = profile != WL_QUALITY;
    return 0;
}

int wl_ldac_select(const uint8_t *caps, size_t size, unsigned preferred,
                   int allow_hires, uint8_t config[8], unsigned *chosen) {
    static const uint8_t identity[6] = {0x2d, 0x01, 0, 0, 0xaa, 0};
    const unsigned fallback[] = {48000, 44100, 96000, 88200};
    unsigned rate = 0;
    if (!caps || !config || !chosen || size != 8 ||
        memcmp(caps, identity, 6) || !(caps[7] & LDACBT_CHANNEL_MODE_STEREO))
        return -1;
    if (rate_bit(preferred) && (allow_hires || preferred <= 48000) &&
        (caps[6] & rate_bit(preferred))) rate = preferred;
    for (size_t i = 0; !rate && i < sizeof(fallback) / sizeof(*fallback); ++i)
        if ((allow_hires || fallback[i] <= 48000) && (caps[6] & rate_bit(fallback[i])))
            rate = fallback[i];
    if (!rate) return -1;
    memcpy(config, identity, 6);
    config[6] = rate_bit(rate);
    config[7] = LDACBT_CHANNEL_MODE_STEREO;
    *chosen = rate;
    return 0;
}

static void be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

size_t wl_rtp_packet(wl_rtp_state *state, const uint8_t *frames, size_t bytes,
                     unsigned count, size_t mtu, uint8_t *out, size_t capacity) {
    const size_t header = 13; /* 12-byte RTP + 1-byte unfragmented LDAC header. */
    if (!state || !frames || !out || !bytes || !count || count > 15 ||
        state->payload_type < 96 || state->payload_type > 127 ||
        (state->samples_per_frame != 128 && state->samples_per_frame != 256) ||
        mtu < header || capacity < header || bytes > mtu - header ||
        bytes > capacity - header) return 0;
    out[0] = 0x80; out[1] = state->payload_type;
    out[2] = (uint8_t)(state->sequence >> 8); out[3] = (uint8_t)state->sequence;
    be32(out + 4, state->timestamp); be32(out + 8, state->ssrc);
    out[12] = (uint8_t)count;
    memcpy(out + header, frames, bytes);
    state->sequence++;
    state->timestamp += count * state->samples_per_frame;
    return header + bytes;
}

int wl_encoder_open(wl_encoder *enc, unsigned rate, LDACBT_SMPL_FMT_T format,
                     int eqmid, unsigned mtu) {
    unsigned word_bytes;
    if (!enc) return -1;
    memset(enc, 0, sizeof(*enc));
    if (!rate_bit(rate) || eqmid < LDACBT_EQMID_HQ || eqmid > LDACBT_EQMID_MQ ||
        mtu < 679 || mtu > LDACBT_MAX_NBYTES) return -1;
    switch (format) {
    case LDACBT_SMPL_FMT_S16: word_bytes = 2; break;
    case LDACBT_SMPL_FMT_S24: word_bytes = 3; break;
    case LDACBT_SMPL_FMT_S32:
    case LDACBT_SMPL_FMT_F32: word_bytes = 4; break;
    default: return -1;
    }
    enc->handle = ldacBT_get_handle();
    if (!enc->handle) return -1;
    if (ldacBT_init_handle_encode(enc->handle, (int)mtu, eqmid,
            LDACBT_CHANNEL_MODE_STEREO, format, (int)rate) != 0) {
        enc->library_error = ldacBT_get_error_code(enc->handle);
        wl_encoder_close(enc);
        return -1;
    }
    enc->rate = rate; enc->channels = 2; enc->sample_bytes = word_bytes;
    return 0;
}

int wl_encoder_push(wl_encoder *enc, const void *pcm, wl_encoded *out) {
    int used = 0;
    if (!enc || !enc->handle || !out) return -1;
    out->size = out->frames = 0;
    if (ldacBT_encode(enc->handle, (void *)pcm, &used, out->bytes,
                      &out->size, &out->frames) != 0) {
        enc->library_error = ldacBT_get_error_code(enc->handle);
        return -1;
    }
    if ((pcm && used != (int)(128 * enc->channels * enc->sample_bytes)) ||
        out->size < 0 || out->size > LDACBT_MAX_NBYTES || out->frames < 0 ||
        out->frames > 15 || (out->size == 0) != (out->frames == 0)) return -1;
    return 0;
}

void wl_encoder_close(wl_encoder *enc) {
    if (enc && enc->handle) {
        ldacBT_free_handle(enc->handle);
        enc->handle = NULL;
    }
}
