# Open LDAC Windows

Latest physical result: [XM6 manual bitrate, endurance and recovery investigation](docs/validation/XM6_SUSTAINED_2026-11-07.md). Three 60-second streams completed with early manual 990→660, 660→330 and 330→990 changes; the user heard both ears without noticing anomalies. The 600-second 990 kbps trial stopped after about 93.4 seconds of source at the unchanged 40 ms pacing guard. Power-off closed safely, but two explicit reopens failed; a fresh service query followed by a new 90-second 660 kbps stream succeeded. This is not automatic reconnect acceptance, measured fidelity or battery-life validation. No experimental host driver is installed.

[繁體中文](README.zh-TW.md)

Traditional Chinese: [research background](docs/RESEARCH_BACKGROUND.zh-TW.md),
[development tutorial](docs/DEVELOPMENT_GUIDE.zh-TW.md),
[development journal](docs/DEVELOPMENT_JOURNAL.zh-TW.md), and
[executed kernel-lab record](docs/validation/KERNEL_LAB_2026-11-01.md).
Previous: [bounded signaling reassembly and transaction deadlines](docs/validation/SIGNALING_REASSEMBLY_2026-11-06.md).
Previous: [completion-aware sender drain and normal CLOSE, 2026-11-06](docs/validation/GRACEFUL_CLOSE_2026-11-06.md).
Previous: [native channel/audio-worker implementation and executed VM tests, 2026-11-04](docs/validation/PLAYBACK_USERMODE_2026-11-04.md).
Previous: [observed WDF low-resource failures and recovery](docs/validation/KERNEL_LOW_RESOURCE_2026-11-04.md).
Previous: [deferred OPEN cancellation and two Verifier profiles, 2026-11-03](docs/validation/KERNEL_LAB_2026-11-03.md).

An independent open-source project developing LDAC playback through Windows'
native Bluetooth stack, with laptop power consumption treated as a core requirement.

**Development: offline core and an experimental VM-only native transport, with
short 990 kbps XM6 streaming, user-reported short-tone bilateral audibility,
and three 60-second sender-side manual bitrate cases recorded.
No experimental driver is installed on the daily-use host; the Windows audio
endpoint and release installer are not implemented. Build results alone remain
distinct from the recorded hardware and listening evidence.**

## What works in this milestone

- Real LDAC encoding using pinned AOSP/Sony `libldac` source, with complete license
  and notice files. Original project glue is Apache-2.0.
- Stereo PCM16/24/32 and float32 RIFF WAV input at 44.1/48/88.2/96 kHz. Writes
  concatenated LDAC transport frames; this offline encode command does not resample, decode, or transmit Bluetooth audio.
- LDAC capability selection, basic single-packet AVDTP discovery/capability
  parsing and configuration builders, and bounded RTP/LDAC media packet assembly.
- A bounded single-packet [AVDTP initiator session](docs/AVDTP_SESSION.md): endpoint traversal, exact PCM rate, media-ready gating and terminal error handling. A short 48 kHz/990 kbps XM6 path has now been exercised through the VM transport; this is not complete AVDTP compliance.
- Experimental real XM6 playback of 2-second stereo test tones at 990 kbps, with
  sender completion, normal channel retirement, and later user-reported hearing
  in both ears. The transport was installed in the isolated VM only.
- Three real 60-second manual bitrate cases with one applied change each (990→660,
  660→330, 330→990), tested with early controls. The original later-control
  cases failed before their controls; those failures are retained.
- Durable native-playback result reports, per-owner Audio MMCSS, per-adapter
  high-resolution one-shot timing and bounded timing diagnostics. These changes
  do not remove the 40 ms protection or establish a battery-life improvement.
- Explicit quality/power policy and a reproducible offline encoder CPU benchmark.
- Tests for malformed input, MTU limits, packet rollover, codec configurations,
  file preservation and frame headers. Tests do not prove audio quality or playback.

## Laptop-first policy

Default to HQ at 44.1/48 kHz, retaining a directly compatible source rate.
Higher rates require an explicit choice. Unplugging a laptop does not silently
lower quality. An explicit Eco profile can select SQ. Runtime capture/transport
must use events and bounded buffers; no busy-waiting or global 1 ms timer changes.

These are implementation requirements, not a claim of improved battery life.
Read [power and quality](docs/POWER_AND_QUALITY.md) for profile definitions,
measurement limits and the electrical A/B test plan.

## Build the offline core

With CMake and a C11 compiler:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tests/integration.py build/Release/wldac_cli.exe
```

For a portable Zig 0.14.1 compiler on Windows:

```powershell
./tools/build-zig.ps1 -Zig C:/path/to/zig.exe
python tests/integration.py build/wldac_cli.exe
```

The build script only builds user-mode executables and runs offline tests.
It does not install a driver or change boot/security settings. The compiler is
not bundled in the source repository. Linux CMake executable paths have no
`Release/` subdirectory or `.exe` suffix.

## Use the offline tools

```powershell
./build/wldac_cli.exe encode input.wav new-output.ldac hq
./build/wldac_cli.exe bitrates 48000
./build/wldac_cli.exe encode input.wav new-output.ldac --bitrate 990
./build/wldac_cli.exe bench 48000 hq 300
python tools/benchmark.py build/wldac_cli.exe --output build/cpu-report.json
```

The encoder refuses to overwrite an existing output. WAVE_FORMAT_EXTENSIBLE and
compressed WAV are currently rejected. An offline LDAC file is not a playable
Windows endpoint. Benchmarks measure synthetic encoder CPU cost; **no watts,
battery hours, RF retransmissions, or listening scores are measured**.

Manual numeric selection supports 990/660/330 kbps for the 48/96 kHz family and
909/606/303 kbps for 44.1/88.2 kHz. Omitting the encode selector defaults to HQ.
The user-mode API supports serialized encoder-mode changes, and the experimental
SCM worker queues manual requests. Short real-headset trials verified the fixed
990 kbps setting; other numeric rates and live bitrate switching remain unverified.
See [bitrate control](docs/BITRATE_CONTROL.md).

## Native Windows driver roadmap

| Milestone | Deliverable | Status |
| --- | --- | --- |
| M0 | Offline codec, protocol primitives, quality/power policy and tests | Implemented; see verification report |
| M1 | KMDF L2CAP transport, XM6-specific INF and discovery probe | Experimental VM load and real XM6 signaling verified; VM-only INF variant guarded separately |
| M2 | Complete AVDTP state machine and LDAC streaming to a real headset | Bounded path: short 990 kbps XM6 tone streaming and user hearing confirmed; full compliance and extended testing pending |
| M3 | Windows audio endpoint and event-driven user-mode encoding service | WAV/SCM worker prototype exists; Windows PCM endpoint not implemented |
| M4 | Power/latency/glitch measurements, suspend/resume and coexistence | Not measured |
| M5 | Release-signed, reviewed installer and recovery path | Guest-only test signing and guarded installation exercised; release installer not implemented |

The design keeps the existing Windows Bluetooth radio/transport drivers and
uses public Bluetooth profile DDIs. It does not claim that a registry key adds a
codec to Microsoft's A2DP driver. See [native architecture](docs/NATIVE_ARCHITECTURE.md)
and [driver build prerequisites](driver/README.md).
An isolated real-WDF lab with a synthetic provider passed its baseline and targeted
standard Verifier cases on `36bc35f`; see the [executed record](docs/validation/KERNEL_LAB_2026-11-01.md).
Real BthPort/PnP loading and the short XM6 path now have separate hardware evidence.
Additional fault profiles, long-duration operation and general hardware acceptance
remain pending; see the [kernel test matrix](docs/KERNEL_TEST_MATRIX.md). Host
adapter tests and the synthetic provider alone do not establish compatibility.

## License and provenance

- Original codec/glue/probe source: [Apache License 2.0](LICENSE).
- Microsoft-derived transport: [MS-PL](driver/LICENSE-MS-PL), with [provenance](driver/UPSTREAM.md).
- Encoder: AOSP/Sony, pinned commit recorded in [UPSTREAM.md](vendor/libldac/UPSTREAM.md).
- Keep [NOTICE](NOTICE) and [Sony's upstream NOTICE](vendor/libldac/NOTICE).
  The upstream notice mentions certification for use in products; this repository
  makes no certification claim and does not resolve product certification requirements.
- LDAC is Sony's trademark. This project is independent of Sony and BluetoothGoodies.
- No proprietary Alternative A2DP Driver binaries or source are included.

When reporting compatibility, distinguish a successful offline build, kernel
driver build, driver installation, negotiated LDAC streaming, and actual listening.
Do not include MAC addresses, private recordings, credentials or raw device dumps
in public issues. Read [CONTRIBUTING.md](CONTRIBUTING.md).

- [Physical transport preparation and disconnect-error validation](docs/validation/PHYSICAL_TRANSPORT_2026-11-06.md)
