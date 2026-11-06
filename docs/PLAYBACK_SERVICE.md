# Native channel and audio-worker prototype

This development path connects the existing Windows transport interface to the
user-mode AVDTP session, LDAC encoder and RTP packetizer. The first source is a
read-only WAV file. It is not a Windows sound-output endpoint and has not yet
established MediaTek/XM6 interoperability or measured sound quality/power.

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
PSM 0x0019. This iteration does not change the kernel ABI, driver binary or INF.
The existing model-specific service binding and production signing/load gates
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

## Cancellation and peer signaling

The real adapter uses OVERLAPPED I/O with stable event/request/buffer storage.
Stop or timeout calls CancelIoEx and waits for the actual completion before
reusing/freeing storage. ERROR_NOT_FOUND may be a completion/cancel race; it
does not permit skipping retirement. An uncooperative lower driver can stall
the owner during drain. This version preserves memory safety instead of
claiming a hard bound on a lower driver's completion time.

A persistent signaling read watches streaming waits/media sends. This subset
fails closed on an unsolicited packet or signaling-channel loss; it does not
yet implement peer-initiated SUSPEND/RECONFIGURE or full AVDTP interoperability.
Incoming transaction responses can now be reassembled within the bounded
[signaling reassembly contract](SIGNALING_REASSEMBLY.md); unsolicited streaming
traffic still fails closed. The pending read is reused for normal EOF signaling,
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
wldac_player.exe --play "source.wav" --connect --device-index 0 --bitrate 990
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

Established single-SEP peer SUSPEND/START/CLOSE/ABORT now use a separate prepare/commit path after reassembly. Optional bounded reconnect explicitly restarts WAV and preserves manual bitrate. See [design and limitations](PEER_CONTROL_RECONNECT.md). The response-only session API still rejects commands; setup and simultaneous local CLOSE collisions remain fail-closed. AVRCP and physical headset interoperability are unverified.
