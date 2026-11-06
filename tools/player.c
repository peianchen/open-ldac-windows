/* SPDX-License-Identifier: Apache-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include "wav_source.h"
#include "win_native_io.h"
#include "reconnect.h"

/* Explicit foreground/service entry points only. This executable never creates
 * a service, changes its start type, installs a driver, or starts at logon. */
static const wchar_t service_name[] = L"OpenLdacAudioLab";
typedef struct {
    const char *play, *inspect, *result;
    unsigned device, bitrate, duration_ms, reconnect_restarts;
    int service, selftest, connect, help;
} arguments;
typedef struct {
    arguments args;
    HANDLE stop, ready, result_file;
    SERVICE_STATUS_HANDLE status_handle;
    SERVICE_STATUS status;
    wl_wav_source source;
    wl_playback_report report;
    wl_reconnect_report reconnect;
    wl_win_native_timing timing;
    int timing_available;
    int mmcss_attempted, mmcss_registered, mmcss_revert_attempted, mmcss_reverted;
    DWORD mmcss_registration_error, mmcss_revert_error;
    volatile LONG requested, control_rate, accepting_controls;
    volatile LONG stop_controls, shutdown_controls, hq_controls, sq_controls, mq_controls;
    volatile LONG rejected_controls, status_error;
    int service_main_invoked, handler_registered, running_reported, stop_pending_reported;
    int timed_out, worker_result, preflight_passed, worker_started;
    DWORD worker_error, exit_code;
} application;
static application app;
static HANDLE console_stop;
static SRWLOCK console_lock = SRWLOCK_INIT;

static int number(const char *text, unsigned *value) {
    char *end; unsigned long n;
    if (!text || !*text) return -1;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return -1;
    errno = 0; n = strtoul(text, &end, 10);
    if (*end || errno == ERANGE || n > UINT_MAX) return -1;
    *value = (unsigned)n;
    return 0;
}
static void usage(void) {
    fprintf(stderr,
        "Experimental native LDAC player; RF requires explicit --play AND --connect.\n"
        "  wldac_player --inspect source.wav\n"
        "  wldac_player --play source.wav --connect [--device-index 0..15] [--bitrate kbps]\n"
        "  wldac_player --service --play source.wav --connect [same options]\n"
        "  Optional: --result new.json (CREATE_NEW final report; existing files are never overwritten)\n"
        "  Optional: --reconnect-restart 0..3 (default 0; whole WAV restarts after link loss)\n"
        "  wldac_player --service-selftest --result new.json [--duration-ms 1000..60000]\n"
        "Service modes require SCM name OpenLdacAudioLab; no service is installed automatically.\n"
        "Selftest uses SCM and synthetic 48000-Hz control mapping only; no codec/driver/RF.\n"
        "Stereo RIFF PCM16/24/32 or float32, 44100/48000/88200/96000 Hz; no resampling.\n"
        "Default HQ; exact source-family manual bitrates only; no battery downgrade.\n"
        "SCM controls 128=HQ, 129=SQ, 130=MQ; foreground Ctrl+C stops and drains I/O.\n"
        "Reconnect uses 1/2/4-second stoppable backoff and the same selected interface.\n"
        "Reconnect replays from the beginning; it is not gapless and never lowers bitrate.\n"
        "Windows command line and file paths preserve Unicode through UTF-16.\n");
}
static int parse(int argc, char **argv, arguments *a) {
    unsigned seen = 0;
    memset(a, 0, sizeof(*a)); a->duration_ms = 60000;
    if (argc == 1) { a->help = 1; return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { a->help = 1; return 0; }
    for (int i = 1; i < argc; ++i) {
        unsigned flag;
        const char *arg = argv[i];
        if (!strcmp(arg, "--play")) flag = 1;
        else if (!strcmp(arg, "--inspect")) flag = 2;
        else if (!strcmp(arg, "--connect")) flag = 4;
        else if (!strcmp(arg, "--device-index")) flag = 8;
        else if (!strcmp(arg, "--bitrate")) flag = 16;
        else if (!strcmp(arg, "--service")) flag = 32;
        else if (!strcmp(arg, "--service-selftest")) flag = 64;
        else if (!strcmp(arg, "--result")) flag = 128;
        else if (!strcmp(arg, "--duration-ms")) flag = 256;
        else if (!strcmp(arg, "--reconnect-restart")) flag = 512;
        else return -1;
        if (seen & flag) return -1;
        seen |= flag;
        if (flag == 4) { a->connect = 1; continue; }
        if (flag == 32) { a->service = 1; continue; }
        if (flag == 64) { a->selftest = a->service = 1; continue; }
        if (++i >= argc || !argv[i][0]) return -1;
        switch (flag) {
        case 1: a->play = argv[i]; break;
        case 2: a->inspect = argv[i]; break;
        case 8: if (number(argv[i], &a->device) || a->device > 15) return -1; break;
        case 16: if (number(argv[i], &a->bitrate)) return -1; break;
        case 128: a->result = argv[i]; break;
        case 512: if (number(argv[i], &a->reconnect_restarts) || a->reconnect_restarts > 3) return -1; break;
        case 256:
            if (number(argv[i], &a->duration_ms) || a->duration_ms < 1000 || a->duration_ms > 60000)
                return -1;
            break;
        default: return -1;
        }
    }
    if (a->inspect) return seen == 2 ? 0 : -1;
    if (a->selftest) return a->result && !(seen & (1 | 2 | 4 | 8 | 16 | 32 | 512)) ? 0 : -1;
    if (!a->play || !a->connect || (seen & 256)) return -1;
    return 0;
}
static wchar_t *wide_path(const char *utf8) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    wchar_t *wide;
    if (!count || !(wide = malloc((size_t)count * sizeof(*wide)))) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, count)) {
        free(wide); return NULL;
    }
    return wide;
}
static LONG atomic_read(volatile LONG *value) { return InterlockedCompareExchange(value, 0, 0); }
static BOOL WINAPI console_handler(DWORD control) {
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT && control != CTRL_CLOSE_EVENT &&
        control != CTRL_LOGOFF_EVENT && control != CTRL_SHUTDOWN_EVENT) return FALSE;
    /* A callback already dispatched before unregister may still run. Never
     * wait in this callback: failure to acquire means owner cleanup has begun. */
    if (TryAcquireSRWLockShared(&console_lock)) {
        if (console_stop) SetEvent(console_stop);
        ReleaseSRWLockShared(&console_lock);
    }
    return TRUE;
}
static int preflight(void) {
    if (app.args.selftest) {
        /* Valid only as a synthetic manual-control fixture, never negotiation. */
        InterlockedExchange(&app.control_rate, 48000);
        return 0;
    }
    if (wl_wav_open(&app.source, app.args.play)) {
        fprintf(stderr, "WAV preflight failed; no Bluetooth channel opened.\n");
        app.worker_result = WL_PLAY_SOURCE;
        app.worker_error = ERROR_BAD_FORMAT;
        return -1;
    }
    if (app.args.bitrate && wl_bitrate_eqmid(app.source.pcm.rate, app.args.bitrate) < 0) {
        fprintf(stderr, "Bitrate is not an exact choice for this source sample-rate family.\n");
        app.worker_result = WL_PLAY_BITRATE;
        app.worker_error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    InterlockedExchange(&app.control_rate, (LONG)app.source.pcm.rate);
    InterlockedExchange(&app.requested, (LONG)app.args.bitrate);
    app.preflight_passed = 1;
    return 0;
}
typedef struct { wl_win_native_io *native; } reconnect_context;
static int restart_source(void *context) {
    (void)context;
    if (!wl_wav_rewind(&app.source)) return 0;
    app.worker_error = ERROR_READ_FAULT; return -1;
}
static int retry_wait(void *context, unsigned milliseconds) {
    DWORD wait;
    (void)context;
    wait = WaitForSingleObject(app.stop, milliseconds);
    if (wait == WAIT_TIMEOUT) return 0;
    app.worker_error = wait == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED :
        (wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE);
    return -1;
}
static int retry_link_loss(void *context, int result, const wl_playback_report *report) {
    reconnect_context *retry = context;
    (void)report;
    if (result != WL_PLAY_TRANSPORT) return 0;
    return wl_win_native_retryable_error(wl_win_native_last_error(retry->native));
}
/* MMCSS association and reversion belong to the playback owner thread.
 * Audio uses the OS task's existing settings; no explicit priority override,
 * system timer resolution, registry or power-plan change is made. */
static HANDLE worker_mmcss_enter(void) {
    DWORD task_index = 0;
    HANDLE task;
    app.mmcss_attempted = 1;
    task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    if (task) app.mmcss_registered = 1;
    else {
        app.mmcss_registration_error = GetLastError();
        if (!app.mmcss_registration_error) app.mmcss_registration_error = ERROR_GEN_FAILURE;
    }
    return task;
}
static void worker_mmcss_leave(HANDLE task) {
    if (!task) return;
    app.mmcss_revert_attempted = 1;
    if (AvRevertMmThreadCharacteristics(task)) app.mmcss_reverted = 1;
    else {
        app.mmcss_revert_error = GetLastError();
        if (!app.mmcss_revert_error) app.mmcss_revert_error = ERROR_GEN_FAILURE;
    }
}
static DWORD WINAPI worker(void *unused) {
    (void)unused;
    app.worker_started = 1;
    if (app.ready) {
        HANDLE startup[] = {app.stop, app.ready};
        DWORD wait = WaitForMultipleObjects(2, startup, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) { app.worker_result = WL_PLAY_STOPPED; return 0; }
        if (wait != WAIT_OBJECT_0 + 1) {
            app.worker_error = GetLastError(); app.worker_result = WL_PLAY_TRANSPORT; return 0;
        }
    }
    if (app.args.selftest) {
        DWORD wait = WaitForSingleObject(app.stop, app.args.duration_ms);
        if (wait == WAIT_TIMEOUT) app.timed_out = 1;
        else if (wait != WAIT_OBJECT_0) {
            app.worker_error = GetLastError(); app.worker_result = WL_PLAY_TRANSPORT;
        }
        return 0;
    }
    wl_playback_io io = {0};
    wl_playback_options options = {0};
    FILETIME now; ULARGE_INTEGER stamp;
    wl_win_native_io *native = wl_win_native_create(app.args.device, NULL, app.stop, &app.requested);
    if (!native) {
        app.worker_error = GetLastError();
        if (!app.worker_error) app.worker_error = ERROR_NOT_ENOUGH_MEMORY;
        app.worker_result = WL_PLAY_TRANSPORT;
        return 0;
    }
    /* Failure is observable but retains the existing scheduling fallback.
     * Every exit after this point goes through the matching same-thread revert. */
    HANDLE mmcss_task = worker_mmcss_enter();
    GetSystemTimeAsFileTime(&now);
    stamp.LowPart = now.dwLowDateTime; stamp.HighPart = now.dwHighDateTime;
    options.generation = stamp.QuadPart ? stamp.QuadPart : 1;
    options.ssrc = (uint32_t)(stamp.LowPart ^ GetCurrentProcessId());
    options.local_seid = 1; options.bitrate_kbps = app.args.bitrate;
    wl_win_native_bind(native, &io);
    reconnect_context context = {native};
    wl_reconnect_policy policy = {&context, app.args.reconnect_restarts,
        restart_source, retry_wait, retry_link_loss};
    app.worker_result = wl_playback_reconnect(&io, &app.source.pcm, &options,
        &policy, &app.report, &app.reconnect);
    if (!app.worker_error) app.worker_error = wl_win_native_last_error(native);
    app.timing_available = wl_win_native_get_timing(native, &app.timing) == 0;
    wl_win_native_destroy(native);
    worker_mmcss_leave(mmcss_task);
    return 0;
}
static int report_status(DWORD state, DWORD error, DWORD specific) {
    memset(&app.status, 0, sizeof(app.status));
    app.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    app.status.dwCurrentState = state;
    app.status.dwWin32ExitCode = error;
    app.status.dwServiceSpecificExitCode = specific;
    if (state == SERVICE_RUNNING)
        app.status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        app.status.dwCheckPoint = 1; app.status.dwWaitHint = 30000;
    }
    if (!SetServiceStatus(app.status_handle, &app.status)) {
        DWORD failure = GetLastError();
        if (!failure) failure = ERROR_GEN_FAILURE;
        InterlockedCompareExchange(&app.status_error, (LONG)failure, 0);
        SetEvent(app.stop);
        return -1;
    }
    if (state == SERVICE_RUNNING) app.running_reported = 1;
    if (state == SERVICE_STOP_PENDING) app.stop_pending_reported = 1;
    return 0;
}
static DWORD WINAPI control_handler(DWORD control, DWORD type, void *data, void *context) {
    (void)type; (void)data; (void)context;
    if (control == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        if (control == SERVICE_CONTROL_STOP) InterlockedIncrement(&app.stop_controls);
        else InterlockedIncrement(&app.shutdown_controls);
        /* Cancellation is only a signal. ServiceMain reports STOP_PENDING and
         * joins the owning worker; the handler never waits or frees I/O. */
        return SetEvent(app.stop) ? NO_ERROR : GetLastError();
    }
    if (control >= 128 && control <= 130) {
        unsigned rate = (unsigned)atomic_read(&app.control_rate), bitrate;
        if (!atomic_read(&app.accepting_controls) || !rate ||
            !(bitrate = wl_bitrate_kbps(rate, (int)(control - 128)))) {
            InterlockedIncrement(&app.rejected_controls);
            return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
        }
        InterlockedExchange(&app.requested, (LONG)bitrate);
        if (control == 128) InterlockedIncrement(&app.hq_controls);
        if (control == 129) InterlockedIncrement(&app.sq_controls);
        if (control == 130) InterlockedIncrement(&app.mq_controls);
        return NO_ERROR;
    }
    InterlockedIncrement(&app.rejected_controls);
    return ERROR_CALL_NOT_IMPLEMENTED;
}
/* Called after the worker has joined, or before it starts. SCM acceptance is a request, not
 * proof of an applied codec change; report counters cover only the last attempt. */
static const char *timeout_source(unsigned source) {
    switch (source) {
    case WL_WIN_TIMEOUT_PACING: return "pacing_lateness";
    case WL_WIN_TIMEOUT_IO_WAIT: return "io_wait";
    case WL_WIN_TIMEOUT_IO_COMPLETION: return "io_completion";
    default: return "none";
    }
}
static int native_report_json(char *json, size_t capacity) {
    return snprintf(json, capacity,
        "{\"kind\":\"native_playback_report\",\"result\":%d,\"win32_error\":%lu,"
        "\"protocol_error\":%d,\"encoder_error\":%d,\"stage\":%u,"
        "\"actual_kbps\":%u,\"source_frames\":%llu,\"media_packets\":%llu,"
        "\"media_bytes\":%llu,\"bitrate_changes\":%u,\"requested_kbps\":%u,"
        "\"attempts\":%u,\"restarts\":%u,\"retry_exhausted\":%s,"
        "\"generation_exhausted\":%s,\"reconnect_max_retries\":%u,\"restart_policy\":\"whole_source\","
        "\"counters_scope\":\"last_attempt\",\"battery_downgrade\":false,"
        "\"preflight_passed\":%s,\"worker_started\":%s,\"service_mode\":%s,"
        "\"service_main_invoked\":%s,\"handler_registered\":%s,"
        "\"running_reported\":%s,\"stop_pending_reported\":%s,"
        "\"pending_control_requested_kbps\":%ld,\"hq_controls\":%ld,"
        "\"sq_controls\":%ld,\"mq_controls\":%ld,\"rejected_controls\":%ld,"
        "\"stop_controls\":%ld,\"shutdown_controls\":%ld,\"service_status_error\":%ld,"
        "\"exit_code_before_report\":%lu,\"peer_commands\":%u,\"peer_rejects\":%u,"
        "\"peer_suspends\":%u,\"peer_resumes\":%u,"
        "\"last_attempt_media_write_completed\":%s,"
        "\"timing\":{\"available\":%s,\"scope\":\"whole_adapter_all_attempts\","
        "\"pacing_limit_us\":%u,\"peak_pacing_lateness_us\":%llu,"
        "\"pacing_deadline_misses\":%llu,\"last_pacing_miss_offset\":%llu,"
        "\"io_wait_timeouts\":%llu,\"io_completion_timeout_errors\":%llu,"
        "\"media_write_calls\":%llu,\"media_write_timing_samples\":%llu,"
        "\"max_media_write_us\":%llu,\"last_timeout_source\":\"%s\","
        "\"high_resolution_timer\":%s,\"timer_fallback_used\":%s,\"high_resolution_timer_error\":%lu,"
        "\"pacing_timer_wake_samples\":%llu,\"peak_timer_wake_lateness_us\":%llu,"
        "\"peak_before_wait_lateness_us\":%llu,\"pacing_misses_after_timer_wake\":%llu,"
        "\"pacing_misses_before_timer_wait\":%llu},"
        "\"mmcss\":{\"task\":\"Audio\",\"scope\":\"playback_owner_thread\","
        "\"priority_override\":false,\"registration_attempted\":%s,\"registered\":%s,"
        "\"registration_error\":%lu,\"revert_attempted\":%s,\"reverted\":%s,"
        "\"revert_error\":%lu}}\n",
        app.worker_result, (unsigned long)app.worker_error, app.report.protocol_error,
        app.report.encoder_error, app.report.stage, app.report.actual_kbps,
        (unsigned long long)app.report.source_frames, (unsigned long long)app.report.media_packets,
        (unsigned long long)app.report.media_bytes, app.report.bitrate_changes,
        app.report.requested_kbps, app.reconnect.attempts, app.reconnect.restarts,
        app.reconnect.exhausted ? "true" : "false",
        app.reconnect.generation_exhausted ? "true" : "false", app.args.reconnect_restarts,
        app.preflight_passed ? "true" : "false", app.worker_started ? "true" : "false",
        app.args.service ? "true" : "false", app.service_main_invoked ? "true" : "false",
        app.handler_registered ? "true" : "false", app.running_reported ? "true" : "false",
        app.stop_pending_reported ? "true" : "false", atomic_read(&app.requested),
        atomic_read(&app.hq_controls), atomic_read(&app.sq_controls), atomic_read(&app.mq_controls),
        atomic_read(&app.rejected_controls), atomic_read(&app.stop_controls), atomic_read(&app.shutdown_controls),
        atomic_read(&app.status_error), (unsigned long)app.exit_code, app.report.peer_commands,
        app.report.peer_rejects, app.report.peer_suspends, app.report.peer_resumes,
        app.report.media_packets ? "true" : "false", app.timing_available ? "true" : "false",
        WL_WIN_NATIVE_MAX_LATENESS_MS * 1000u,
        (unsigned long long)app.timing.peak_pacing_lateness_us,
        (unsigned long long)app.timing.pacing_deadline_misses,
        (unsigned long long)app.timing.last_pacing_miss_offset,
        (unsigned long long)app.timing.io_wait_timeouts,
        (unsigned long long)app.timing.io_completion_timeout_errors,
        (unsigned long long)app.timing.media_write_calls,
        (unsigned long long)app.timing.media_write_timing_samples,
        (unsigned long long)app.timing.max_media_write_us, timeout_source(app.timing.last_timeout_source),
        app.timing.high_resolution_timer ? "true" : "false", app.timing.timer_fallback_used ? "true" : "false",
        (unsigned long)app.timing.high_resolution_timer_error,
        (unsigned long long)app.timing.pacing_timer_wake_samples,
        (unsigned long long)app.timing.peak_timer_wake_lateness_us,
        (unsigned long long)app.timing.peak_before_wait_lateness_us,
        (unsigned long long)app.timing.pacing_misses_after_timer_wake,
        (unsigned long long)app.timing.pacing_misses_before_timer_wait,
        app.mmcss_attempted ? "true" : "false", app.mmcss_registered ? "true" : "false",
        (unsigned long)app.mmcss_registration_error, app.mmcss_revert_attempted ? "true" : "false",
        app.mmcss_reverted ? "true" : "false", (unsigned long)app.mmcss_revert_error);
}
static int result_json(void) {
    char json[4096]; DWORD written;
    int length;
    if (app.result_file == INVALID_HANDLE_VALUE) return 0;
    if (!app.args.selftest) length = native_report_json(json, sizeof(json));
    else length = snprintf(json, sizeof(json),
        "{\"kind\":\"scm_no_rf_selftest\",\"service_main_invoked\":%s,"
        "\"handler_registered\":%s,\"running_reported\":%s,\"stop_pending_reported\":%s,"
        "\"test_source_rate\":48000,\"synthetic_control_mapping\":true,"
        "\"hq_requested_kbps\":990,\"sq_requested_kbps\":660,\"mq_requested_kbps\":330,"
        "\"stop_controls\":%ld,\"shutdown_controls\":%ld,\"hq_controls\":%ld,"
        "\"sq_controls\":%ld,\"mq_controls\":%ld,\"rejected_controls\":%ld,"
        "\"requested_kbps\":%ld,\"worker_result\":%d,\"worker_win32_error\":%lu,"
        "\"service_status_error\":%ld,\"stop_signalled\":%s,\"timed_out\":%s,"
        "\"bluetooth_transmitted\":false,\"codec_exercised\":false}\n",
        app.service_main_invoked ? "true" : "false", app.handler_registered ? "true" : "false",
        app.running_reported ? "true" : "false", app.stop_pending_reported ? "true" : "false",
        atomic_read(&app.stop_controls), atomic_read(&app.shutdown_controls),
        atomic_read(&app.hq_controls), atomic_read(&app.sq_controls), atomic_read(&app.mq_controls),
        atomic_read(&app.rejected_controls), atomic_read(&app.requested), app.worker_result,
        (unsigned long)app.worker_error, atomic_read(&app.status_error),
        WaitForSingleObject(app.stop, 0) == WAIT_OBJECT_0 ? "true" : "false",
        app.timed_out ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(json) ||
        !WriteFile(app.result_file, json, (DWORD)length, &written, NULL) || written != (DWORD)length ||
        !FlushFileBuffers(app.result_file)) return -1;
    return 0;
}
static void WINAPI service_main(DWORD argc, wchar_t **argv) {
    HANDLE thread = NULL;
    DWORD error = NO_ERROR, specific = 0;
    (void)argc; (void)argv;
    app.service_main_invoked = 1;
    app.status_handle = RegisterServiceCtrlHandlerExW(service_name, control_handler, NULL);
    if (!app.status_handle) { app.exit_code = GetLastError(); goto finish; }
    app.handler_registered = 1;
    if (report_status(SERVICE_START_PENDING, NO_ERROR, 0)) goto stopping;
    if (preflight()) { error = app.worker_error; goto stopping; }
    app.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app.ready || !(thread = CreateThread(NULL, 0, worker, NULL, 0, NULL))) {
        error = GetLastError(); goto stopping;
    }
    InterlockedExchange(&app.accepting_controls, 1);
    if (report_status(SERVICE_RUNNING, NO_ERROR, 0)) goto stopping;
    if (!SetEvent(app.ready)) { error = GetLastError(); goto stopping; }
    {
        HANDLE events[] = {app.stop, thread};
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1) error = GetLastError();
    }
stopping:
    InterlockedExchange(&app.accepting_controls, 0);
    /* Publish once before draining; do not invent periodic progress if stuck. */
    report_status(SERVICE_STOP_PENDING, NO_ERROR, 0);
    SetEvent(app.stop);
    if (thread) {
        if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0 && !error) error = GetLastError();
        CloseHandle(thread);
    }
    if (!error && app.worker_result != WL_PLAY_OK && app.worker_result != WL_PLAY_STOPPED &&
        app.worker_result != WL_PLAY_PEER_CLOSED) {
        error = app.worker_error;
        if (!error) { error = ERROR_SERVICE_SPECIFIC_ERROR; specific = (DWORD)-app.worker_result; }
    }
finish:
    InterlockedExchange(&app.accepting_controls, 0);
    wl_wav_close(&app.source);
    if (app.ready) { CloseHandle(app.ready); app.ready = NULL; }
    if (!error) error = (DWORD)atomic_read(&app.status_error);
    if (!error) error = app.exit_code;
    app.exit_code = error;
    if (result_json() && (!error || !app.args.selftest)) error = ERROR_WRITE_FAULT;
    if (app.result_file != INVALID_HANDLE_VALUE) {
        if (!CloseHandle(app.result_file) && !error) error = GetLastError();
        app.result_file = INVALID_HANDLE_VALUE;
    }
    app.exit_code = error;
    if (app.status_handle && report_status(SERVICE_STOPPED, error, specific))
        app.exit_code = (DWORD)atomic_read(&app.status_error);
}
static int inspect(const char *path) {
    wl_wav_source source = {0};
    if (wl_wav_open(&source, path)) {
        fprintf(stderr, "Unsupported/malformed/truncated WAV. No Bluetooth operation.\n"); return 2;
    }
    const char *format = source.pcm.format == LDACBT_SMPL_FMT_S16 ? "pcm16" :
        source.pcm.format == LDACBT_SMPL_FMT_S24 ? "pcm24" :
        source.pcm.format == LDACBT_SMPL_FMT_S32 ? "pcm32" : "float32";
    printf("{\"kind\":\"wav_structure_inspection\",\"format\":\"%s\",\"sample_rate\":%u,"
        "\"channels\":2,\"source_frames\":%llu,\"data_bytes\":%llu,"
        "\"supported_kbps\":[%u,%u,%u],\"default_kbps\":%u,\"sample_values_scanned\":false,"
        "\"resampling\":false,\"battery_downgrade\":false,\"bluetooth_transmitted\":false}\n",
        format, source.pcm.rate, (unsigned long long)(source.data_bytes / source.block_align),
        (unsigned long long)source.data_bytes, wl_bitrate_kbps(source.pcm.rate, LDACBT_EQMID_HQ),
        wl_bitrate_kbps(source.pcm.rate, LDACBT_EQMID_SQ), wl_bitrate_kbps(source.pcm.rate, LDACBT_EQMID_MQ),
        wl_bitrate_kbps(source.pcm.rate, LDACBT_EQMID_HQ));
    wl_wav_close(&source);
    return 0;
}
static int run(int argc, char **argv) {
    memset(&app, 0, sizeof(app)); app.result_file = INVALID_HANDLE_VALUE;
    if (parse(argc, argv, &app.args)) { usage(); return ERROR_INVALID_PARAMETER; }
    app.worker_result = app.args.selftest ? WL_PLAY_OK : WL_PLAY_ARGUMENT;
    if (app.args.help) { usage(); return 0; }
    if (app.args.inspect) return inspect(app.args.inspect);
    app.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app.stop) return (int)GetLastError();
    if (app.args.result) {
        wchar_t *path = wide_path(app.args.result);
        if (!path) { CloseHandle(app.stop); return ERROR_INVALID_NAME; }
        app.result_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD error = app.result_file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        free(path);
        if (error) { CloseHandle(app.stop); return (int)error; }
    }
    if (app.args.service) {
        SERVICE_TABLE_ENTRYW table[] = {{(wchar_t *)service_name, service_main}, {NULL, NULL}};
        if (!StartServiceCtrlDispatcherW(table)) {
            app.exit_code = GetLastError();
            app.worker_error = app.exit_code;
            app.worker_result = WL_PLAY_TRANSPORT;
            if (result_json()) app.exit_code = ERROR_WRITE_FAULT;
        }
    } else {
        if (preflight()) app.exit_code = app.worker_error;
        else {
            console_stop = app.stop;
            if (!SetConsoleCtrlHandler(console_handler, TRUE)) app.exit_code = GetLastError();
            else {
                worker(NULL);
                if (!SetConsoleCtrlHandler(console_handler, FALSE)) app.exit_code = GetLastError();
                if (app.worker_result != WL_PLAY_OK && app.worker_result != WL_PLAY_STOPPED &&
                    app.worker_result != WL_PLAY_PEER_CLOSED)
                    app.exit_code = app.worker_error ? app.worker_error : ERROR_SERVICE_SPECIFIC_ERROR;
                char json[4096];
                int length = native_report_json(json, sizeof(json));
                if (length < 0 || (size_t)length >= sizeof(json)) app.exit_code = ERROR_WRITE_FAULT;
                else printf("%s", json);
            }
            AcquireSRWLockExclusive(&console_lock);
            console_stop = NULL;
            ReleaseSRWLockExclusive(&console_lock);
        }
    }
    wl_wav_close(&app.source);
    if (!app.args.service && result_json()) app.exit_code = ERROR_WRITE_FAULT;
    if (app.result_file != INVALID_HANDLE_VALUE && !CloseHandle(app.result_file)) app.exit_code = ERROR_WRITE_FAULT;
    CloseHandle(app.stop);
    return (int)app.exit_code;
}
int main(void) {
    int argc, result = ERROR_NOT_ENOUGH_MEMORY;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **args;
    if (!wide) return (int)GetLastError();
    args = calloc((size_t)argc, sizeof(*args));
    if (!args) { LocalFree(wide); return ERROR_NOT_ENOUGH_MEMORY; }
    for (int i = 0; i < argc; ++i) {
        int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!count || !(args[i] = malloc((size_t)count)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, args[i], count, NULL, NULL))
            goto done;
    }
    result = run(argc, args);
done:
    for (int i = 0; i < argc; ++i) free(args[i]);
    free(args); LocalFree(wide);
    return result;
}
