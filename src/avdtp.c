/* SPDX-License-Identifier: Apache-2.0 */
#include "avdtp.h"
#include "wldac.h"
#include <string.h>
static int valid_seid(uint8_t seid) { return seid >= 1 && seid <= 62; }
int wl_avdtp_parse(const uint8_t *packet, size_t size, wl_avdtp_message *out) {
    if (!packet || !out || size < 2 || (packet[0] & 0x0c) || !(packet[1] & 0x3f))
        return -1;
    out->transaction = packet[0] >> 4;
    out->message_type = packet[0] & 3;
    out->signal = packet[1] & 0x3f;
    out->payload = packet + 2;
    out->payload_size = size - 2;
    return 0;
}
int wl_avdtp_first_audio_sink(const uint8_t *payload, size_t size, uint8_t *seid) {
    if (!payload || !seid || size % 2) return -1;
    for (size_t i = 0; i < size; i += 2) {
        const uint8_t remote = payload[i] >> 2;
        if (valid_seid(remote) && !(payload[i] & 2) &&
            (payload[i + 1] >> 4) == 0 && (payload[i + 1] & 8)) {
            *seid = remote;
            return 0;
        }
    }
    return -1;
}
int wl_avdtp_ldac_capability(const uint8_t *payload, size_t size, uint8_t codec[8]) {
    size_t offset = 0;
    int found = 0;
    uint8_t candidate[8];
    if (!payload || !codec) return -1;
    while (offset < size) {
        if (size - offset < 2) return -1;
        const uint8_t category = payload[offset], length = payload[offset + 1];
        offset += 2;
        if (length > size - offset) return -1;
        if (category == 7 && length == 10 && (payload[offset] >> 4) == 0 &&
            payload[offset + 1] == 0xff &&
            !memcmp(payload + offset + 2, "\x2d\x01\x00\x00\xaa\x00", 6)) {
            if (found) return -1;
            memcpy(candidate, payload + offset + 2, 8);
            found = 1;
        }
        offset += length;
    }
    if (!found) return -1;
    memcpy(codec, candidate, 8);
    return 0;
}
size_t wl_avdtp_get_caps(uint8_t transaction, uint8_t remote, uint8_t *out, size_t capacity) {
    if (transaction > 15 || !valid_seid(remote) || !out || capacity < 3) return 0;
    out[0] = transaction << 4;
    out[1] = 2;
    out[2] = remote << 2;
    return 3;
}
size_t wl_avdtp_discover(uint8_t transaction, uint8_t *out, size_t capacity) {
    if (transaction > 15 || !out || capacity < 2) return 0;
    out[0] = transaction << 4; out[1] = WL_AVDTP_DISCOVER;
    return 2;
}
static size_t endpoint_command(uint8_t transaction, uint8_t remote, uint8_t signal,
                               uint8_t *out, size_t capacity) {
    if (transaction > 15 || !valid_seid(remote) || !out || capacity < 3) return 0;
    out[0] = transaction << 4; out[1] = signal; out[2] = remote << 2;
    return 3;
}
size_t wl_avdtp_open(uint8_t transaction, uint8_t remote, uint8_t *out, size_t capacity) {
    return endpoint_command(transaction, remote, WL_AVDTP_OPEN, out, capacity);
}
size_t wl_avdtp_start(uint8_t transaction, uint8_t remote, uint8_t *out, size_t capacity) {
    return endpoint_command(transaction, remote, WL_AVDTP_START, out, capacity);
}
size_t wl_avdtp_suspend(uint8_t transaction, uint8_t remote, uint8_t *out, size_t capacity) {
    return endpoint_command(transaction, remote, WL_AVDTP_SUSPEND, out, capacity);
}
size_t wl_avdtp_abort(uint8_t transaction, uint8_t remote, uint8_t *out, size_t capacity) {
    return endpoint_command(transaction, remote, WL_AVDTP_ABORT, out, capacity);
}
size_t wl_avdtp_set_config(uint8_t transaction, uint8_t remote, uint8_t local,
                          const uint8_t codec[8], uint8_t *out, size_t capacity) {
    uint8_t selected[8];
    unsigned rate;
    if (transaction > 15 || !valid_seid(remote) || !valid_seid(local) ||
        !codec || !out || capacity < 18 ||
        wl_ldac_select(codec, 8, 0, 1, selected, &rate) || memcmp(codec, selected, 8))
        return 0; /* A configuration must select exactly one rate and channel mode. */
    out[0] = transaction << 4; out[1] = 3;
    out[2] = remote << 2; out[3] = local << 2;
    out[4] = 1; out[5] = 0; /* Media Transport, zero-length. */
    out[6] = 7; out[7] = 10; out[8] = 0; out[9] = 0xff;
    memcpy(out + 10, codec, 8);
    return 18;
}
