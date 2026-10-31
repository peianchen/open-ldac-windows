# Native transport provenance

- Upstream: https://github.com/microsoft/Windows-driver-samples
- Commit: `f4c1ff842b6a0001152f44cb61788b216a1c7627`
- Origin: `bluetooth/bthecho/bthcli/sys/*.{c,h}`, `common/inc/*.h`, `common/lib/*.c`.
- Files are flattened into `transport/`; original Microsoft notices are retained.
- License: Microsoft Public License, retained as `LICENSE-MS-PL`. Derived source
  and `transport/control.c` are MS-PL, not Apache-2.0 or entirely original code.
- Original shared ABI/lifetime/probe/build code is Apache-2.0.

Changes: Audio Sink UUID, project interface GUID, explicit-connect IOCTL, passive
file open/status, bounded channel/presented-transfer counts, packet/MTU validation,
encrypted Basic L2CAP, receive depth ten, counters and cleanup handling.
Non-WPP trace formats use ordinary printf-compatible NTSTATUS formatting, and
the connect declaration carries matching SAL annotations for static analysis.

Lifecycle redesign removes the unused server continuous-reader implementation,
per-open synchronous SDP and synchronous file-close wait. It adds independent
connection roots, bounded retained-root accounting, reserved close request/memory,
a shared freestanding lease engine, file pointer locking, request references and
pre/post-send cancellation checks for open and data I/O. The remote remains the
bound XM6 Audio Sink PDO; PSM 0x0019 is fixed, not supplied by user mode.

`src/lifetime.c`, `include/lifetime.h` and host tests are original Apache-2.0 code.
The surrounding WDF/Bluetooth adapter remains a Microsoft-derived MS-PL work.
No background service, timer-resolution change, kernel codec math or Windows PCM
endpoint is included. Host fault models/static INF checks are not evidence of
kernel cancellation, suspend, playback or battery safety. Provider retirement and
kernel fault tests remain installation gates; see `docs/LIFETIME.md`.
