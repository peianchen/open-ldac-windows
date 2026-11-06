# Playback negotiation: bounded initiator subset

The VM low-resource campaign on `969a9ef` passed its stated gates before this
user-mode playback work began. See the [executed resource-fault report](validation/KERNEL_LOW_RESOURCE_2026-11-04.md).
This module now feeds the [native-channel WAV/audio-worker prototype](PLAYBACK_SERVICE.md).
The Windows adapter is implemented and user-mode VM tests exist, but actual
BthPort/MediaTek/XM6 playback and a Windows PCM endpoint are still unverified.

## Contract and quality

`include/avdtp_session.h` and `src/avdtp_session.c` implement one outstanding
transaction on an owner thread. The caller supplies a fresh transport generation,
incoming packet events, send failures, deadlines and media-channel readiness.
The module allocates no heap, starts no threads/timers and performs no I/O or
polling. A fixed array admits at most 62 remote endpoints.

It discovers available audio sinks and queries their capabilities in sequence,
rather than assuming the first sink supports LDAC. Selection requires stereo
LDAC at exactly the caller's source sampling rate. An otherwise usable sink
requiring conversion is not configured: there is no resampler in this path.
Supplying an explicit 88.2/96 kHz source is supported; the module does not create
high-rate PCM by upsampling.

The proposed configuration contains Media Transport and LDAC only. Optional
advertised content protection is not claimed as negotiated support. The peer
must accept the exact unprotected proposal. Protected playback is unsupported.
HQ remains the encoder default on AC and battery; manual kbps is an encoder
setting, not an additional AVDTP codec field. The session does not adapt bitrate.
These policies do not establish measured listening quality or battery savings.

## Progression and failure

The command/accept sequence is DISCOVER, per-candidate GET_CAPABILITIES,
SET_CONFIGURATION and OPEN. OPEN acceptance reaches WAIT_MEDIA. Only the
transport owner's explicit `media_ready(generation)` permits START; only a
matching START acceptance reaches STREAMING. SUSPEND acceptance reaches
SUSPENDED, from which resume emits START. Configured idle sessions can ABORT;
an outstanding transaction returns BUSY, so its owner must complete it or
terminate the transport before restarting. ABORT acceptance invalidates the
transport epoch and the owner closes the channel.

Only matching transaction label, signal and local generation can advance a
pending response. Payload shape is checked before acceptance. Unknown/stale
response tuples do not advance state. Capacity/NULL failures during command
construction preserve state, transaction counters and output tokens.

Malformed matching responses, peer commands, raw fragments passed directly to
the complete-message session API, matching reject,
timeout and send/transport failure terminate this subset. It does not attempt
interoperability recovery by guessing peer state. The owner must close/retire
the old channels before initializing with a strictly higher generation.
Timers and send-failure callbacks carry generation plus a monotonic serial,
label and signal, protecting later work from stale local callbacks.

AVDTP has a four-bit wire transaction label. An arbitrarily ancient duplicate
with the same label/signal after wrap on the same channel cannot be identified
using that header alone. Local tokens do not add a serial to the peer's wire
reply. A timeout therefore invalidates the entire channel epoch rather than
reusing that timed-out transaction on it.

## Verification and next integration

`tests/avdtp_session_tests.c` uses synthetic wire fixtures: multi-endpoint
selection, exact rates, malformed/rejected responses, capacity guards,
generation/token handling and progression through media-ready/start/suspend/
resume/abort. It is included in CMake/CTest and the Windows Zig helper; the CI
sanitizer job also exercises it. Passing these tests is not a headset trace,
Bluetooth qualification or audio playback result.

Executed locally on 2026-11-04 with the existing Zig 0.14.1 Windows toolchain:
4,837 protocol CHECK evaluations passed. The same final build passed 5,805 core
codec/protocol/policy checks, 18,976 lifecycle checks (1,020 modeled schedules),
7,532 production-adapter host-shim checks and 104 WAV/CLI integration cases.
CHECK counts include helper assertions and repeated flows, not that many
independent scenarios. None of these host tests establishes kernel or RF behavior.

Independent review found that parsing an unrelated reserved-signal/oversized
response before matching its tuple could invalidate live work. The implementation
now matches a readable single-packet response header before validating a matching
payload. Regressions require unrelated traffic to leave the session byte-for-byte
unchanged; a matching oversized packet still invalidates transport. Short headers,
raw fragments passed directly to the session and peer commands keep the
explicitly unsupported/fail-closed behavior. Playback now reassembles incoming
transaction fragments in a separate framing layer before calling this API.

The next integration must provide asynchronous signaling I/O and separate media
channel lifetime, cancellation/deadline ownership, actual peer packet traces and
an explicit teardown path. Peer-initiated commands and interleaved signaling,
peer-initiated closure/reconfiguration/content protection and interoperability still
require development before claiming a complete AVDTP implementation. Windows
PCM endpoint/service, real PnP/sleep behavior, signing/restore and quality/power
measurements remain separate gates before daily-use installation.

Protocol references: Bluetooth SIG [AVDTP 1.3](https://www.bluetooth.com/specifications/specs/a-v-distribution-transport-protocol-1-3/),
sections 6.2 (transaction model), 8.4 (headers) and 8.6–8.16 (commands/response
formats). The associated erratum and complete profile requirements remain part
of any future conformity work; this subset makes no conformity claim.

## Normal CLOSE continuation (2026-11-06)

`request_close` emits signal 0x08 from STREAMING, SUSPENDED, or the established
OPEN state represented by START before a command is pending. Existing enum
values are preserved; CLOSE, WAIT_MEDIA_CLOSE and CLOSED are appended. A
matching empty acceptance reaches WAIT_MEDIA_CLOSE. Only the owner's
`media_closed(generation)` report advances to CLOSED and clears configuration.
This report means local ownership retirement; the current backend's handle close
does not provide proof of asynchronous BRB or remote L2CAP disconnect completion.
Failures invalidate the epoch; rejection never silently falls back to ABORT.

The Bluetooth SIG [AVDTP test suite p22](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2025/02/AVDTP.TS_.p22.pdf),
sections 4.3.1.14 and 4.3.1.43, specifies acceptance before media release and the
rejection format. These implementation tests do not constitute SIG qualification.
Executed current tests and remaining gates are in the
[dated report](validation/GRACEFUL_CLOSE_2026-11-06.md).

## Receive framing boundary (2026-11-06)

The session API still consumes complete normalized SINGLE messages. Raw fragments
passed directly to it remain unsupported. The playback transaction owner now
uses a separate [bounded reassembler](SIGNALING_REASSEMBLY.md) before calling
`wl_avdtp_session_receive`; this enables fragmented discovery/capability responses
without making the session own buffers, clocks or asynchronous I/O. Unmatched
complete response tuples are still filtered before semantic parsing. An active
fragment sequence cannot be interleaved with another message in this subset.
