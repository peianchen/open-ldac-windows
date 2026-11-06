/* SPDX-License-Identifier: Apache-2.0 */
#include "reconnect.h"
#include <string.h>

int wl_playback_reconnect(const wl_playback_io *io, const wl_pcm_source *source,
    const wl_playback_options *options, const wl_reconnect_policy *policy,
    wl_playback_report *last, wl_reconnect_report *report) {
    wl_playback_options next;
    int result;
    if (!last || !report) return WL_PLAY_ARGUMENT;
    memset(last, 0, sizeof(*last)); last->result = WL_PLAY_ARGUMENT;
    memset(report, 0, sizeof(*report));
    if (!io || !io->stopped || !source || !options || !policy || policy->max_retries > 3 ||
        (policy->max_retries && (!policy->rewind || !policy->wait_retry || !policy->retryable)))
        return WL_PLAY_ARGUMENT;
    next = *options;
    for (;;) {
        if (io->stopped(io->context)) { result = WL_PLAY_STOPPED; break; }
        ++report->attempts; report->last_generation = next.generation;
        result = wl_playback_run(io, source, &next, last);
        /* run retires every entered handle before returning. Never rewind a
         * live encoder or resend an uncertain media packet on the old epoch. */
        if (io->stopped(io->context)) { result = WL_PLAY_STOPPED; break; }
        if (!policy->max_retries || result != WL_PLAY_TRANSPORT ||
            policy->retryable(policy->context, result, last) <= 0) break;
        if (report->restarts == policy->max_retries) { report->exhausted = 1; break; }
        if (next.generation == UINT64_MAX) { report->generation_exhausted = 1; break; }
        if (policy->wait_retry(policy->context, 1000u << report->restarts)) {
            result = io->stopped(io->context) ? WL_PLAY_STOPPED : WL_PLAY_TRANSPORT; break;
        }
        if (io->stopped(io->context)) { result = WL_PLAY_STOPPED; break; }
        if (policy->rewind(policy->context)) {
            result = io->stopped(io->context) ? WL_PLAY_STOPPED : WL_PLAY_SOURCE; break;
        }
        if (io->stopped(io->context)) { result = WL_PLAY_STOPPED; break; }
        if (last->requested_kbps) next.bitrate_kbps = last->requested_kbps;
        ++next.generation; ++next.ssrc; ++report->restarts;
    }
    last->result = result;
    return result;
}
