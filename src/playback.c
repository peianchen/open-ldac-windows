/* SPDX-License-Identifier: Apache-2.0 */
#include "playback.h"
#include "avdtp_reassembly.h"
#include <string.h>

#define WL_PLAY_MAX_MTU 1008u
#define WL_PLAY_MIN_MTU 48u
#define WL_PLAY_TIMEOUT_MS 5000u
#define WL_PLAY_MAX_UNMATCHED 16u
#define WL_PLAY_MAX_WIRE_PACKETS 64u
#define WL_PLAY_MAX_SETUP (WL_AVDTP_SESSION_MAX_SEPS + 8u)

static unsigned sample_bytes(LDACBT_SMPL_FMT_T format) {
    switch (format) {
    case LDACBT_SMPL_FMT_S16: return 2;
    case LDACBT_SMPL_FMT_S24: return 3;
    case LDACBT_SMPL_FMT_S32:
    case LDACBT_SMPL_FMT_F32: return 4;
    default: return 0;
    }
}
static int stopped(const wl_playback_io *io) { return io->stopped(io->context) != 0; }
static int io_failure(const wl_playback_io *io) {
    return stopped(io) ? WL_PLAY_STOPPED : WL_PLAY_TRANSPORT;
}
static int protocol_failure(wl_playback_report *r, int error) {
    r->protocol_error = error; return WL_PLAY_PROTOCOL;
}
static int codec_failure(wl_playback_report *r, const wl_encoder *enc) {
    r->encoder_error = enc->library_error ? enc->library_error : -1;
    return WL_PLAY_CODEC;
}
static int valid_mtu(unsigned out, unsigned in, int media) {
    return out >= (media ? 679u : WL_PLAY_MIN_MTU) && out <= WL_PLAY_MAX_MTU &&
        in >= WL_PLAY_MIN_MTU && in <= WL_PLAY_MAX_MTU;
}
static int exchange_timeout(wl_avdtp_session *s,
                            const wl_avdtp_session_token *token,
                            wl_playback_report *r) {
    (void)wl_avdtp_session_timeout(s, token);
    return protocol_failure(r, WL_AVDTP_SESSION_TIMEOUT);
}
static unsigned remaining_ms(const wl_playback_io *io, uint64_t started) {
    /* A clock earlier than the transaction start fails closed by unsigned
     * subtraction. The adapter contract requires all readings monotonic. */
    uint64_t elapsed = io->now_ms(io->context) - started;
    return elapsed >= WL_PLAY_TIMEOUT_MS ? 0u : WL_PLAY_TIMEOUT_MS - (unsigned)elapsed;
}
static int exchange(const wl_playback_io *io, wl_avdtp_session *s,
                    unsigned in_mtu, wl_playback_report *r) {
    uint8_t command[18], response[WL_PLAY_MAX_MTU];
    wl_avdtp_reassembly assembly;
    wl_avdtp_session_token token;
    uint64_t started;
    size_t size;
    unsigned unmatched = 0;
    int result;
    r->stage = (unsigned)s->state;
    if (stopped(io)) return WL_PLAY_STOPPED;
    size = wl_avdtp_session_next_command(s, command, sizeof(command), &token);
    if (!size) return protocol_failure(r, s->last_error ? s->last_error : WL_AVDTP_SESSION_INVALID_STATE);
    wl_avdtp_reassembly_reset(&assembly);
    started = io->now_ms(io->context);
    if (io->send(io->context, WL_SIGNALING, command, size, WL_PLAY_TIMEOUT_MS)) {
        result = io_failure(io);
        if (result != WL_PLAY_STOPPED) r->protocol_error = wl_avdtp_session_send_failed(s, &token);
        return result;
    }
    for (unsigned i = 0; i < WL_PLAY_MAX_WIRE_PACKETS; ++i) {
        const uint8_t *message = NULL;
        size_t received = 0, message_size = 0;
        unsigned remaining;
        if (stopped(io)) return WL_PLAY_STOPPED;
        remaining = remaining_ms(io, started);
        if (!remaining) return exchange_timeout(s, &token, r);
        if (io->receive(io->context, WL_SIGNALING, response, in_mtu, &received, remaining)) {
            result = io_failure(io);
            if (result != WL_PLAY_STOPPED)
                r->protocol_error = wl_avdtp_session_transport_failed(s, s->generation);
            return result;
        }
        if (stopped(io)) return WL_PLAY_STOPPED;
        if (!remaining_ms(io, started)) return exchange_timeout(s, &token, r);
        if (received > in_mtu) {
            (void)wl_avdtp_session_transport_failed(s, s->generation);
            return protocol_failure(r, WL_AVDTP_SESSION_MALFORMED);
        }
        result = wl_avdtp_reassembly_push(&assembly, response, received, &message, &message_size);
        if (result == WL_AVDTP_REASSEMBLY_MORE) continue;
        if (result != WL_AVDTP_REASSEMBLY_COMPLETE) {
            (void)wl_avdtp_session_transport_failed(s, s->generation);
            return protocol_failure(r, WL_AVDTP_SESSION_MALFORMED);
        }
        result = wl_avdtp_session_receive(s, s->generation, message, message_size);
        if (result == WL_AVDTP_SESSION_UNMATCHED_RESPONSE) {
            if (++unmatched >= WL_PLAY_MAX_UNMATCHED) return exchange_timeout(s, &token, r);
            continue;
        }
        return result ? protocol_failure(r, result) : WL_PLAY_OK;
    }
    return exchange_timeout(s, &token, r);
}
static int drive(const wl_playback_io *io, wl_avdtp_session *s,
                 wl_avdtp_session_state goal, unsigned in_mtu,
                 wl_playback_report *r) {
    for (unsigned i = 0; i < WL_PLAY_MAX_SETUP; ++i) {
        int result;
        if (s->state == goal) return WL_PLAY_OK;
        result = exchange(io, s, in_mtu, r);
        if (result) return result;
    }
    return protocol_failure(r, WL_AVDTP_SESSION_INVALID_STATE);
}
static int bitrate_request(const wl_playback_io *io, wl_encoder *enc,
                           wl_playback_report *r) {
    unsigned requested;
    if (!io->requested_bitrate) return WL_PLAY_OK;
    requested = io->requested_bitrate(io->context);
    if (stopped(io)) return WL_PLAY_STOPPED;
    if (!requested || requested == r->requested_kbps) return WL_PLAY_OK;
    r->requested_kbps = requested;
    if (wl_bitrate_eqmid(enc->rate, requested) < 0) return WL_PLAY_BITRATE;
    if (wl_encoder_set_bitrate(enc, requested)) {
        r->encoder_error = enc->library_error; return WL_PLAY_BITRATE;
    }
    ++r->bitrate_changes;
    return WL_PLAY_OK;
}
/* All control processing runs on the stream owner. A suspend ACK is sent
 * only after the previous media write retired. No PCM is read while paused. */
static int peer_message(const wl_playback_io *io, wl_avdtp_session *session,
                         unsigned mtu, wl_playback_report *r) {
    uint8_t packet[WL_PLAY_MAX_MTU];
    wl_avdtp_reassembly assembly;
    wl_avdtp_peer_reply reply;
    uint64_t started = io->now_ms(io->context);
    wl_avdtp_reassembly_reset(&assembly);
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t *message = NULL;
        size_t size = 0, length = 0;
        unsigned remaining = remaining_ms(io, started);
        int status;
        if (stopped(io)) return WL_PLAY_STOPPED;
        if (!remaining) return protocol_failure(r, WL_AVDTP_SESSION_TIMEOUT);
        if (io->receive(io->context, WL_SIGNALING, packet, mtu, &size, remaining))
            return io_failure(io);
        if (stopped(io)) return WL_PLAY_STOPPED;
        if (!remaining_ms(io, started)) return protocol_failure(r, WL_AVDTP_SESSION_TIMEOUT);
        if (size > mtu) return protocol_failure(r, WL_AVDTP_SESSION_MALFORMED);
        status = wl_avdtp_reassembly_push(&assembly, packet, size, &message, &length);
        if (status == WL_AVDTP_REASSEMBLY_MORE) continue;
        if (status != WL_AVDTP_REASSEMBLY_COMPLETE)
            return protocol_failure(r, WL_AVDTP_SESSION_MALFORMED);
        status = wl_avdtp_session_peer_prepare(session, session->generation, message, length, &reply);
        if (status) return protocol_failure(r, status);
        ++r->peer_commands;
        if (!reply.size) return WL_PLAY_OK; /* Spec: invalid-SEID ABORT is ignored. */
        if (reply.action == WL_AVDTP_PEER_SUSPEND && io->pause_clock(io->context, 1))
            return io_failure(io);
        remaining = remaining_ms(io, started);
        if (!remaining) return protocol_failure(r, WL_AVDTP_SESSION_TIMEOUT);
        if (io->send(io->context, WL_SIGNALING, reply.bytes, reply.size, remaining))
            return io_failure(io);
        if (stopped(io)) return WL_PLAY_STOPPED;
        if (!remaining_ms(io, started)) return protocol_failure(r, WL_AVDTP_SESSION_TIMEOUT);
        status = wl_avdtp_session_peer_commit(session, &reply);
        if (status) return protocol_failure(r, status);
        r->stage = (unsigned)session->state;
        switch (reply.action) {
        case WL_AVDTP_PEER_SUSPEND: ++r->peer_suspends; break;
        case WL_AVDTP_PEER_START:
            if (io->pause_clock(io->context, 0)) return io_failure(io);
            ++r->peer_resumes; break;
        case WL_AVDTP_PEER_CLOSE: return WL_PLAY_PEER_CLOSED;
        case WL_AVDTP_PEER_ABORT: return WL_PLAY_PEER_ABORTED;
        default: ++r->peer_rejects; break;
        }
        return WL_PLAY_OK;
    }
    return protocol_failure(r, WL_AVDTP_SESSION_MALFORMED);
}
static int peer_controls(const wl_playback_io *io, wl_avdtp_session *session,
                          unsigned mtu, wl_playback_report *r) {
    unsigned commands = 0;
    if (stopped(io)) return WL_PLAY_STOPPED;
    if (!io->poll_control) return WL_PLAY_OK;
    for (;;) {
        int ready, status;
        if (stopped(io)) return WL_PLAY_STOPPED;
        ready = io->poll_control(io->context,
            session->state == WL_AVDTP_SESSION_SUSPENDED ? 5000u : 0u);
        if (stopped(io)) return WL_PLAY_STOPPED;
        if (ready < 0) return io_failure(io);
        if (!ready) {
            if (session->state == WL_AVDTP_SESSION_SUSPENDED) { commands = 0; continue; }
            return WL_PLAY_OK;
        }
        /* A continuously ready peer must not starve media indefinitely. Quiet
         * suspended event waits do not consume the bounded command budget. */
        if (++commands > 64) return protocol_failure(r, WL_AVDTP_SESSION_BUSY);
        status = peer_message(io, session, mtu, r);
        if (status) return status;
    }
}
static int media_output(const wl_playback_io *io, wl_encoder *enc,
                        wl_rtp_state *rtp, const wl_encoded *encoded,
                        wl_playback_report *r, unsigned *last_packet_samples,
                        wl_avdtp_session *session, unsigned signaling_mtu) {
    uint8_t packet[WL_PLAY_MAX_MTU];
    size_t size;
    uint64_t offset;
    if (!encoded->size) return WL_PLAY_OK; /* No packet or timeline advancement. */
    if (r->encoded_frames > UINT64_MAX / rtp->samples_per_frame ||
        (uint64_t)encoded->frames > UINT64_MAX / rtp->samples_per_frame - r->encoded_frames)
        return codec_failure(r, enc);
    offset = r->encoded_frames * rtp->samples_per_frame;
    size = wl_rtp_packet(rtp, encoded->bytes, (size_t)encoded->size,
        (unsigned)encoded->frames, r->negotiated_mtu, packet, sizeof(packet));
    if (!size || r->media_packets == UINT64_MAX || r->media_bytes > UINT64_MAX - size)
        return codec_failure(r, enc);
    if (stopped(io)) return WL_PLAY_STOPPED;
    for (;;) {
        int status = peer_controls(io, session, signaling_mtu, r);
        if (status) return status;
        status = io->wait_samples(io->context, offset, enc->rate);
        if (status == WL_PLAY_IO_CONTROL && io->poll_control) continue;
        if (status) return io_failure(io);
        break;
    }
    if (stopped(io)) return WL_PLAY_STOPPED;
    if (io->send(io->context, WL_MEDIA, packet, size, WL_PLAY_TIMEOUT_MS)) return io_failure(io);
    *last_packet_samples = (unsigned)encoded->frames * rtp->samples_per_frame;
    r->encoded_frames += (unsigned)encoded->frames;
    ++r->media_packets; r->media_bytes += size;
    return peer_controls(io, session, signaling_mtu, r);
}

int wl_playback_run(const wl_playback_io *io, const wl_pcm_source *source,
                    const wl_playback_options *options, wl_playback_report *report) {
    wl_avdtp_session session = {0};
    wl_encoder encoder = {0};
    wl_rtp_state rtp = {0};
    _Alignas(16) uint8_t pcm[128u * 2u * 4u];
    unsigned bytes, signaling_out = 0, signaling_in = 0, media_out = 0, media_in = 0;
    unsigned last_packet_samples = 0;
    int result = WL_PLAY_ARGUMENT, mode, entered_io = 0, media_closed = 0, padded = 0, drained = 0;
    if (!report) return WL_PLAY_ARGUMENT;
    memset(report, 0, sizeof(*report)); report->result = WL_PLAY_ARGUMENT;
    if (!io || !source || !options || !io->open || !io->send || !io->receive ||
        ((io->poll_control == NULL) != (io->pause_clock == NULL)) ||
        !io->now_ms || !io->wait_samples || !io->finish_media || !io->stopped || !io->close || !source->read ||
        !(bytes = sample_bytes(source->format)) || !wl_bitrate_kbps(source->rate, LDACBT_EQMID_HQ))
        return WL_PLAY_ARGUMENT;
    report->requested_kbps = options->bitrate_kbps ? options->bitrate_kbps : wl_bitrate_kbps(source->rate, LDACBT_EQMID_HQ);
    mode = wl_bitrate_eqmid(source->rate, report->requested_kbps);
    if (mode < 0) {report->result = WL_PLAY_BITRATE; return WL_PLAY_BITRATE;}
    if (wl_avdtp_session_init(&session, options->generation, options->local_seid, source->rate))
        return WL_PLAY_ARGUMENT;
    report->stage = (unsigned)session.state;
    entered_io = 1;
    if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
    if (io->open(io->context, WL_SIGNALING, &signaling_out, &signaling_in)) {result = io_failure(io); goto done;}
    if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
    if (!valid_mtu(signaling_out, signaling_in, 0)) {result = WL_PLAY_MTU; goto done;}
    result = drive(io, &session, WL_AVDTP_SESSION_WAIT_MEDIA, signaling_in, report);
    if (result) goto done;
    report->stage = (unsigned)session.state;
    if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
    if (io->open(io->context, WL_MEDIA, &media_out, &media_in)) {result = io_failure(io); goto done;}
    if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
    report->negotiated_mtu = media_out;
    if (!valid_mtu(media_out, media_in, 1)) {result = WL_PLAY_MTU; goto done;}
    if (wl_encoder_open(&encoder, source->rate, source->format, mode, media_out)) {
        result = codec_failure(report, &encoder); goto done;
    }
    report->actual_kbps = (unsigned)wl_encoder_bitrate(&encoder);
    if (wl_avdtp_session_media_ready(&session, options->generation)) {
        result = protocol_failure(report, WL_AVDTP_SESSION_INVALID_STATE); goto done;
    }
    result = drive(io, &session, WL_AVDTP_SESSION_STREAMING, signaling_in, report);
    if (result) goto done;
    report->stage = (unsigned)session.state;
    rtp.ssrc = options->ssrc; rtp.payload_type = 96;
    rtp.samples_per_frame = source->rate > 48000 ? 256u : 128u;
    for (;;) {
        unsigned frames = 0;
        wl_encoded encoded;
        if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
        result = peer_controls(io, &session, signaling_in, report);
        if (result) goto done;
        result = bitrate_request(io, &encoder, report);
        if (result) goto done;
        if (source->read(source->context, pcm, 128, &frames)) {
            result = stopped(io) ? WL_PLAY_STOPPED : WL_PLAY_SOURCE; goto done;
        }
        if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
        if (frames > 128 || (padded && frames) || report->source_frames > UINT64_MAX - frames) {
            result = WL_PLAY_SOURCE; goto done;
        }
        if (!frames) break;
        report->source_frames += frames;
        if (frames < 128) {
            memset(pcm + frames * 2u * bytes, 0, (128u - frames) * 2u * bytes); padded = 1;
        }
        if (wl_encoder_push(&encoder, pcm, &encoded)) {result = codec_failure(report, &encoder); goto done;}
        report->actual_kbps = (unsigned)wl_encoder_bitrate(&encoder);
        result = media_output(io, &encoder, &rtp, &encoded, report, &last_packet_samples, &session, signaling_in);
        if (result) goto done;
    }
    for (unsigned i = 0; i < 32; ++i) {
        wl_encoded encoded;
        if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
        result = peer_controls(io, &session, signaling_in, report);
        if (result) goto done;
        result = bitrate_request(io, &encoder, report);
        if (result) goto done;
        if (wl_encoder_push(&encoder, NULL, &encoded)) {result = codec_failure(report, &encoder); goto done;}
        report->actual_kbps = (unsigned)wl_encoder_bitrate(&encoder);
        if (!encoded.size) {drained = 1; break;}
        result = media_output(io, &encoder, &rtp, &encoded, report, &last_packet_samples, &session, signaling_in);
        if (result) goto done;
    }
    if (!drained) {result = codec_failure(report, &encoder); goto done;}
    if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
    /* Include codec lookahead/padding and a final packet whose write completed
     * late. Receiver buffering remains a separate physical validation gate. */
    for (;;) {
        result = peer_controls(io, &session, signaling_in, report);
        if (result) goto done;
        int status = io->finish_media(io->context, report->encoded_frames * rtp.samples_per_frame,
                                     last_packet_samples, source->rate);
        if (status == WL_PLAY_IO_CONTROL && io->poll_control) continue;
        if (status) { result = io_failure(io); goto done; }
        break;
    }
    if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
    if (wl_avdtp_session_request_close(&session)) {
        result = protocol_failure(report, WL_AVDTP_SESSION_INVALID_STATE); goto done;
    }
    result = drive(io, &session, WL_AVDTP_SESSION_WAIT_MEDIA_CLOSE, signaling_in, report);
    if (!result) {
        io->close(io->context, WL_MEDIA); media_closed = 1;
        if (wl_avdtp_session_media_closed(&session, options->generation))
            result = protocol_failure(report, WL_AVDTP_SESSION_INVALID_STATE);
    }
    if (!result) report->stage = (unsigned)session.state;
done:
    wl_encoder_close(&encoder);
    if (result == WL_PLAY_PEER_CLOSED) {
        io->close(io->context, WL_MEDIA); media_closed = 1;
        if (wl_avdtp_session_media_closed(&session, options->generation))
            result = protocol_failure(report, WL_AVDTP_SESSION_INVALID_STATE);
        report->stage = (unsigned)session.state;
    }
    if (entered_io) {if (!media_closed) io->close(io->context, WL_MEDIA); io->close(io->context, WL_SIGNALING);}
    report->result = result;
    return result;
}
