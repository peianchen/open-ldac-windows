/* SPDX-License-Identifier: Apache-2.0 */
#include "wav_source.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static unsigned read16(const unsigned char *p) {
    return p[0] | ((unsigned)p[1] << 8);
}
static uint32_t read32(const unsigned char *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static int seek_to(FILE *file, uint64_t offset) {
#ifdef _WIN32
    return offset > INT64_MAX ? -1 : _fseeki64(file, (__int64)offset, SEEK_SET);
#else
    return offset > LONG_MAX ? -1 : fseek(file, (long)offset, SEEK_SET);
#endif
}
static int length_of(FILE *file, uint64_t *length) {
#ifdef _WIN32
    __int64 end;
    if (_fseeki64(file, 0, SEEK_END) || (end = _ftelli64(file)) < 0) return -1;
#else
    long end;
    if (fseek(file, 0, SEEK_END) || (end = ftell(file)) < 0) return -1;
#endif
    *length = (uint64_t)end;
    return seek_to(file, 0);
}
static FILE *open_readonly(const char *path) {
#ifdef _WIN32
    FILE *file;
    wchar_t *wide;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!count || !(wide = malloc((size_t)count * sizeof(*wide)))) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide); return NULL;
    }
    file = _wfopen(wide, L"rb");
    free(wide);
    return file;
#else
    return fopen(path, "rb");
#endif
}
static int read_pcm(void *context, void *pcm, unsigned capacity, unsigned *frames) {
    wl_wav_source *source = context;
    uint64_t available;
    size_t bytes;
    if (!frames) return -1;
    *frames = 0;
    if (!source || !source->file || !pcm || !capacity || capacity > 128) return -1;
    available = source->remaining_bytes / source->block_align;
    if (available > capacity) available = capacity;
    bytes = (size_t)available * source->block_align;
    if (bytes && fread(pcm, 1, bytes, source->file) != bytes) return -1;
    if (source->pcm.format == LDACBT_SMPL_FMT_F32) {
        const unsigned char *p = pcm;
        /* IEEE-754 binary32 exponent all ones denotes either NaN or infinity.
         * Test the WAV bytes directly without raising floating-point exceptions. */
        for (size_t i = 0; i < bytes; i += 4)
            if ((read32(p + i) & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) return -1;
    }
    source->remaining_bytes -= bytes;
    *frames = (unsigned)available;
    return 0;
}
int wl_wav_rewind(wl_wav_source *source) {
    if (!source || !source->file || !source->block_align ||
        !source->data_bytes || source->data_bytes % source->block_align ||
        source->remaining_bytes > source->data_bytes ||
        source->remaining_bytes % source->block_align ||
        source->data_offset > UINT64_MAX - source->data_bytes) return -1;
    if (seek_to(source->file, source->data_offset)) return -1;
    clearerr(source->file);
    source->remaining_bytes = source->data_bytes;
    return 0;
}
void wl_wav_close(wl_wav_source *source) {
    if (!source) return;
    if (source->file) fclose(source->file);
    memset(source, 0, sizeof(*source));
}
int wl_wav_open(wl_wav_source *source, const char *path) {
    unsigned char header[12], chunk[8], fmt[18];
    uint64_t length, end, position;
    int have_format = 0, have_data = 0;
    wl_wav_source candidate = {0};
    if (!source || !path || !*path || source->file) return -1;
    memset(source, 0, sizeof(*source));
    candidate.file = open_readonly(path);
    if (!candidate.file || length_of(candidate.file, &length) || length < 12 ||
        fread(header, 1, 12, candidate.file) != 12 || memcmp(header, "RIFF", 4) ||
        memcmp(header + 8, "WAVE", 4) || read32(header + 4) < 4) goto bad;
    end = (uint64_t)read32(header + 4) + 8;
    if (end > length) goto bad;
    position = 12;
    while (position < end) {
        uint32_t size;
        uint64_t padded, content;
        if (end - position < 8 || seek_to(candidate.file, position) ||
            fread(chunk, 1, 8, candidate.file) != 8) goto bad;
        size = read32(chunk + 4);
        content = position + 8;
        padded = (uint64_t)size + (size & 1u);
        if (padded > end - content) goto bad;
        if (!memcmp(chunk, "fmt ", 4)) {
            unsigned tag, bits, rate, align;
            if (have_format || (size != 16 && size != 18) ||
                fread(fmt, 1, size, candidate.file) != size) goto bad;
            if (size == 18 && read16(fmt + 16) != 0) goto bad;
            tag = read16(fmt); bits = read16(fmt + 14);
            rate = read32(fmt + 4); align = read16(fmt + 12);
            if (read16(fmt + 2) != 2 ||
                (rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000)) goto bad;
            if (tag == 1 && bits == 16) candidate.pcm.format = LDACBT_SMPL_FMT_S16;
            else if (tag == 1 && bits == 24) candidate.pcm.format = LDACBT_SMPL_FMT_S24;
            else if (tag == 1 && bits == 32) candidate.pcm.format = LDACBT_SMPL_FMT_S32;
            else if (tag == 3 && bits == 32) candidate.pcm.format = LDACBT_SMPL_FMT_F32;
            else goto bad; /* Compressed and extensible WAV are explicit errors. */
            if (align != 2 * bits / 8 || (uint64_t)rate * align != read32(fmt + 8)) goto bad;
            candidate.pcm.rate = rate;
            candidate.block_align = align;
            have_format = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (have_data || !size) goto bad;
            candidate.data_offset = content;
            candidate.data_bytes = size;
            have_data = 1;
        }
        position = content + padded;
    }
    if (!have_format || !have_data || candidate.data_bytes % candidate.block_align ||
        seek_to(candidate.file, candidate.data_offset)) goto bad;
    candidate.remaining_bytes = candidate.data_bytes;
    candidate.pcm.read = read_pcm;
    *source = candidate;
    source->pcm.context = source;
    return 0;
bad:
    wl_wav_close(&candidate);
    return -1;
}
