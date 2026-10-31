# Native transport provenance

- Upstream: https://github.com/microsoft/Windows-driver-samples
- Commit: `f4c1ff842b6a0001152f44cb61788b216a1c7627`
- Origin: `bluetooth/bthecho/bthcli/sys/*.{c,h}`, `common/inc/*.h`, `common/lib/*.c`.
- Files are flattened into `transport/`; original Microsoft notices are retained.
- License: Microsoft Public License, retained as `LICENSE-MS-PL`. Derived source
  and `transport/control.c` are MS-PL, not Apache-2.0 or entirely original code.
- Original shared ABI/probe/build code is Apache-2.0.

Changes: Audio Sink UUID, project interface GUID, explicit-connect IOCTL, passive
file open/status, bounded channel/presented-transfer counts, packet/MTU validation,
encrypted Basic L2CAP, receive depth ten, counters and cleanup handling.

Lifetime changes include fail-closed close-submission handling, early initialization
of cleanup synchronization, failed-open/file-close null safety, closing/open race
handling and query-interface reference cleanup. These require kernel/runtime review.

Server continuous readers are not initialized by the client. No background service,
timer-resolution change, kernel codec math or Windows PCM endpoint is included.
Build/static INF checks are not evidence of cancellation, suspend or playback safety.

Known installation blocker: an immediate CLOSE submission failure does not prove
callback retirement. M1 deliberately does not release the connection on that
assumption; close may remain pending. A proven retirement/recovery path and
fault-injection testing are required before installation on the daily-use laptop.
