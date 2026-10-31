/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_AVDTP_H
#define WL_AVDTP_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint8_t transaction;
    uint8_t message_type;
    uint8_t signal;
    const uint8_t *payload;
    size_t payload_size;
} wl_avdtp_message;
/* M0 supports single signaling packets only. Fragments are explicitly rejected. */
int wl_avdtp_parse(const uint8_t *packet, size_t size, wl_avdtp_message *out);
int wl_avdtp_first_audio_sink(const uint8_t *discover_payload, size_t size, uint8_t *seid);
int wl_avdtp_ldac_capability(const uint8_t *capability_payload, size_t size, uint8_t codec[8]);
size_t wl_avdtp_get_caps(uint8_t transaction, uint8_t remote_seid, uint8_t *out, size_t capacity);
size_t wl_avdtp_set_config(uint8_t transaction, uint8_t remote_seid, uint8_t local_seid,
                          const uint8_t codec[8], uint8_t *out, size_t capacity);
#endif
