# Manual LDAC bitrate control

Implemented: exact numeric bitrate selection in the offline tool, supported-rate
listing and a user-mode encoder API for changing bitrate between encode calls.
The original hq/sq/mq CLI labels continue to work. The native-channel prototype
now has an explicit SCM worker accepting manual requests, with owner-thread codec
changes tested through a fake peer. A real XM6 received short 990 kbps playback
from the isolated VM, and the user confirmed hearing both ears. Three later VM
tests each completed a 60-second source with one early manual change: 990→660, 660→330, and 330→990 kbps. These were separate sessions, not
one continuous three-change stream. The user reported hearing both ears without
perceived anomalies for these three signals. Endurance and reconnect acceptance
remain unmet. A 600-second fixed-HQ attempt stopped after about
93.39 seconds of source input and did not pass endurance. After a separate
headset power cycle, two explicit restarts at requested 660 kbps failed CONNECT
before media. External fresh SDP followed by a new worker later completed
90 seconds at 660 kbps, with no within-worker restart. Preserving the request
is not proof of automatic reconnect. The test VM was restored to its verified
clean stock state and powered off; no experimental host installation is implied. A
settings UI and Windows PCM endpoint remain unfinished. See [the worker contract](PLAYBACK_SERVICE.md).

| Source sampling family | HQ (default) | SQ | MQ |
| --- | --- | --- | --- |
| 44.1 / 88.2 kHz | 909 kbps | 606 kbps | 303 kbps |
| 48 / 96 kHz | 990 kbps | 660 kbps | 330 kbps |

These are nominal stereo codec rates from the pinned Sony/AOSP API. They are not
measured Bluetooth throughput or proof of listening quality. The program keeps
the WAV's actual sample rate. Asking for 990 kbps with 44.1 kHz input fails instead
of resampling or silently selecting 909 kbps. Choosing hq selects the appropriate
highest rate for either family. Battery power does not change the selection.

## Offline tool

```powershell
# Show valid rates for a 48 kHz source.
./build/wldac_cli.exe bitrates 48000

# Highest quality is the default when no selector is specified.
./build/wldac_cli.exe encode input.wav new-output.ldac

# Explicit fixed rate; input must be 48 or 96 kHz.
./build/wldac_cli.exe encode input.wav new-output.ldac --bitrate 990
./build/wldac_cli.exe encode input.wav new-output.ldac --bitrate 660

# Original labels and direct numeric selectors remain supported.
./build/wldac_cli.exe encode input.wav new-output.ldac hq
./build/wldac_cli.exe encode input.wav new-output.ldac 330

# Synthetic offline CPU test, not a power/battery measurement.
./build/wldac_cli.exe bench 48000 --bitrate 990 30
```

Use a new output path for each encode: the tool refuses to overwrite existing
files. JSON reports requested_kbps, encoder_kbps and eqmid, and explicitly reports
bluetooth_transmitted=false. No registry setting, battery rule or resident service
is installed by these commands.

## Encoder API

`wl_bitrate_kbps(rate, eqmid)` and `wl_bitrate_eqmid(rate, kbps)` validate exact
supported combinations. `wl_encoder_set_bitrate(encoder, kbps)` invokes the pinned
codec's ldacBT_set_eqmid without rebuilding the encoder. Serialize it with
push/close on the encoder's owning thread. A future settings UI must queue requests
to that thread; it must not call the handle concurrently with encoding.

`requested_eqmid` records the accepted target; `wl_encoder_bitrate` queries the
codec's current nominal rate. Already buffered frames may still use the preceding
mode during a change. This API does not confirm a headset's negotiated codec,
reconfigure AVDTP, or prove that changing quality during real playback is safe.

## Validation

The prototype service accepts user-defined controls 128/129/130 for HQ/SQ/MQ,
mapped to the active source family. Its handler queues an atomic request; the
encoding owner applies it. The VM SCM selftest validates synthetic control
mapping only, while portable playback tests exercise the real codec change.
Those tests alone do not establish that an XM6 has played a requested mode.
The separate [short physical test](validation/XM6_CONNECT_FIX_2026-11-07.md) covers
990 kbps and subjective bilateral audibility only. The
[longer-playback campaign](validation/XM6_SUSTAINED_2026-11-07.md) records live
control/reconnect gates and failed attempts without extrapolating from it. Its
three completed 60-second live-control cases used a request at about three
seconds, not the original twenty-second control gate; each reported one applied
change and the corresponding final 660, 330, or 990 kbps encoder rate. Earlier
pacing failures remain evidence against a general reliability claim. This does
not establish measured fidelity; the user separately reported bilateral
audibility without perceived anomalies for these three signals only. No
service is automatically installed or started by the executable.

The native player can retain a final `--result <new.json>` report; an existing
path is rejected before native I/O. Accepted SCM control counts record accepted
requests. The queue slot is consumed and reset to zero; older reports misleadingly
name it `latest_control_requested_kbps`; current builds use
`pending_control_requested_kbps`. It is not a durable latest-request
record. `bitrate_changes` and `actual_kbps` describe the final playback attempt.
Rapid requests may coalesce.
A control-command exit of zero alone is not a successful physical mode-change
test. Reconnect restarts the whole WAV and retains the manual request; media and
change counters cover the last attempt, not all attempts.

Core tests change modes repeatedly on all four supported sample rates, inspect
emitted frame headers and sizes after settling, and verify invalid requests leave
the selected encoder mode intact. Integration tests independently inspect output
frames for each numeric mode, check default HQ, reject cross-family/invalid values,
and preserve input/existing output. Basic short real streaming and user-reported
hearing are documented separately; measured fidelity, sustained reliability and
battery/power comparisons are still outstanding. Process CPU measurements can
characterize the tested worker but cannot stand in for whole-laptop power or
representative music quality. No automatic bitrate adaptation is enabled by this
feature.

Primary source: pinned `vendor/libldac/inc/ldacBT.h` bitrate table and
ldacBT_set_eqmid/ldacBT_get_bitrate contracts; source revision is recorded in
`vendor/libldac/UPSTREAM.md`.
