/* SPDX-License-Identifier: Apache-2.0 */
#include "wldac.h"
#include "avdtp.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

static int policy_tests(void) {
    wl_policy ac, dc;
    CHECK(!wl_select_policy(WL_QUALITY, 48000, 0, 0, &ac));
    CHECK(!wl_select_policy(WL_QUALITY, 48000, 0, 1, &dc));
    CHECK(ac.sample_rate == dc.sample_rate && ac.eqmid == dc.eqmid);
    CHECK(ac.eqmid == LDACBT_EQMID_HQ && !ac.adaptation_allowed);
    CHECK(!wl_select_policy(WL_BALANCED, 44100, 0, 1, &dc));
    CHECK(dc.sample_rate == 44100 && dc.eqmid == LDACBT_EQMID_HQ);
    CHECK(!wl_select_policy(WL_QUALITY, 96000, 0, 1, &dc) && dc.sample_rate == 48000);
    CHECK(!wl_select_policy(WL_QUALITY, 96000, 1, 1, &dc) && dc.sample_rate == 96000);
    CHECK(!wl_select_policy(WL_ECO, 88200, 1, 1, &dc) && dc.sample_rate == 44100);
    CHECK(dc.eqmid == LDACBT_EQMID_SQ);
    CHECK(wl_select_policy(WL_QUALITY, 192000, 1, 1, &dc) < 0);
    CHECK(wl_select_policy((wl_profile)99, 48000, 0, 1, &dc) < 0);
    CHECK(wl_select_policy(WL_QUALITY, 48000, 0, 1, NULL) < 0);
    return 0;
}
static int negotiation_tests(void) {
    uint8_t caps[8] = {0x2d, 1, 0, 0, 0xaa, 0, 0x3c, 7}, out[8];
    unsigned rate;
    CHECK(!wl_ldac_select(caps, 8, 44100, 0, out, &rate));
    CHECK(rate == 44100 && out[6] == 0x20 && out[7] == 1);
    CHECK(!wl_ldac_select(caps, 8, 96000, 0, out, &rate) && rate == 48000);
    CHECK(!wl_ldac_select(caps, 8, 96000, 1, out, &rate) && rate == 96000);
    caps[6] = 4;
    CHECK(wl_ldac_select(caps, 8, 48000, 0, out, &rate) < 0);
    CHECK(!wl_ldac_select(caps, 8, 48000, 1, out, &rate) && rate == 96000);
    caps[7] = 4;
    CHECK(wl_ldac_select(caps, 8, 48000, 1, out, &rate) < 0);
    caps[7] = 1; caps[0] = 0;
    CHECK(wl_ldac_select(caps, 8, 48000, 1, out, &rate) < 0);
    CHECK(wl_ldac_select(caps, 7, 48000, 1, out, &rate) < 0);
    CHECK(wl_ldac_select(NULL, 8, 48000, 1, out, &rate) < 0);
    return 0;
}
static int rtp_tests(void) {
    uint8_t data[660] = {0}, out[680];
    wl_rtp_state state = {65535, 0xffffff80u, 0x12345678u, 128, 96};
    memset(out, 0xcc, sizeof(out));
    CHECK(wl_rtp_packet(&state, data, 660, 2, 679, out, sizeof(out)) == 673);
    CHECK(out[0] == 0x80 && out[1] == 96 && out[2] == 0xff && out[3] == 0xff);
    CHECK(out[4] == 0xff && out[7] == 0x80 && out[8] == 0x12 && out[11] == 0x78);
    CHECK(out[12] == 2 && out[673] == 0xcc);
    CHECK(state.sequence == 0 && state.timestamp == 128);
    CHECK(!wl_rtp_packet(&state, data, 660, 2, 672, out, sizeof(out)));
    CHECK(state.sequence == 0 && state.timestamp == 128);
    CHECK(!wl_rtp_packet(&state, data, 660, 2, 679, out, 672));
    CHECK(!wl_rtp_packet(&state, data, 660, 16, 679, out, sizeof(out)));
    CHECK(!wl_rtp_packet(&state, data, 660, 0, 679, out, sizeof(out)));
    CHECK(!wl_rtp_packet(&state, data, SIZE_MAX, 2, 679, out, sizeof(out)));
    state.samples_per_frame = 256;
    CHECK(wl_rtp_packet(&state, data, 660, 2, 679, out, sizeof(out)) == 673);
    CHECK(state.timestamp == 640);
    return 0;
}
static int avdtp_tests(void) {
    const uint8_t discovered[] = {0x06, 0x08, 0x08, 0x00, 0x14, 0x08};
    const uint8_t caps[] = {1, 0, 7, 10, 0, 0xff, 0x2d, 1, 0, 0, 0xaa, 0, 0x3c, 7};
    const uint8_t expected[] = {0x70, 3, 20, 4, 1, 0, 7, 10, 0, 0xff,
                                0x2d, 1, 0, 0, 0xaa, 0, 0x10, 1};
    uint8_t seid, codec[8], config[8], out[32], invalid[28];
    unsigned rate;
    wl_avdtp_message message;
    CHECK(!wl_avdtp_first_audio_sink(discovered, sizeof(discovered), &seid) && seid == 5);
    CHECK(wl_avdtp_first_audio_sink(discovered, 5, &seid) < 0);
    CHECK(!wl_avdtp_ldac_capability(caps, sizeof(caps), codec));
    CHECK(wl_avdtp_ldac_capability(caps, sizeof(caps) - 1, codec) < 0);
    memcpy(invalid, caps, 14); memcpy(invalid + 14, caps, 14);
    CHECK(wl_avdtp_ldac_capability(invalid, sizeof(invalid), codec) < 0);
    CHECK(!wl_ldac_select(codec, 8, 48000, 0, config, &rate));
    CHECK(wl_avdtp_set_config(7, 5, 1, config, out, sizeof(out)) == sizeof(expected));
    CHECK(!memcmp(out, expected, sizeof(expected)));
    CHECK(!wl_avdtp_parse(out, sizeof(expected), &message));
    CHECK(message.transaction == 7 && message.signal == 3 && message.payload_size == 16);
    CHECK(wl_avdtp_set_config(7, 5, 1, codec, out, sizeof(out)) == 0);
    CHECK(wl_avdtp_set_config(7, 5, 1, config, out, 17) == 0);
    CHECK(wl_avdtp_get_caps(15, 62, out, sizeof(out)) == 3);
    CHECK(out[0] == 0xf0 && out[1] == 2 && out[2] == 248);
    CHECK(wl_avdtp_get_caps(16, 62, out, sizeof(out)) == 0);
    CHECK(wl_avdtp_get_caps(0, 63, out, sizeof(out)) == 0);
    out[0] |= 4;
    CHECK(wl_avdtp_parse(out, 3, &message) < 0);
    CHECK(wl_avdtp_parse(out, 1, &message) < 0);
    return 0;
}
static int codec_tests(void) {
    const unsigned rates[] = {44100, 48000, 88200, 96000};
    float pcm[128 * 2];
    wl_encoder encoder;
    wl_encoded out;
    for (unsigned i = 0; i < 256; ++i) pcm[i] = (float)(0.4 * sin(i * 0.13));
    for (unsigned r = 0; r < 4; ++r) for (int q = 0; q < 3; ++q) {
        uint64_t bytes = 0, frames = 0;
        CHECK(!wl_encoder_open(&encoder, rates[r], LDACBT_SMPL_FMT_F32, q, 679));
        for (unsigned n = 0; n < 100; ++n) {
            CHECK(!wl_encoder_push(&encoder, pcm, &out));
            CHECK(out.size <= 666); /* Negotiated MTU minus RTP/LDAC headers. */
            if (out.size) CHECK(out.bytes[0] == 0xaa);
            bytes += (unsigned)out.size; frames += (unsigned)out.frames;
        }
        int drained = 0;
        for (unsigned n = 0; n < 32; ++n) {
            CHECK(!wl_encoder_push(&encoder, NULL, &out));
            bytes += (unsigned)out.size; frames += (unsigned)out.frames;
            if (!out.size) { drained = 1; break; }
        }
        CHECK(drained && frames >= (rates[r] > 48000 ? 50u : 100u));
        const int kbps = ldacBT_get_bitrate(encoder.handle);
        const unsigned expected = (rates[r] == 44100 || rates[r] == 88200) ? 909 : 990;
        CHECK(kbps == (int)(expected * (3 - q) / 3));
        /* Constant-size LDAC transport frames: HQ 330, SQ 220, MQ 110 bytes. */
        CHECK(bytes == frames * (q == 0 ? 330u : q == 1 ? 220u : 110u));
        wl_encoder_close(&encoder);
        CHECK(wl_encoder_push(&encoder, pcm, &out) < 0);
        wl_encoder_close(&encoder);
    }
    const LDACBT_SMPL_FMT_T formats[] = {LDACBT_SMPL_FMT_S16, LDACBT_SMPL_FMT_S24,
                                        LDACBT_SMPL_FMT_S32, LDACBT_SMPL_FMT_F32};
    unsigned char silence[1024] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(!wl_encoder_open(&encoder, 48000, formats[i], 0, 679));
        CHECK(!wl_encoder_push(&encoder, silence, &out));
        wl_encoder_close(&encoder);
    }
    CHECK(wl_encoder_open(&encoder, 192000, LDACBT_SMPL_FMT_F32, 0, 679) < 0);
    CHECK(wl_encoder_open(&encoder, 48000, LDACBT_SMPL_FMT_F32, 0, 678) < 0);
    CHECK(wl_encoder_open(&encoder, 48000, LDACBT_SMPL_FMT_F32, 3, 679) < 0);
    return 0;
}
int main(void) {
    if (policy_tests() || negotiation_tests() || rtp_tests() || avdtp_tests() || codec_tests()) return 1;
    printf("PASS: %u checks; offline codec/protocol/policy only; no Bluetooth playback.\n", checks);
    return 0;
}
