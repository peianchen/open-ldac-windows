# Native channel and audio-worker prototype

This development path connects the existing Windows transport interface to the
user-mode AVDTP session, LDAC encoder and RTP packetizer. The first source is a
read-only WAV file. A VM-only signed transport has sent a two-second 990 kbps
stereo test tone through the real MediaTek radio to an XM6; the user reported
hearing both ears at a suitable volume. This is limited physical interoperability
evidence, not a Windows sound-output endpoint or a sound-quality/power measurement.
See the [short-tone report](validation/XM6_CONNECT_FIX_2026-11-07.md) and
[longer-playback campaign](validation/XM6_SUSTAINED_2026-11-07.md). That campaign
also completed three separate 60-second manual-change sessions, alongside
preserved pacing failures. The user reported both ears audible without perceived
anomalies for those three signals; this does not establish measured fidelity or
sustained reliability. A planned 600-second HQ run stopped after about 93.39
seconds of source input on the unchanged pacing guard; asynchronous resource
retirement also outlasted the initial ten-second observation window before a
later query showed zero retained objects. The final verified VM state was the
paired stock snapshot with Secure Boot enabled, no OpenLdac driver/service/test
certificate/package, and normal power-off; the radio was returned to the host.

## Ordering and ownership

`wl_playback_run` is a serialized owner-thread pipeline. Its I/O interface has
both a deterministic test peer and a real Windows implementation:

1. Validate stereo PCM format/rate and the exact requested bitrate before I/O.
2. Open the selected service's signaling handle through SetupAPI and the
   existing `OL_IOCTL_CONNECT`; verify ABI/status and actual negotiated MTUs.
3. Discover/select an exact-rate stereo LDAC endpoint; receive configuration
   and OPEN acceptance before opening a second handle for the media channel.
4. Initialize the encoder using the actual media MTU, report media readiness
   and require START acceptance before sending RTP/LDAC packets.
5. Feed bounded 128-frame PCM blocks; pad only the last short block, then drain
   the codec with a finite limit. Packet offsets use encoded sample duration,
   including codec lookahead/padding, rather than a per-packet Sleep loop.
6. On normal EOF, wait until the later of the encoded sample endpoint and
   the last successful media-write completion plus that packet's duration.
   Send CLOSE, require its matching acceptance, then retire the local media
   handle before signaling. CLOSE acceptance alone is not local retirement.
   Every exit closes media before signaling. Stop/error does not start new
   best-effort signaling transactions while cancellation is in progress.

The EOF wait uses upward-rounded QPC deadlines and one-shot timers, preserving
stop/disconnect monitoring. It is a sender timing lower bound, not confirmation
that a headset has rendered its jitter-buffer tail. No guessed receiver latency
is added. The 40 ms streaming lateness limit remains unchanged; terminal drain
may finish late rather than cutting short a successfully transmitted final packet.
CLOSE acceptance plus local I/O/handle retirement does not prove the kernel's
asynchronous BRB close or peer L2CAP disconnect has completed. Delay reporting,
real closure traces and tail-audio capture remain physical validation gates.
See the [2026-11-06 drain/CLOSE report](validation/GRACEFUL_CLOSE_2026-11-06.md).

Two existing kernel handles already provide separate L2CAP channels on assigned
PSM 0x0019. The CONNECT completion-information correction is recorded separately
in the short-tone report; it preserves the kernel ABI. The experimental VM uses
a guarded, VM-only derived INF. Public model-specific binding and production signing/load gates
still apply. An ordinary file open/query is separate from the explicit connect.

## Quality and scheduling

HQ is the default. Numeric manual choices retain the source-rate family:
990/660/330 kbps for 48/96 kHz and 909/606/303 for 44.1/88.2 kHz. Explicit
runtime requests are serialized through `wl_encoder_set_bitrate`; buffered old
frames may still emerge during a mode change. Requested and current encoder
rates are different observations. AC/battery state never changes the choice.

There is no resampler, EQ or volume conversion. Unsupported rates/channel
formats and media MTU below the encoder's 679-byte minimum fail explicitly.
The transport caps packets at 1008 bytes. The pinned codec reserves its own
transport overhead; RTP/LDAC packet assembly also checks the actual MTU.

The Windows adapter anchors sample deadlines to QPC and waits on a one-shot
waitable timer plus stop/signaling events. More than 40 ms of scheduling
lateness fails this prototype rather than catching up with an unbounded burst,
dropping PCM or silently lowering quality. This is an engineering limit, not a
measured RF jitter optimum or an underrun-free promise. No global timer
resolution, periodic background poll or battery downgrade is used.

The playback owner thread requests the existing MMCSS `Audio` task using
`AvSetMmThreadCharacteristicsW`, then calls `AvRevertMmThreadCharacteristics`
on that same thread after native cleanup. It does not override task priority,
edit the registry/power plan, or change system timer resolution. Registration
failure retains the existing scheduling path and is reported rather than
silently claiming MMCSS support. The final `mmcss` object records attempted,
registered, revert-attempted, reverted and error states. The no-RF SCM selftest
skips this association. API registration success is not sustained-playback proof;
the 40 ms lateness guard still applies.

The current adapter requests an unnamed high-resolution one-shot waitable timer.
Only `ERROR_INVALID_PARAMETER` from that request permits fallback to the ordinary
timer; allocation/security failures are not masked by fallback. The final timing
object records selection, fallback and the original high-resolution creation
error, plus separate observations of lateness before waiting and after timer
wake. These are locations where delay was observed, not proof of its ultimate
cause. Selecting a high-resolution timer changes the local wait mechanism; it is
not a measured power optimization or a guarantee of deadlines. No global timer
resolution setting is requested.

## Cancellation and peer signaling

The real adapter uses OVERLAPPED I/O with stable event/request/buffer storage.
Stop or timeout calls CancelIoEx and waits for the actual completion before
reusing/freeing storage. ERROR_NOT_FOUND may be a completion/cancel race; it
does not permit skipping retirement. An uncooperative lower driver can stall
the owner during drain. This version preserves memory safety instead of
claiming a hard bound on a lower driver's completion time.

A persistent signaling read watches streaming waits/media sends. Established
single-SEP peer SUSPEND/START/CLOSE/ABORT use the bounded prepare/commit path
described below; unsupported commands and malformed traffic fail closed. Full
AVDTP interoperability and peer RECONFIGURE are not implemented by this subset.
Incoming transaction responses are reassembled within the bounded
[signaling reassembly contract](SIGNALING_REASSEMBLY.md). The pending read is reused for normal EOF signaling,
and drained before closing the signaling handle. The existing four-bit wire
label limitation remains documented in [the session contract](AVDTP_SESSION.md).

## Foreground and service modes

The Windows executable is `wldac_player`. Help and inspection do not connect:

```powershell
wldac_player.exe --help
wldac_player.exe --inspect "source.wav"
```

The foreground prototype requires explicit connection intent:

```powershell
wldac_player.exe --play "source.wav" --connect --device-index 0 --bitrate 990 --result "new-result.json"
```

This example is for a prepared, signed and recoverable physical test target.
It does not install or bind a transport driver. Until that separate gate is
met, it cannot produce headset audio on an unprepared daily-use machine.

The SCM worker is named `OpenLdacAudioLab`; `--service --play ... --connect`
runs through ServiceMain/HandlerEx. Its control handler signals stop and
records explicit user requests; it does not wait for transport I/O. The worker
reports STOP_PENDING and waits for owner cleanup before STOPPED. User-defined
controls 128/129/130 request HQ/SQ/MQ respectively. No service installer,
automatic registration or autorun is performed by the executable.

`--service-selftest --result <new.json>` exercises SCM control/status/stop
without Bluetooth. Its 48 kHz mapping is explicitly synthetic and does not
claim codec negotiation. Only the isolated VM test helper registers this
temporary manual service. Results distinguish that test from actual playback.

## Durable playback result and interpretation

Foreground playback and `--service --play` accept `--result <new.json>`. The
Windows path is opened with `CREATE_NEW` before dispatching the service or
starting native playback. An existing file or an uncreatable destination aborts
before Bluetooth I/O; the program never overwrites an earlier report. The final
JSON is written and flushed after worker cleanup. This is a final report, not a
live progress stream; interruption can leave an incomplete file. Write/flush or
close failure must be checked through the process/service outcome as well.
`exit_code_before_report` deliberately precedes result-output errors.

`hq_controls`, `sq_controls`, and `mq_controls` count requests accepted by the
SCM handler. The request slot is consumed with an atomic exchange to zero; it
is not durable history of the latest control. Historical reports call this
`latest_control_requested_kbps`; that name is misleading; current builds use
`pending_control_requested_kbps`. A zero value can mean the owner already
consumed the request. Neither the slot nor acceptance counts prove that every
request reached the encoder: closely spaced requests
can replace one another before the owner reads them. Compare these with
`bitrate_changes`, `requested_kbps`, and `actual_kbps` in the completed playback
attempt. A final codec rate does not by itself demonstrate uninterrupted or
correct headset rendering at every intermediate rate.

`attempts` and `restarts` describe optional bounded reconnection. The policy is
`whole_source`: a retry starts the WAV from the beginning, preserves the explicit
manual target, and does not promise seamless sample-position recovery. Media,
source-frame and bitrate-change counters have `counters_scope: last_attempt`;
they must not be summed as if they represented all retries.
`last_attempt_media_write_completed` means at least one sender media packet
completed; it does not mean the whole source played or the user heard it.

Timing-enabled builds add a `timing` object whose scope is
`whole_adapter_all_attempts`, separate from last-attempt playback counters. It
records pacing lateness/misses, I/O wait timeouts, completion timeout errors and
media-write durations. `last_timeout_source` identifies the observed adapter
failure category; older result files without these fields cannot retrospectively
prove that category. A measured pacing miss identifies the guard that stopped
the run, not which operating-system or hypervisor activity caused the delay.

Guest process CPU seconds, wall time and memory samples are useful for diagnosing
this worker. `Process.TotalProcessorTime` includes both user time and privileged
(kernel) time attributed to that process, as specified by
[Microsoft](https://learn.microsoft.com/en-us/dotnet/api/system.diagnostics.process.totalprocessortime?view=netframework-4.7.2).
It does not measure the complete guest/host, other processes, hypervisor or
hardware energy costs, so it is not laptop battery-life or electrical power. Keep HQ and explicit manual choices unchanged while investigating
scheduling or transport failures; this prototype does not lower bitrate based on
AC/battery state.

## Input and evidence

The WAV reader validates complete RIFF chunk bounds, physical length, data
alignment and supported PCM16/24/32/float32 stereo formats before channel open.
Windows paths are explicitly UTF-8/UTF-16 converted. It opens the source only
for reading. Nonfinite float samples are rejected before encoding as blocks
are read; structural preflight does not claim to inspect every sample value.

Portable peer fixtures exercise the actual codec and RTP path; Windows named
pipes exercise the production OVERLAPPED helper; SCM checks exercise the real
Windows service manager in the VM. Each answers a different question. The
[dated test report](validation/PLAYBACK_USERMODE_2026-11-04.md) records executed
results, identities and limitations. None replaces an actual Bluetooth trace,
listening test, sleep/reconnect trial or electrical measurement.

Primary contracts: [L2CAP client](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/creating-a-l2cap-client-connection-to-a-remote-device),
[CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex),
[service control](https://learn.microsoft.com/en-us/windows/win32/services/service-control-handler-function),
[waitable timers](https://learn.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects).

## One deadline per signaling transaction

The required `now_ms` adapter callback uses Windows GetTickCount64. Each outgoing
command and all response fragments share one 5,000 ms budget. Each receive gets
only the remaining time; progress never renews the deadline. A response observed
at or after the deadline is rejected even when complete. Stop takes precedence.
This bounds protocol waiting, not an uncooperative lower driver's cancellation
retirement, which still must be drained before storage can be released.

## Peer-control continuation

Established single-SEP peer SUSPEND/START/CLOSE/ABORT now use a separate prepare/commit path after reassembly. Optional bounded reconnect explicitly restarts WAV and preserves manual bitrate. See [design and limitations](PEER_CONTROL_RECONNECT.md). The response-only session API still rejects commands; setup and simultaneous local CLOSE collisions remain fail-closed. The later physical campaign observed an XM6 peer CLOSE during user power-off
and local cleanup, but no automatic restart: normal peer CLOSE is terminal for
this policy. Two explicit starts after power-on then failed CONNECT with Win32
71 before media. A later external fresh SDP query followed by a new worker
completed 90 seconds at 660 kbps with attempts 1/restarts 0. This does not prove
automatic reconnect or a causal fix for error 71. Reconnect acceptance remains
unmet. AVRCP and the other physical
peer-control cases remain separate gates; follow the dated campaign for exact
evidence and limits.
