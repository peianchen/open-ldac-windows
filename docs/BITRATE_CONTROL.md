# Manual LDAC bitrate control

Implemented: exact numeric bitrate selection in the offline tool, supported-rate
listing and a user-mode encoder API for changing bitrate between encode calls.
The original hq/sq/mq CLI labels continue to work. There is no live headset/UI
control yet; the Windows audio endpoint and streaming service remain unfinished.

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

Core tests change modes repeatedly on all four supported sample rates, inspect
emitted frame headers and sizes after settling, and verify invalid requests leave
the selected encoder mode intact. Integration tests independently inspect output
frames for each numeric mode, check default HQ, reject cross-family/invalid values,
and preserve input/existing output. Actual streaming, listening and RF behavior
remain unmeasured. No automatic bitrate adaptation is enabled by this feature.

Primary source: pinned `vendor/libldac/inc/ldacBT.h` bitrate table and
ldacBT_set_eqmid/ldacBT_get_bitrate contracts; source revision is recorded in
`vendor/libldac/UPSTREAM.md`.
