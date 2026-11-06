/* SPDX-License-Identifier: Apache-2.0 */
#include "wldac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static double cpu_seconds(void) {
#ifdef _WIN32
    FILETIME creation, exit_time, kernel, user;
    ULARGE_INTEGER k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
        return -1;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) / 10000000.0;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}
static double wall_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}
static int number(const char *text, unsigned *value) {
    char *end;
    unsigned long n;
    if (!text || !*text || *text == '-') return -1;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    n = strtoul(text, &end, 10);
    if (*end || errno == ERANGE || n > UINT_MAX) return -1;
    *value = (unsigned)n;
    return 0;
}
static int quality(const char *text, unsigned rate) {
    unsigned kbps;
    if (!strcmp(text, "hq")) return LDACBT_EQMID_HQ;
    if (!strcmp(text, "sq")) return LDACBT_EQMID_SQ;
    if (!strcmp(text, "mq")) return LDACBT_EQMID_MQ;
    return number(text, &kbps) ? -1 : wl_bitrate_eqmid(rate, kbps);
}
static int bitrates(unsigned rate) {
    unsigned hq = wl_bitrate_kbps(rate, LDACBT_EQMID_HQ);
    if (!hq) return 2;
    printf("{\"sample_rate\":%u,\"supported_kbps\":[%u,%u,%u],\"default_kbps\":%u,"
           "\"selection\":\"manual\",\"battery_downgrade\":false,\"bluetooth_transmitted\":false}\n",
           rate, hq, wl_bitrate_kbps(rate, LDACBT_EQMID_SQ), wl_bitrate_kbps(rate, LDACBT_EQMID_MQ), hq);
    return 0;
}
static int bench(unsigned rate, int eqmid, unsigned seconds) {
    wl_encoder enc;
    wl_encoded packet;
    float source[8192 * 2];
    uint64_t bytes = 0, frames = 0;
    if (!seconds || seconds > 3600 || wl_encoder_open(&enc, rate,
            LDACBT_SMPL_FMT_F32, eqmid, 679)) return 1;
    /* Fixed periodic two-tone fixture. This is not an audio-quality corpus. */
    for (unsigned i = 0; i < 8192; ++i) {
        source[i * 2] = (float)(0.4 * sin(2 * 3.141592653589793 * 173 * i / 8192));
        source[i * 2 + 1] = (float)(0.4 * sin(2 * 3.141592653589793 * 257 * i / 8192));
    }
    const uint64_t blocks = (uint64_t)rate * seconds / 128;
    const double audio = blocks * 128.0 / rate;
    const double cpu_start = cpu_seconds(), wall_start = wall_seconds();
    for (uint64_t i = 0; i < blocks; ++i) {
        if (wl_encoder_push(&enc, source + (i % 64) * 256, &packet)) {
            wl_encoder_close(&enc); return 1;
        }
        bytes += (unsigned)packet.size; frames += (unsigned)packet.frames;
    }
    /* Drain includes codec lookahead/padding, which may extend encoded duration. */
    for (unsigned i = 0; i < 32; ++i) {
        if (wl_encoder_push(&enc, NULL, &packet)) {
            wl_encoder_close(&enc); return 1;
        }
        bytes += (unsigned)packet.size; frames += (unsigned)packet.frames;
        if (!packet.size) break;
        if (i == 31) { wl_encoder_close(&enc); return 1; }
    }
    const double wall = wall_seconds() - wall_start;
    const double cpu = cpu_seconds() - cpu_start;
    printf("{\"kind\":\"offline_synthetic_encoder\",\"rate\":%u,\"eqmid\":%d,"
           "\"audio_seconds\":%.6f,\"wall_seconds\":%.6f,\"process_cpu_seconds\":%.6f,"
           "\"one_core_realtime_percent\":%.6f,\"encoded_bytes\":%llu,"
           "\"encoded_frames\":%llu,\"nominal_kbps\":%d,\"electrical_power_measured\":false}\n",
           rate, eqmid, audio, wall, cpu, 100.0 * cpu / audio,
           (unsigned long long)bytes, (unsigned long long)frames,
           ldacBT_get_bitrate(enc.handle));
    wl_encoder_close(&enc);
    return 0;
}
static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t le32(const unsigned char *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
typedef struct { unsigned rate, align; uint32_t bytes; LDACBT_SMPL_FMT_T format; } wave;
static int read_wave(FILE *file, wave *w) {
    unsigned char header[12], chunk[8], fmt[16];
    int have_fmt = 0;
    uint64_t remaining;
    if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) ||
        memcmp(header + 8, "WAVE", 4) || le32(header + 4) < 4) return -1;
    remaining = le32(header + 4) - 4;
    while (remaining >= 8 && fread(chunk, 1, 8, file) == 8) {
        const uint32_t size = le32(chunk + 4);
        const uint64_t padded = (uint64_t)size + (size & 1);
        remaining -= 8;
        if (padded > remaining || padded > LONG_MAX) return -1;
        if (!memcmp(chunk, "fmt ", 4)) {
            if (have_fmt || size < 16 || fread(fmt, 1, 16, file) != 16) return -1;
            const unsigned tag = le16(fmt), bits = le16(fmt + 14);
            if (le16(fmt + 2) != 2) return -1;
            w->rate = le32(fmt + 4); w->align = le16(fmt + 12);
            if (tag == 1 && bits == 16) w->format = LDACBT_SMPL_FMT_S16;
            else if (tag == 1 && bits == 24) w->format = LDACBT_SMPL_FMT_S24;
            else if (tag == 1 && bits == 32) w->format = LDACBT_SMPL_FMT_S32;
            else if (tag == 3 && bits == 32) w->format = LDACBT_SMPL_FMT_F32;
            else return -1; /* WAVE_FORMAT_EXTENSIBLE is not yet supported. */
            if (w->align != 2 * bits / 8 || (uint64_t)w->rate * w->align != le32(fmt + 8))
                return -1;
            if (fseek(file, (long)(padded - 16), SEEK_CUR)) return -1;
            have_fmt = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!have_fmt || !size || size % w->align) return -1;
            w->bytes = size;
            return 0;
        } else if (fseek(file, (long)padded, SEEK_CUR)) return -1;
        remaining -= padded;
    }
    return -1;
}
static int encode(const char *input, const char *output, const char *choice) {
    wave w = {0};
    wl_encoder enc = {0};
    wl_encoded packet;
    FILE *in = NULL, *out = NULL;
    int result = 1;
    int output_created = 0;
    uint64_t total = 0;
    int eqmid;
    unsigned char pcm[128 * 2 * 4];
    if (!strcmp(input, output)) { fprintf(stderr, "Input and output must differ.\n"); return 1; }
    in = fopen(input, "rb");
    if (!in || read_wave(in, &w)) {
        fprintf(stderr, "Unsupported/malformed WAV. Require stereo PCM16/24/32 or float32 RIFF WAV.\n");
        goto done;
    }
    eqmid = quality(choice, w.rate);
    if (eqmid < 0) {
        fprintf(stderr, "Unsupported bitrate/quality for %u Hz. Use 'bitrates %u' for exact choices.\n", w.rate, w.rate);
        goto done;
    }
    if (wl_encoder_open(&enc, w.rate, w.format, eqmid, 679)) {
        fprintf(stderr, "Encoder initialization failed (supported rates: 44100/48000/88200/96000).\n");
        goto done;
    }
    /* Exclusive creation also protects the source through alternate path spellings. */
    out = fopen(output, "wbx");
    if (!out) { fprintf(stderr, "Cannot create output; it may already exist.\n"); goto done; }
    output_created = 1;
    uint32_t remaining = w.bytes;
    while (remaining) {
        const size_t block = 128 * w.align;
        const size_t take = remaining < block ? remaining : block;
        memset(pcm, 0, sizeof(pcm));
        if (fread(pcm, 1, take, in) != take || wl_encoder_push(&enc, pcm, &packet)) goto done;
        if (fwrite(packet.bytes, 1, (size_t)packet.size, out) != (size_t)packet.size) goto done;
        total += (unsigned)packet.size;
        remaining -= (uint32_t)take;
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (wl_encoder_push(&enc, NULL, &packet)) goto done;
        if (fwrite(packet.bytes, 1, (size_t)packet.size, out) != (size_t)packet.size) goto done;
        total += (unsigned)packet.size;
        if (!packet.size) break;
        if (i == 31) goto done;
    }
    if (fflush(out)) goto done;
    printf("{\"kind\":\"offline_ldac_stream\",\"sample_rate\":%u,\"encoded_bytes\":%llu,"
           "\"requested_kbps\":%u,\"encoder_kbps\":%d,\"eqmid\":%d,\"bluetooth_transmitted\":false}\n",
           w.rate, (unsigned long long)total, wl_bitrate_kbps(w.rate, eqmid), wl_encoder_bitrate(&enc), eqmid);
    result = 0;
done:
    if (in) fclose(in);
    if (out && fclose(out)) result = 1;
    wl_encoder_close(&enc);
    if (result && output_created) remove(output);
    return result;
}
int main(int argc, char **argv) {
    unsigned rate, seconds, kbps;
    int eqmid;
    if (argc == 5 && !strcmp(argv[1], "bench") && !number(argv[2], &rate) &&
        (eqmid = quality(argv[3], rate)) >= 0 && !number(argv[4], &seconds))
        return bench(rate, eqmid, seconds);
    if (argc == 6 && !strcmp(argv[1], "bench") && !number(argv[2], &rate) &&
        !strcmp(argv[3], "--bitrate") && !number(argv[4], &kbps) &&
        (eqmid = wl_bitrate_eqmid(rate, kbps)) >= 0 && !number(argv[5], &seconds))
        return bench(rate, eqmid, seconds);
    if (argc == 3 && !strcmp(argv[1], "bitrates") && !number(argv[2], &rate))
        return bitrates(rate);
    if (argc == 4 && !strcmp(argv[1], "encode"))
        return encode(argv[2], argv[3], "hq");
    if (argc == 5 && !strcmp(argv[1], "encode"))
        return encode(argv[2], argv[3], argv[4]);
    if (argc == 6 && !strcmp(argv[1], "encode") && !strcmp(argv[4], "--bitrate") &&
        !number(argv[5], &kbps))
        return encode(argv[2], argv[3], argv[5]);
    fprintf(stderr, "Offline development tool; no Bluetooth transmission.\n"
        "Usage: wldac_cli bench <44100|48000|88200|96000> <hq|sq|mq> <seconds 1..3600>\n"
        "       wldac_cli bench <rate> --bitrate <kbps> <seconds>\n"
        "       wldac_cli encode input.wav new-output.ldac [hq|sq|mq|kbps] (default HQ)\n"
        "       wldac_cli encode input.wav new-output.ldac --bitrate <kbps>\n"
        "       wldac_cli bitrates <rate>\n");
    return 2;
}
