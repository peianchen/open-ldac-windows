# Open LDAC Windows

## Research and development notes (Traditional Chinese)

This default branch remains the M0 offline milestone. The following documents
describe the ongoing M1 development branch, its executed synthetic-kernel tests,
and its remaining hardware/audio/power gates.

- [Background, principles and research methods](https://github.com/peianchen/open-ldac-windows/blob/611c4bc5d31a04a6a26352ef2ea198260094093c/docs/RESEARCH_BACKGROUND.zh-TW.md)
- [Step-by-step development tutorial](https://github.com/peianchen/open-ldac-windows/blob/611c4bc5d31a04a6a26352ef2ea198260094093c/docs/DEVELOPMENT_GUIDE.zh-TW.md)
- [Development journal and reflections](https://github.com/peianchen/open-ldac-windows/blob/7c39d232cfd73bf82ee0df90bf24eeb98adbff42/docs/DEVELOPMENT_JOURNAL.zh-TW.md)
- [Executed kernel-lab and standard Verifier record](https://github.com/peianchen/open-ldac-windows/blob/611c4bc5d31a04a6a26352ef2ea198260094093c/docs/validation/KERNEL_LAB_2026-11-01.md)
- [2026-11-03: deferred OPEN, standard and forced-pending Verifier](https://github.com/peianchen/open-ldac-windows/blob/896a3cab7478ad5d6e76806940000c625e290f11/docs/validation/KERNEL_LAB_2026-11-03.md)
- [2026-11-04: observed WDF resource failures, recovery and playback-session foundation](https://github.com/peianchen/open-ldac-windows/blob/ac0fe0eecb2da1e6c28d47a3e2973e01ece5228c/docs/validation/KERNEL_LOW_RESOURCE_2026-11-04.md)
- [2026-11-04: native channel/audio-worker integration and VM evidence](https://github.com/peianchen/open-ldac-windows/blob/7c39d232cfd73bf82ee0df90bf24eeb98adbff42/docs/validation/PLAYBACK_USERMODE_2026-11-04.md)

[繁體中文](README.zh-TW.md)

An independent open-source project developing LDAC playback through Windows'
native Bluetooth stack, with laptop power consumption treated as a core requirement.

**Development milestone M0: offline codec/protocol core. This is not an
installable audio driver and cannot send audio to Bluetooth headphones yet.**

## What works in this milestone

- Real LDAC encoding using pinned AOSP/Sony `libldac` source, with complete license
  and notice files. Original project glue is Apache-2.0.
- Stereo PCM16/24/32 and float32 RIFF WAV input at 44.1/48/88.2/96 kHz. Writes
  concatenated LDAC transport frames; no resampling, decoding, or Bluetooth output.
- LDAC capability selection, basic single-packet AVDTP discovery/capability
  parsing and configuration builders, and bounded RTP/LDAC media packet assembly.
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
./build/wldac_cli.exe bench 48000 hq 300
python tools/benchmark.py build/wldac_cli.exe --output build/cpu-report.json
```

The encoder refuses to overwrite an existing output. WAVE_FORMAT_EXTENSIBLE and
compressed WAV are currently rejected. An offline LDAC file is not a playable
Windows endpoint. Benchmarks measure synthetic encoder CPU cost; **no watts,
battery hours, RF retransmissions, or listening scores are measured**.

## Native Windows driver roadmap

| Milestone | Deliverable | Status |
| --- | --- | --- |
| M0 | Offline codec, protocol primitives, quality/power policy and tests | Implemented; see verification report |
| M1 | KMDF profile driver with cancelable L2CAP signaling/media channels | Not implemented |
| M2 | Complete AVDTP state machine and LDAC streaming to a real headset | Not implemented |
| M3 | Windows audio endpoint and event-driven user-mode encoding service | Not implemented |
| M4 | Power/latency/glitch measurements, suspend/resume and coexistence | Not measured |
| M5 | Signed, reviewed installer and recovery path | Not implemented |

The design keeps the existing Windows Bluetooth radio/transport drivers and
uses public Bluetooth profile DDIs. It does not claim that a registry key adds a
codec to Microsoft's A2DP driver. See [native architecture](docs/NATIVE_ARCHITECTURE.md)
and [driver build prerequisites](driver/README.md).

## License and provenance

- Project source: [Apache License 2.0](LICENSE).
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
