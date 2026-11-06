/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_WIN_NATIVE_IO_H
#define WL_WIN_NATIVE_IO_H
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "playback.h"
typedef struct wl_win_native_io wl_win_native_io;
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
#endif
#endif
