/* SPDX-License-Identifier: Apache-2.0 */
#include "avdtp_session.h"
#include "wldac.h"
#include <string.h>

static int valid_seid(uint8_t seid) { return seid >= 1 && seid <= 62; }
static int valid_rate(unsigned rate) {
    return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000;
}
static int epoch(const wl_avdtp_session *s, uint64_t generation) {
    if (!s || !generation) return WL_AVDTP_SESSION_INVALID_ARGUMENT;
    if (generation != s->generation) return WL_AVDTP_SESSION_WRONG_GENERATION;
    return s->transport_valid ? WL_AVDTP_SESSION_OK : WL_AVDTP_SESSION_TRANSPORT_INVALID;
}
static int fail(wl_avdtp_session *s, int error) {
    s->pending = 0; s->transport_valid = 0;
    s->state = WL_AVDTP_SESSION_FAILED; s->last_error = error;
    return error;
}
static uint8_t state_signal(wl_avdtp_session_state state) {
    switch (state) {
    case WL_AVDTP_SESSION_DISCOVER: return WL_AVDTP_DISCOVER;
    case WL_AVDTP_SESSION_CAPABILITIES: return WL_AVDTP_GET_CAPABILITIES;
    case WL_AVDTP_SESSION_CONFIGURE: return WL_AVDTP_SET_CONFIGURATION;
    case WL_AVDTP_SESSION_OPEN: return WL_AVDTP_OPEN;
    case WL_AVDTP_SESSION_START: return WL_AVDTP_START;
    case WL_AVDTP_SESSION_SUSPEND: return WL_AVDTP_SUSPEND;
    case WL_AVDTP_SESSION_ABORT: return WL_AVDTP_ABORT;
    case WL_AVDTP_SESSION_CLOSE: return WL_AVDTP_CLOSE;
    default: return 0;
    }
}

int wl_avdtp_session_init(wl_avdtp_session *s, uint64_t generation,
                         uint8_t local_seid, unsigned source_rate) {
    wl_avdtp_session initial = {0};
    if (!s || !generation || !valid_seid(local_seid) || !valid_rate(source_rate))
        return WL_AVDTP_SESSION_INVALID_ARGUMENT;
    if (generation <= s->generation) return WL_AVDTP_SESSION_WRONG_GENERATION;
    if (s->transport_valid) return WL_AVDTP_SESSION_INVALID_STATE;
    initial.generation = generation; initial.transport_valid = 1;
    initial.local_seid = local_seid; initial.source_rate = source_rate;
    initial.state = WL_AVDTP_SESSION_DISCOVER;
    *s = initial;
    return WL_AVDTP_SESSION_OK;
}

size_t wl_avdtp_session_next_command(wl_avdtp_session *s,
                                    uint8_t *out, size_t capacity,
                                    wl_avdtp_session_token *token) {
    uint8_t signal, transaction;
    size_t needed, length = 0;
    if (!s || !out || !token || !s->transport_valid || s->pending) return 0;
    signal = state_signal(s->state);
    if (!signal) return 0;
    needed = signal == WL_AVDTP_DISCOVER ? 2u : signal == WL_AVDTP_SET_CONFIGURATION ? 18u : 3u;
    if (capacity < needed) return 0;
    if (s->serial == UINT64_MAX) {
        (void)fail(s, WL_AVDTP_SESSION_SEQUENCE_EXHAUSTED); return 0;
    }
    transaction = s->next_transaction;
    switch (signal) {
    case WL_AVDTP_DISCOVER:
        length = wl_avdtp_discover(transaction, out, capacity); break;
    case WL_AVDTP_GET_CAPABILITIES:
        if (s->candidate_index >= s->candidate_count || s->candidate_count > WL_AVDTP_SESSION_MAX_SEPS)
            return 0;
        length = wl_avdtp_get_caps(transaction, s->candidates[s->candidate_index], out, capacity); break;
    case WL_AVDTP_SET_CONFIGURATION:
        length = wl_avdtp_set_config(transaction, s->remote_seid, s->local_seid, s->codec, out, capacity); break;
    case WL_AVDTP_OPEN:
        length = wl_avdtp_open(transaction, s->remote_seid, out, capacity); break;
    case WL_AVDTP_START:
        length = wl_avdtp_start(transaction, s->remote_seid, out, capacity); break;
    case WL_AVDTP_SUSPEND:
        length = wl_avdtp_suspend(transaction, s->remote_seid, out, capacity); break;
    case WL_AVDTP_ABORT:
        length = wl_avdtp_abort(transaction, s->remote_seid, out, capacity); break;
    case WL_AVDTP_CLOSE:
        length = wl_avdtp_close(transaction, s->remote_seid, out, capacity); break;
    default: return 0;
    }
    if (!length) return 0;
    ++s->serial;
    s->outstanding.generation = s->generation; s->outstanding.serial = s->serial;
    s->outstanding.transaction = transaction; s->outstanding.signal = signal;
    s->next_transaction = (uint8_t)((transaction + 1u) & 15u); s->pending = 1;
    *token = s->outstanding;
    return length;
}

static int discover_accept(wl_avdtp_session *s, const uint8_t *payload, size_t size) {
    uint8_t candidates[WL_AVDTP_SESSION_MAX_SEPS] = {0};
    uint64_t seen = 0;
    unsigned count = 0;
    if (size % 2 || size > WL_AVDTP_SESSION_MAX_SEPS * 2u)
        return fail(s, WL_AVDTP_SESSION_MALFORMED);
    for (size_t i = 0; i < size; i += 2) {
        uint8_t seid = payload[i] >> 2;
        if (!valid_seid(seid) || (seen & (UINT64_C(1) << seid)))
            return fail(s, WL_AVDTP_SESSION_MALFORMED);
        seen |= UINT64_C(1) << seid;
        /* RFA bits are ignored; inspect only in-use, media type and TSEP. */
        if (!(payload[i] & 2u) && (payload[i + 1] >> 4) == 0 && (payload[i + 1] & 8u))
            candidates[count++] = seid;
    }
    if (!count) return fail(s, WL_AVDTP_SESSION_NO_ENDPOINT);
    memcpy(s->candidates, candidates, sizeof(candidates));
    s->candidate_count = (uint8_t)count; s->candidate_index = 0;
    s->state = WL_AVDTP_SESSION_CAPABILITIES; s->pending = 0;
    return WL_AVDTP_SESSION_OK;
}

/* Check only the categories consumed by this unprotected configuration. Other
 * well-bounded optional categories are not represented as negotiated support. */
static int capability_shape(const uint8_t *payload, size_t size) {
    size_t offset = 0;
    unsigned transport = 0, codec = 0;
    while (offset < size) {
        uint8_t category, length;
        if (size - offset < 2) return 0;
        category = payload[offset]; length = payload[offset + 1]; offset += 2;
        if (length > size - offset || !category) return 0;
        if (category == 1 && (++transport != 1 || length != 0)) return 0;
        if (category == 7) {
            if (++codec != 1 || length < 2) return 0;
            if (payload[offset + 1] == 0xff && length < 8) return 0;
            if (length >= 8 && payload[offset + 1] == 0xff &&
                !memcmp(payload + offset + 2, "\x2d\x01\x00\x00\xaa\x00", 6) && length != 10)
                return 0;
        }
        if (category == 4 && length < 2) return 0;
        offset += length;
    }
    return transport == 1 && codec == 1;
}
static int capability_accept(wl_avdtp_session *s, const uint8_t *payload, size_t size) {
    uint8_t capabilities[8], selected[8];
    unsigned chosen = 0;
    if (!capability_shape(payload, size)) return fail(s, WL_AVDTP_SESSION_MALFORMED);
    if (!wl_avdtp_ldac_capability(payload, size, capabilities) &&
        !wl_ldac_select(capabilities, sizeof(capabilities), s->source_rate, 1, selected, &chosen)) {
        if (chosen == s->source_rate) {
            if (s->candidate_index >= s->candidate_count)
                return fail(s, WL_AVDTP_SESSION_INVALID_STATE);
            memcpy(s->codec, selected, sizeof(s->codec));
            s->remote_seid = s->candidates[s->candidate_index];
            s->state = WL_AVDTP_SESSION_CONFIGURE; s->pending = 0;
            return WL_AVDTP_SESSION_OK;
        }
        s->conversion_required_seen = 1;
    }
    s->pending = 0;
    if (++s->candidate_index >= s->candidate_count)
        return fail(s, s->conversion_required_seen ? WL_AVDTP_SESSION_CONVERSION_REQUIRED : WL_AVDTP_SESSION_NO_LDAC);
    return WL_AVDTP_SESSION_OK;
}
static int reject_response(wl_avdtp_session *s, const wl_avdtp_message *m) {
    size_t expected;
    if (m->message_type == WL_AVDTP_GENERAL_REJECT) {
        if (m->payload_size != 0) return fail(s, WL_AVDTP_SESSION_MALFORMED);
        return fail(s, WL_AVDTP_SESSION_REJECTED);
    }
    if (m->signal == WL_AVDTP_ABORT) return fail(s, WL_AVDTP_SESSION_MALFORMED);
    expected = m->signal == WL_AVDTP_SET_CONFIGURATION || m->signal == WL_AVDTP_START ||
               m->signal == WL_AVDTP_SUSPEND ? 2u : 1u;
    if (m->payload_size != expected || m->payload[expected - 1] == 0)
        return fail(s, WL_AVDTP_SESSION_MALFORMED);
    if ((m->signal == WL_AVDTP_START || m->signal == WL_AVDTP_SUSPEND) &&
        (m->payload[0] >> 2) != s->remote_seid)
        return fail(s, WL_AVDTP_SESSION_MALFORMED);
    s->last_reject_error = m->payload[expected - 1];
    if (m->signal == WL_AVDTP_SET_CONFIGURATION) s->last_reject_category = m->payload[0];
    return fail(s, WL_AVDTP_SESSION_REJECTED);
}

int wl_avdtp_session_receive(wl_avdtp_session *s, uint64_t generation,
                            const uint8_t *packet, size_t size) {
    wl_avdtp_message message;
    int result = epoch(s, generation);
    if (result) return result;
    if (!packet || size < 2)
        return fail(s, WL_AVDTP_SESSION_MALFORMED);
    if (packet[0] & 0x0c) return fail(s, WL_AVDTP_SESSION_UNSUPPORTED_FRAGMENT);
    if ((packet[0] & 3u) == WL_AVDTP_COMMAND) return fail(s, WL_AVDTP_SESSION_REMOTE_COMMAND);
    /* A readable response header is sufficient to reject an unrelated tuple.
     * Do not let its invalid signal, length or payload alter the live command.
     * Fragments and peer commands remain unsupported channel-wide events. */
    if (!s->pending || (packet[0] >> 4) != s->outstanding.transaction ||
        (packet[1] & 0x3fu) != s->outstanding.signal)
        return WL_AVDTP_SESSION_UNMATCHED_RESPONSE;
    if (size > WL_AVDTP_SESSION_MAX_PACKET || wl_avdtp_parse(packet, size, &message))
        return fail(s, WL_AVDTP_SESSION_MALFORMED);
    if (state_signal(s->state) != message.signal) return fail(s, WL_AVDTP_SESSION_INVALID_STATE);
    if (message.message_type != WL_AVDTP_ACCEPT) return reject_response(s, &message);
    if (message.signal == WL_AVDTP_DISCOVER)
        return discover_accept(s, message.payload, message.payload_size);
    if (message.signal == WL_AVDTP_GET_CAPABILITIES)
        return capability_accept(s, message.payload, message.payload_size);
    if (message.payload_size) return fail(s, WL_AVDTP_SESSION_MALFORMED);
    s->pending = 0;
    switch (message.signal) {
    case WL_AVDTP_SET_CONFIGURATION:
        s->configured = 1; s->state = WL_AVDTP_SESSION_OPEN; break;
    case WL_AVDTP_OPEN:
        s->state = WL_AVDTP_SESSION_WAIT_MEDIA; break;
    case WL_AVDTP_START:
        s->state = WL_AVDTP_SESSION_STREAMING; break;
    case WL_AVDTP_SUSPEND:
        s->state = WL_AVDTP_SESSION_SUSPENDED; break;
    case WL_AVDTP_CLOSE:
        s->state = WL_AVDTP_SESSION_WAIT_MEDIA_CLOSE; break;
    case WL_AVDTP_ABORT:
        s->state = WL_AVDTP_SESSION_ABORTED; s->transport_valid = 0; break;
    default: return fail(s, WL_AVDTP_SESSION_INVALID_STATE);
    }
    return WL_AVDTP_SESSION_OK;
}

int wl_avdtp_session_media_ready(wl_avdtp_session *s, uint64_t generation) {
    int result = epoch(s, generation);
    if (result) return result;
    if (s->pending || s->state != WL_AVDTP_SESSION_WAIT_MEDIA) return WL_AVDTP_SESSION_INVALID_STATE;
    s->state = WL_AVDTP_SESSION_START;
    return WL_AVDTP_SESSION_OK;
}
int wl_avdtp_session_request_suspend(wl_avdtp_session *s) {
    if (!s) return WL_AVDTP_SESSION_INVALID_ARGUMENT;
    if (!s->transport_valid) return WL_AVDTP_SESSION_TRANSPORT_INVALID;
    if (s->pending) return WL_AVDTP_SESSION_BUSY;
    if (s->state != WL_AVDTP_SESSION_STREAMING) return WL_AVDTP_SESSION_INVALID_STATE;
    s->state = WL_AVDTP_SESSION_SUSPEND; return WL_AVDTP_SESSION_OK;
}
int wl_avdtp_session_request_resume(wl_avdtp_session *s) {
    if (!s) return WL_AVDTP_SESSION_INVALID_ARGUMENT;
    if (!s->transport_valid) return WL_AVDTP_SESSION_TRANSPORT_INVALID;
    if (s->pending) return WL_AVDTP_SESSION_BUSY;
    if (s->state != WL_AVDTP_SESSION_SUSPENDED) return WL_AVDTP_SESSION_INVALID_STATE;
    s->state = WL_AVDTP_SESSION_START; return WL_AVDTP_SESSION_OK;
}
int wl_avdtp_session_request_abort(wl_avdtp_session *s) {
    if (!s) return WL_AVDTP_SESSION_INVALID_ARGUMENT;
    if (!s->transport_valid) return WL_AVDTP_SESSION_TRANSPORT_INVALID;
    if (s->pending) return WL_AVDTP_SESSION_BUSY;
    if (s->configured) s->state = WL_AVDTP_SESSION_ABORT;
    else {s->state = WL_AVDTP_SESSION_ABORTED; s->transport_valid = 0;}
    return WL_AVDTP_SESSION_OK;
}
int wl_avdtp_session_request_close(wl_avdtp_session *s) {
    if (!s) return WL_AVDTP_SESSION_INVALID_ARGUMENT;
    if (!s->transport_valid) return WL_AVDTP_SESSION_TRANSPORT_INVALID;
    if (s->pending) return WL_AVDTP_SESSION_BUSY;
    if (s->state != WL_AVDTP_SESSION_STREAMING && s->state != WL_AVDTP_SESSION_SUSPENDED &&
        s->state != WL_AVDTP_SESSION_START)
        return WL_AVDTP_SESSION_INVALID_STATE;
    s->state = WL_AVDTP_SESSION_CLOSE; return WL_AVDTP_SESSION_OK;
}
int wl_avdtp_session_media_closed(wl_avdtp_session *s, uint64_t generation) {
    int result = epoch(s, generation);
    if (result) return result;
    if (s->pending || s->state != WL_AVDTP_SESSION_WAIT_MEDIA_CLOSE)
        return WL_AVDTP_SESSION_INVALID_STATE;
    s->configured = 0; s->transport_valid = 0; s->state = WL_AVDTP_SESSION_CLOSED;
    return WL_AVDTP_SESSION_OK;
}
static int token_matches(const wl_avdtp_session *s, const wl_avdtp_session_token *token) {
    return s && token && s->transport_valid && s->pending &&
        token->generation == s->generation && token->generation == s->outstanding.generation &&
        token->serial == s->outstanding.serial && token->transaction == s->outstanding.transaction &&
        token->signal == s->outstanding.signal;
}
int wl_avdtp_session_timeout(wl_avdtp_session *s, const wl_avdtp_session_token *token) {
    if (!token_matches(s, token)) return WL_AVDTP_SESSION_TOKEN_MISMATCH;
    return fail(s, WL_AVDTP_SESSION_TIMEOUT);
}
int wl_avdtp_session_send_failed(wl_avdtp_session *s, const wl_avdtp_session_token *token) {
    if (!token_matches(s, token)) return WL_AVDTP_SESSION_TOKEN_MISMATCH;
    return fail(s, WL_AVDTP_SESSION_SEND_FAILED);
}
int wl_avdtp_session_transport_failed(wl_avdtp_session *s, uint64_t generation) {
    int result = epoch(s, generation);
    return result ? result : fail(s, WL_AVDTP_SESSION_TRANSPORT_FAILED);
}
