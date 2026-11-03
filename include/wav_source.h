/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_WAV_SOURCE_H
#define WL_WAV_SOURCE_H
#include <stdio.h>
#include "playback.h"

/* Zero-initialize before first open; close before reusing. This object owns its
 * read-only FILE and must outlive pcm.context. No conversion/resampling occurs.
 * Open checks the entire RIFF structure and physical file bounds, not every
 * sample value. Float NaN/Inf and subsequent I/O errors are rejected on read. */
typedef struct {
    FILE *file;
    wl_pcm_source pcm;
    uint64_t data_offset, data_bytes, remaining_bytes;
    unsigned block_align;
} wl_wav_source;
/* UTF-8 path on Windows (converted strictly to UTF-16), native bytes elsewhere.
 * Returns zero on success. On failure the object remains closed. */
int wl_wav_open(wl_wav_source *, const char *utf8_path);
void wl_wav_close(wl_wav_source *);
#endif
