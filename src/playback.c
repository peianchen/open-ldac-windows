/* SPDX-License-Identifier: Apache-2.0 */
#include "playback.h"
#include <string.h>

#define WL_PLAY_MAX_MTU 1008u
#define WL_PLAY_MIN_MTU 48u
#define WL_PLAY_TIMEOUT_MS 5000u
#define WL_PLAY_MAX_UNMATCHED 16u
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
static int exchange(const wl_playback_io *io, wl_avdtp_session *s,
                    unsigned in_mtu, wl_playback_report *r) {
    uint8_t command[18], response[WL_AVDTP_SESSION_MAX_PACKET];
    wl_avdtp_session_token token;
    size_t size;
    int result;
    r->stage = (unsigned)s->state;
    if (stopped(io)) return WL_PLAY_STOPPED;
    size = wl_avdtp_session_next_command(s, command, sizeof(command), &token);
    if (!size) return protocol_failure(r, s->last_error ? s->last_error : WL_AVDTP_SESSION_INVALID_STATE);
    if (io->send(io->context, WL_SIGNALING, command, size, WL_PLAY_TIMEOUT_MS)) {
        result = io_failure(io);
        if (result != WL_PLAY_STOPPED) r->protocol_error = wl_avdtp_session_send_failed(s, &token);
        return result;
    }
    for (unsigned i = 0; i < WL_PLAY_MAX_UNMATCHED; ++i) {
        size_t received = 0;
        if (stopped(io)) return WL_PLAY_STOPPED;
        if (io->receive(io->context, WL_SIGNALING, response, in_mtu, &received, WL_PLAY_TIMEOUT_MS)) {
            result = io_failure(io);
            if (result != WL_PLAY_STOPPED)
                r->protocol_error = wl_avdtp_session_transport_failed(s, s->generation);
            return result;
        }
        if (stopped(io)) return WL_PLAY_STOPPED;
        if (received > in_mtu) return protocol_failure(r, WL_AVDTP_SESSION_MALFORMED);
        result = wl_avdtp_session_receive(s, s->generation, response, received);
        if (result == WL_AVDTP_SESSION_UNMATCHED_RESPONSE) continue;
        return result ? protocol_failure(r, result) : WL_PLAY_OK;
    }
    (void)wl_avdtp_session_timeout(s, &token);
    return protocol_failure(r, WL_AVDTP_SESSION_TIMEOUT);
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
static int media_output(const wl_playback_io *io, wl_encoder *enc,
                        wl_rtp_state *rtp, const wl_encoded *encoded,
                        wl_playback_report *r, unsigned *last_packet_samples) {
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
    if (io->wait_samples(io->context, offset, enc->rate)) return io_failure(io);
    if (stopped(io)) return WL_PLAY_STOPPED;
    if (io->send(io->context, WL_MEDIA, packet, size, WL_PLAY_TIMEOUT_MS)) return io_failure(io);
    *last_packet_samples = (unsigned)encoded->frames * rtp->samples_per_frame;
    r->encoded_frames += (unsigned)encoded->frames;
    ++r->media_packets; r->media_bytes += size;
    return stopped(io) ? WL_PLAY_STOPPED : WL_PLAY_OK;
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
        !io->wait_samples || !io->finish_media || !io->stopped || !io->close || !source->read ||
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
        result = media_output(io, &encoder, &rtp, &encoded, report, &last_packet_samples);
        if (result) goto done;
    }
    for (unsigned i = 0; i < 32; ++i) {
        wl_encoded encoded;
        if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
        result = bitrate_request(io, &encoder, report);
        if (result) goto done;
        if (wl_encoder_push(&encoder, NULL, &encoded)) {result = codec_failure(report, &encoder); goto done;}
        report->actual_kbps = (unsigned)wl_encoder_bitrate(&encoder);
        if (!encoded.size) {drained = 1; break;}
        result = media_output(io, &encoder, &rtp, &encoded, report, &last_packet_samples);
        if (result) goto done;
    }
    if (!drained) {result = codec_failure(report, &encoder); goto done;}
    if (stopped(io)) {result = WL_PLAY_STOPPED; goto done;}
    /* Include codec lookahead/padding and a final packet whose write completed
     * late. Receiver buffering remains a separate physical validation gate. */
    if (io->finish_media(io->context, report->encoded_frames * rtp.samples_per_frame,
                         last_packet_samples, source->rate)) {
        result = io_failure(io); goto done;
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
    if (entered_io) {if (!media_closed) io->close(io->context, WL_MEDIA); io->close(io->context, WL_SIGNALING);}
    report->result = result;
    return result;
}
