/* SPDX-License-Identifier: Apache-2.0 */
#include "wav_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
static unsigned checks, sequence;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); return 1; } } while (0)

static void put16(unsigned char *p, unsigned n) {
    p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8);
}
static void put32(unsigned char *p, uint32_t n) {
    put16(p, n & 65535); put16(p + 2, n >> 16);
}
static size_t fixture(unsigned char *p, unsigned rate, unsigned tag,
                      unsigned bits, unsigned frames) {
    unsigned align = 2 * bits / 8, bytes = align * frames;
    memset(p, 0, 2048);
    memcpy(p, "RIFF", 4); put32(p + 4, 36 + bytes);
    memcpy(p + 8, "WAVEfmt ", 8); put32(p + 16, 16);
    put16(p + 20, tag); put16(p + 22, 2); put32(p + 24, rate);
    put32(p + 28, rate * align); put16(p + 32, align); put16(p + 34, bits);
    memcpy(p + 36, "data", 4); put32(p + 40, bytes);
    for (unsigned i = 0; i < bytes; ++i) p[44 + i] = tag == 3 ? 0 : (unsigned char)i;
    return 44 + bytes;
}
/* Exclusively create a small fixture in the test working directory. Never
 * overwrite an existing file. A UTF-8 suffix also exercises Windows paths. */
static FILE *new_fixture(char *path, size_t capacity) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
#ifdef _WIN32
        wchar_t wide[256];
        unsigned long pid = GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        snprintf(path, capacity, ".wav-source-test-%lu-%lu-%u-\xe6\xb8\xac\xe8\xa9\xa6.tmp",
                 pid, (unsigned long)time(NULL), ++sequence);
#ifdef _WIN32
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 256)) return NULL;
        FILE *file = _wfopen(wide, L"wbx");
#else
        FILE *file = fopen(path, "wbx");
#endif
        if (file) return file;
    }
    return NULL;
}
static int delete_fixture(const char *path) {
#ifdef _WIN32
    wchar_t wide[256];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 256)) return -1;
    return _wremove(wide);
#else
    return remove(path);
#endif
}
static int saved_fixture(const unsigned char *data, size_t length, char path[256]) {
    FILE *file = new_fixture(path, 256);
    int failed;
    if (!file) return -1;
    failed = fwrite(data, 1, length, file) != length;
    if (fclose(file)) failed = 1;
    if (failed) delete_fixture(path);
    return failed ? -1 : 0;
}
static int opening(const unsigned char *data, size_t length, int valid) {
    char path[256];
    wl_wav_source source = {0};
    int status, removed;
    CHECK(!saved_fixture(data, length, path));
    status = wl_wav_open(&source, path);
    wl_wav_close(&source);
    removed = delete_fixture(path);
    CHECK(!removed);
    CHECK(valid ? status == 0 : status != 0);
    CHECK(source.file == NULL && source.pcm.read == NULL);
    return 0;
}
static int valid_formats(void) {
    const unsigned rates[] = {44100, 48000, 88200, 96000};
    const unsigned tags[] = {1, 1, 1, 3}, bits[] = {16, 24, 32, 32};
    const LDACBT_SMPL_FMT_T formats[] = {LDACBT_SMPL_FMT_S16, LDACBT_SMPL_FMT_S24,
                                       LDACBT_SMPL_FMT_S32, LDACBT_SMPL_FMT_F32};
    unsigned char bytes[2048], output[1026];
    for (unsigned r = 0; r < 4; ++r) for (unsigned f = 0; f < 4; ++f) {
        char path[256];
        unsigned count = 999;
        wl_wav_source source = {0};
        size_t size = fixture(bytes, rates[r], tags[f], bits[f], 129);
        CHECK(!saved_fixture(bytes, size, path));
        int opened = wl_wav_open(&source, path);
        if (opened) { delete_fixture(path); CHECK(!opened); }
        CHECK(source.pcm.rate == rates[r] && source.pcm.format == formats[f]);
        CHECK(source.data_bytes == (uint64_t)129 * source.block_align);
        CHECK(wl_wav_open(&source, path) != 0 && source.file != NULL);
        memset(output, 0xcc, sizeof(output));
        CHECK(!source.pcm.read(source.pcm.context, output + 1, 128, &count) && count == 128);
        CHECK(output[0] == 0xcc && output[1 + 128 * source.block_align] == 0xcc);
        CHECK(!memcmp(output + 1, bytes + 44, 128 * source.block_align));
        CHECK(!source.pcm.read(source.pcm.context, output + 1, 128, &count) && count == 1);
        CHECK(!memcmp(output + 1, bytes + 44 + 128 * source.block_align, source.block_align));
        CHECK(!source.pcm.read(source.pcm.context, output + 1, 128, &count) && count == 0);
        CHECK(source.pcm.read(source.pcm.context, output, 129, &count) != 0 && count == 0);
        CHECK(source.pcm.read(source.pcm.context, output, 0, &count) != 0 && count == 0);
        CHECK(source.pcm.read(source.pcm.context, output, 128, NULL) != 0);
        wl_wav_close(&source); wl_wav_close(&source);
        CHECK(!delete_fixture(path));
    }
    return 0;
}
static int malformed(void) {
    unsigned char b[2048], good[2048];
    size_t length = fixture(good, 48000, 1, 16, 4);
    for (size_t size = 0; size < length; ++size) CHECK(!opening(good, size, 0));
#define BAD(offset, value) do { memcpy(b, good, sizeof(b)); b[offset] = value; \
    CHECK(!opening(b, length, 0)); } while (0)
    BAD(0, 'R' + 1); BAD(8, 'X'); BAD(20, 2); BAD(22, 1);
    BAD(24, 0); BAD(28, 1); BAD(32, 3); BAD(34, 8); BAD(16, 15);
    BAD(40, 15); BAD(40, 0); BAD(36, 'X');
#undef BAD
    memcpy(b, good, sizeof(b)); put32(b + 4, UINT32_MAX); CHECK(!opening(b, length, 0));
    memcpy(b, good, sizeof(b)); put32(b + 4, 3); CHECK(!opening(b, length, 0));
    memcpy(b, good, sizeof(b)); put32(b + 40, UINT32_MAX); CHECK(!opening(b, length, 0));
    memcpy(b, good, sizeof(b)); put32(b + 24, 192000); put32(b + 28, 768000);
    CHECK(!opening(b, length, 0));
    memcpy(b, good, sizeof(b)); put16(b + 20, 0xfffe); CHECK(!opening(b, length, 0));
    memcpy(b, good, sizeof(b)); put16(b + 20, 3); CHECK(!opening(b, length, 0));
    /* Malformed chunk AFTER data must be found during preflight. */
    memcpy(b, good, sizeof(b)); memcpy(b + length, "JUNK", 4); put32(b + length + 4, 4);
    put32(b + 4, (uint32_t)length); CHECK(!opening(b, length + 8, 0));
    memcpy(b, good, sizeof(b)); put32(b + 4, (uint32_t)length - 7);
    CHECK(!opening(b, length + 1, 0)); /* One stray RIFF byte. */
    /* Duplicate data / fmt chunks are rejected, rather than ambiguously selected. */
    memcpy(b, good, sizeof(b)); memcpy(b + length, good + 36, 24);
    put32(b + 4, (uint32_t)length + 16); CHECK(!opening(b, length + 24, 0));
    memcpy(b, good, sizeof(b)); memcpy(b + length, good + 12, 24);
    put32(b + 4, (uint32_t)length + 16); CHECK(!opening(b, length + 24, 0));
    /* Odd unknown chunk needs its physical pad byte. */
    memcpy(b, good, sizeof(b)); memcpy(b + length, "JUNK", 4); put32(b + length + 4, 1);
    put32(b + 4, (uint32_t)length + 2); CHECK(!opening(b, length + 10, 1));
    CHECK(!opening(b, length + 9, 0));
    /* fmt size18 with cbSize0 is allowed; unknown format extensions are not. */
    memcpy(b, good, 36); memmove(b + 38, good + 36, length - 36);
    put16(b + 36, 0); put32(b + 16, 18); put32(b + 4, (uint32_t)length - 6);
    CHECK(!opening(b, length + 2, 1));
    put16(b + 36, 2); CHECK(!opening(b, length + 2, 0));
    return 0;
}
static int float_values(void) {
    const uint32_t values[] = {0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001,
                               0x00000001, 0x80000000, 0x3f800000, 0x7f7fffff};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        unsigned char b[2048], output[1024];
        char path[256]; unsigned frames = 99;
        wl_wav_source source = {0};
        size_t length = fixture(b, 44100, 3, 32, 1);
        put32(b + 44, values[i]);
        CHECK(!saved_fixture(b, length, path));
        int opened = wl_wav_open(&source, path);
        int status = opened ? -99 : source.pcm.read(source.pcm.context, output, 128, &frames);
        wl_wav_close(&source);
        CHECK(!delete_fixture(path));
        CHECK(!opened);
        CHECK(i < 4 ? status != 0 && frames == 0 : status == 0 && frames == 1);
    }
    return 0;
}
int main(void) {
    wl_wav_source source = {0};
    if (valid_formats() || malformed() || float_values()) return 1;
    CHECK(wl_wav_open(NULL, "x") != 0);
    CHECK(wl_wav_open(&source, NULL) != 0);
    CHECK(wl_wav_open(&source, "") != 0);
    wl_wav_close(NULL);
    printf("wav_source_tests: %u checks passed (fixture files removed; no driver/RF).\n", checks);
    return 0;
}
