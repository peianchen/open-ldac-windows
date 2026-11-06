/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_RECONNECT_H
#define WL_RECONNECT_H
#include "playback.h"

/* Explicit whole-source restart policy. No cursor/gapless claim is made: a
 * failed write may already have reached the receiver. Caller retains the same
 * selected transport identity, and all attempt I/O is retired before retry. */
typedef struct {
    void *context;
    unsigned max_retries; /* 0 disables; at most 3 after the initial attempt. */
    int (*rewind)(void *); /* Rewind the existing read-only source; 0 success. */
    int (*wait_retry)(void *, unsigned milliseconds); /* Event wait, stoppable. */
    /* Positive only for an observed link/device loss, not a timeout, malformed
     * protocol, resource failure or ambiguous generic I/O error. */
    int (*retryable)(void *, int result, const wl_playback_report *);
} wl_reconnect_policy;
typedef struct {
    unsigned attempts, restarts;
    int exhausted, generation_exhausted;
    uint64_t last_generation;
} wl_reconnect_report;
/* last_report describes only the last attempt, not aggregate media delivery.
 * Fresh encoder/session/RTP per attempt; requested numeric bitrate survives.
 * Backoff is 1000/2000/4000 ms with no busy polling. */
int wl_playback_reconnect(const wl_playback_io *, const wl_pcm_source *,
    const wl_playback_options *, const wl_reconnect_policy *,
    wl_playback_report *last_report, wl_reconnect_report *);
#endif
