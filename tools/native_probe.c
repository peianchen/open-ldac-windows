/* SPDX-License-Identifier: Apache-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "native_transport.h"
#include "avdtp.h"

static const GUID interface_id = OL_NATIVE_INTERFACE_GUID;
typedef enum { OP_IOCTL, OP_READ, OP_WRITE } operation;

/* Cancel and drain before freeing OVERLAPPED/buffers. A cancellation request
 * is not completion. No polling and no global timer resolution changes. */
static DWORD perform(HANDLE handle, operation op, DWORD code, void *input,
                     DWORD input_size, void *output, DWORD output_size,
                     DWORD *transferred, DWORD timeout_ms)
{
    OVERLAPPED pending = {0};
    DWORD error = ERROR_SUCCESS;
    BOOL ok;
    pending.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!pending.hEvent) return GetLastError();
    *transferred = 0;
    if (op == OP_IOCTL)
        ok = DeviceIoControl(handle, code, input, input_size, output, output_size,
                             transferred, &pending);
    else if (op == OP_READ)
        ok = ReadFile(handle, output, output_size, transferred, &pending);
    else ok = WriteFile(handle, input, input_size, transferred, &pending);
    if (!ok) {
        error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(pending.hEvent, timeout_ms);
            if (wait != WAIT_OBJECT_0) {
                error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                CancelIoEx(handle, &pending);
                /* The lower stack must retire the I/O before these stack buffers go away. */
                GetOverlappedResult(handle, &pending, transferred, TRUE);
            } else if (!GetOverlappedResult(handle, &pending, transferred, FALSE)) {
                error = GetLastError();
            } else error = ERROR_SUCCESS;
        }
    }
    CloseHandle(pending.hEvent);
    return error;
}

static int enumerate(unsigned selected, wchar_t **chosen, unsigned *count)
{
    HDEVINFO devices = SetupDiGetClassDevsW(&interface_id, NULL, NULL,
                                          DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return 1;
    *count = 0;
    *chosen = NULL;
    for (DWORD index = 0; ; ++index) {
        SP_DEVICE_INTERFACE_DATA item = {0};
        DWORD needed = 0;
        item.cbSize = sizeof(item);
        if (!SetupDiEnumDeviceInterfaces(devices, NULL, &interface_id, index, &item)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                SetupDiDestroyDeviceInfoList(devices); free(*chosen); *chosen = NULL; return 1;
            }
            break;
        }
        (*count)++;
        if (index != selected) continue;
        SetupDiGetDeviceInterfaceDetailW(devices, &item, NULL, 0, &needed, NULL);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed > 65536) {
            SetupDiDestroyDeviceInfoList(devices); return 1;
        }
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = malloc(needed);
        if (!detail) { SetupDiDestroyDeviceInfoList(devices); return 1; }
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &item, detail, needed, NULL, NULL)) {
            free(detail); SetupDiDestroyDeviceInfoList(devices); return 1;
        }
        size_t length = wcslen(detail->DevicePath) + 1;
        *chosen = malloc(length * sizeof(wchar_t));
        if (!*chosen) { free(detail); SetupDiDestroyDeviceInfoList(devices); return 1; }
        memcpy(*chosen, detail->DevicePath, length * sizeof(wchar_t));
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return 0;
}

int main(int argc, char **argv)
{
    wchar_t *path = NULL;
    unsigned count, selected = 0;
    DWORD bytes, error;
    ol_native_request request = {sizeof(request), OL_NATIVE_ABI};
    ol_native_status state;
    int discover = argc >= 2 && !strcmp(argv[1], "--discover");
    int list = argc == 1 || (argc == 2 && !strcmp(argv[1], "--list"));
    int status = argc >= 2 && !strcmp(argv[1], "--status");
    int diagnostics = argc >= 2 && !strcmp(argv[1], "--diagnostics");
    if ((!list && !status && !discover && !diagnostics) || argc > 3) {
        fprintf(stderr, "Usage: wldac_probe [--list | --status [index] | --diagnostics [index] | --discover [index]]\n"
                        "List/status do not open a Bluetooth channel. Discover explicitly connects and sends AVDTP.\n");
        return 2;
    }
    if (argc == 3) {
        char *end;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || *argv[2] == '-' || value > 15) return 2;
        selected = (unsigned)value;
    }
    if (enumerate(selected, &path, &count)) {
        fprintf(stderr, "Interface enumeration failed.\n"); return 1;
    }
    if (list) {
        printf("{\"driver_interfaces\":%u,\"bluetooth_channel_opened\":false}\n", count);
        free(path);
        return 0;
    }
    if (!path) {
        fprintf(stderr, "No installed Open LDAC transport interface at this index.\n");
        return 1;
    }
    HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    free(path);
    if (handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Cannot open transport (Windows error %lu). Admin access is required.\n", GetLastError());
        return 1;
    }
    if (diagnostics) {
        ol_native_diagnostics snapshot;
        static const char *names[OL_EVENT_COUNT] = {
            "open_send_attempts", "open_completions", "open_failures",
            "callback_add", "callback_release", "contract_faults", "remote_disconnects",
            "close_send_attempts", "close_completions", "close_failures", "cancel_calls",
            "transfer_retirements", "roots_destroyed", "removals"
        };
        error = perform(handle, OP_IOCTL, OL_IOCTL_DIAGNOSTICS, &request, sizeof(request),
                        &snapshot, sizeof(snapshot), &bytes, 5000);
        if (error || bytes != sizeof(snapshot) || snapshot.size != sizeof(snapshot) ||
            snapshot.version != OL_DIAGNOSTICS_ABI || snapshot.count != OL_EVENT_COUNT) {
            fprintf(stderr, "Diagnostics query failed or ABI mismatch.\n");
            CloseHandle(handle); return 1;
        }
        printf("{\"diagnostics_abi\":%u,\"removed\":%s,\"events\":{", snapshot.version,
               (snapshot.flags & OL_DIAG_REMOVED) ? "true" : "false");
        for (unsigned event = 0; event < OL_EVENT_COUNT; ++event)
            printf("%s\"%s\":%llu", event ? "," : "", names[event],
                   (unsigned long long)snapshot.events[event]);
        printf("},\"bluetooth_channel_opened_by_this_command\":false}\n");
        CloseHandle(handle); return 0;
    }
    if (discover) {
        error = perform(handle, OP_IOCTL, OL_IOCTL_CONNECT, &request, sizeof(request),
                        NULL, 0, &bytes, 15000);
        if (error) {
            fprintf(stderr, "Connect failed (Windows error %lu).\n", error);
            CloseHandle(handle); return 1;
        }
    }
    error = perform(handle, OP_IOCTL, OL_IOCTL_QUERY, &request, sizeof(request),
                    &state, sizeof(state), &bytes, 5000);
    if (error || bytes != sizeof(state) || state.size != sizeof(state) || state.version != OL_NATIVE_ABI) {
        fprintf(stderr, "Status query failed or ABI mismatch.\n"); CloseHandle(handle); return 1;
    }
    if (!discover) {
        printf("{\"abi\":%u,\"connection_state\":%u,\"outbound_mtu\":%u,\"inbound_mtu\":%u,"
               "\"connections\":%u,\"retained_connections\":%u,\"in_flight\":%u,\"bytes_sent\":%llu,\"bytes_received\":%llu,"
               "\"bluetooth_channel_opened_by_this_command\":false}\n",
               state.version, state.connection_state, state.outbound_mtu, state.inbound_mtu,
               state.connections, state.retained_connections, state.in_flight, (unsigned long long)state.bytes_sent,
               (unsigned long long)state.bytes_received);
        CloseHandle(handle); return 0;
    }
    unsigned char command[2] = {0x10, 1}; /* transaction 1, DISCOVER command */
    unsigned char response[OL_MAX_PACKET];
    error = perform(handle, OP_WRITE, 0, command, sizeof(command), NULL, 0, &bytes, 5000);
    if (!error && bytes != sizeof(command)) error = ERROR_WRITE_FAULT;
    if (!error) error = perform(handle, OP_READ, 0, NULL, 0, response, sizeof(response), &bytes, 5000);
    wl_avdtp_message message;
    uint8_t sink = 0;
    if (error || wl_avdtp_parse(response, bytes, &message) || message.transaction != 1 ||
        message.signal != 1 || message.message_type != 2 ||
        wl_avdtp_first_audio_sink(message.payload, message.payload_size, &sink)) {
        fprintf(stderr, "No matching unfragmented DISCOVER acceptance (Windows error %lu).\n", error);
        CloseHandle(handle); return 1;
    }
    printf("{\"avdtp_discover_accepted\":true,\"audio_sink_seid\":%u,"
           "\"ldac_negotiated\":false,\"audio_streamed\":false}\n", sink);
    CloseHandle(handle);
    return 0;
}
