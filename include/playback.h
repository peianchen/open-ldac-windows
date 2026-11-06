/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_PLAYBACK_H
#define WL_PLAYBACK_H
#include "wldac.h"
#include "avdtp_session.h"

enum { WL_SIGNALING = 0, WL_MEDIA = 1 };
enum { WL_PLAY_OK=0, WL_PLAY_ARGUMENT=-1, WL_PLAY_TRANSPORT=-2,
       WL_PLAY_PROTOCOL=-3, WL_PLAY_SOURCE=-4, WL_PLAY_CODEC=-5,
       WL_PLAY_STOPPED=-6, WL_PLAY_BITRATE=-7, WL_PLAY_MTU=-8 };
typedef struct {
    void *context;
    /* 0=success; operations must drain their I/O before returning. Channels
     * share the same selected service interface, not arbitrary Bluetooth PSMs. */
    int (*open)(void *, unsigned channel, unsigned *out_mtu, unsigned *in_mtu);
    int (*send)(void *, unsigned channel, const uint8_t *, size_t, unsigned timeout_ms);
    int (*receive)(void *, unsigned channel, uint8_t *, size_t, size_t *, unsigned timeout_ms);
    /* Absolute source-sample offset from the first media packet. Must wait on
     * events and observe stop/peer disconnect. Lateness beyond the adapter's
     * documented tolerance must fail rather than drop audio/lower quality. */
    int (*wait_samples)(void *, uint64_t offset, unsigned sample_rate);
    /* EOF only: wait through both the nominal end and at least the final
     * packet duration after its successful transport completion. This is a
     * sender drain, not acknowledgement of receiver playout. Empty media uses
     * zero end/final samples. Observe stop/peer loss and retain signaling. */
    int (*finish_media)(void *, uint64_t end_offset, unsigned final_packet_samples, unsigned sample_rate);
    int (*stopped)(void *);
    /* Optional owner-thread read of an explicit user request; 0=no change.
     * Never derive it from AC/battery state. */
    unsigned (*requested_bitrate)(void *);
    void (*close)(void *, unsigned channel);
    /* Required monotonic milliseconds; one signaling transaction shares one
     * deadline across send, all fragments and unmatched responses. */
    uint64_t (*now_ms)(void *);
} wl_playback_io;
typedef struct {
    void *context;
    /* Supplies 0..128 interleaved stereo sample frames. Zero means EOF; only
     * the final short block is padded by the engine. Never overrun capacity. */
    int (*read)(void *, void *pcm, unsigned capacity_frames, unsigned *frames);
    unsigned rate;
    LDACBT_SMPL_FMT_T format;
} wl_pcm_source;
typedef struct {
    unsigned bitrate_kbps; /* 0=HQ for the exact source-rate family. */
    uint64_t generation;
    uint32_t ssrc;
    uint8_t local_seid;
} wl_playback_options;
typedef struct {
    int result, protocol_error, encoder_error;
    unsigned stage, negotiated_mtu, requested_kbps, actual_kbps;
    uint64_t source_frames, encoded_frames, media_packets, media_bytes;
    unsigned bitrate_changes;
} wl_playback_report;
/* Blocking owner-thread orchestration; the concrete I/O adapter waits on OS
 * events. No device/service installation, heap queue, timer or RF operation is
 * performed by this portable module. Always close media before signaling. */
int wl_playback_run(const wl_playback_io *, const wl_pcm_source *,
                    const wl_playback_options *, wl_playback_report *);
#endif
