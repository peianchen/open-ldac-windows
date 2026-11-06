/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_WIN_NATIVE_IO_H
#define WL_WIN_NATIVE_IO_H
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "playback.h"
typedef struct wl_win_native_io wl_win_native_io;
#define WL_WIN_NATIVE_MAX_LATENESS_MS 40u
enum { WL_WIN_TIMEOUT_NONE=0, WL_WIN_TIMEOUT_PACING=1,
       WL_WIN_TIMEOUT_IO_WAIT=2, WL_WIN_TIMEOUT_IO_COMPLETION=3 };
/* Bounded owner-thread aggregates for this adapter's complete lifetime, across
 * reconnect attempts and close/open cycles. No per-packet log or allocation.
 * Durations are QPC observations, not RF latency or receiver playout timing. */
typedef struct {
    uint64_t peak_pacing_lateness_us, pacing_deadline_misses, last_pacing_miss_offset;
    uint64_t io_wait_timeouts, io_completion_timeout_errors;
    uint64_t media_write_calls, media_write_timing_samples, max_media_write_us;
    unsigned last_timeout_source;
    int high_resolution_timer, timer_fallback_used;
    DWORD high_resolution_timer_error;
    uint64_t pacing_timer_wake_samples, peak_timer_wake_lateness_us, peak_before_wait_lateness_us;
    uint64_t pacing_misses_after_timer_wake, pacing_misses_before_timer_wait;
} wl_win_native_timing;

/* Creates user-mode state only. No channel is opened until playback open().
 * NULL interface_path selects exactly one enumerated index (0..15). A supplied
 * path is for the isolated lab harness; callers never publish device paths.
 * A supplied stop_event must be manual-reset and stay signaled until the owner
 * has returned and destroyed this adapter. Caller owns that event's lifetime. */
wl_win_native_io *wl_win_native_create(unsigned index, const wchar_t *interface_path,
                                      HANDLE stop_event, volatile LONG *requested_kbps);
void wl_win_native_bind(wl_win_native_io *, wl_playback_io *);
void wl_win_native_destroy(wl_win_native_io *);
DWORD wl_win_native_last_error(const wl_win_native_io *);
/* Read only after the owner returns (before destroy), or on the owner thread.
 * Returns 0 on success; invalid input returns -1 and clears a non-NULL output. */
int wl_win_native_get_timing(const wl_win_native_io *, wl_win_native_timing *);
/* Pure classification of positively identified link-loss errors only. The
 * caller must also require WL_PLAY_TRANSPORT and observe its stop event.
 * ERROR_OPERATION_ABORTED is ambiguous and never sufficient to retry. */
int wl_win_native_retryable_error(DWORD error);
#endif
#endif
