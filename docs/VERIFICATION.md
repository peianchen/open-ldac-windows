# Verification record — 2026-10-31

## Evidence boundary

M0 builds and runs **user-mode offline tools** on the development Windows x64
computer. It does not build, install or exercise a kernel driver. No claim of
Bluetooth playback, listening quality, radio throughput, battery life or electrical
power follows from this record.

## Build and tests

- Compiler: official Zig 0.14.1 portable distribution; downloaded archive verified
  against the SHA-256 in Zig's official release index:
  `554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c`.
- Build: C11, `-O2`, `x86_64-windows-gnu`, `-mcpu=baseline`. No machine-specific
  AVX requirement is requested. This is not a kernel-driver build toolchain.
- Core executable: **2,845 checks passed**, including real encoder calls at four
  sample rates and three quality levels, all supported PCM input word formats,
  drain behavior, nominal frame sizes/bitrates, MTU checks, AVDTP primitives,
  RTP sequence/timestamp rollover and battery quality policy.
- Integration: **54 cases passed**, including 48 combinations of WAV sample rate,
  encoding quality and PCM representation, malformed/truncated/unsupported WAV,
  preservation of existing output/input and independent inspection of every
  output frame's sync, sampling-rate ID, channel ID and declared length.
- No decoder/listening comparison was run. Valid headers and frame sizes do not
  establish perceptual quality or compatibility with a hardware decoder.
- Windows/Linux GitHub Actions workflow is supplied. Local CMake/MSVC/Linux and
  remote CI results are separate from the verified local Zig build.

The initial sandbox blocked compiler child processes and temporary-file behavior.
The compiler completed successfully outside that sandbox, using build-local
temporary/cache directories. No security or boot settings were modified.

## Offline synthetic encoder CPU comparison

Three runs per configuration, each representing about 300 seconds of stereo
float32 audio; alternate run order and separate warm-up runs. Figures below are
the median process CPU seconds / represented audio seconds × 100, in **one-core
equivalent percent**. They are an offline throughput extrapolation, not a
measurement of a running real-time audio service.

| Configuration | Median one-core equivalent |
| --- | ---: |
| 44.1 kHz HQ | 0.198% |
| 48 kHz HQ | 0.208% |
| 48 kHz SQ | 0.208% |
| 96 kHz HQ | 0.297% |

Raw baseline-target results and limits:
[encoder-cpu-baseline-x64-2026-10-31.json](encoder-cpu-baseline-x64-2026-10-31.json).
The earlier `encoder-cpu-2026-10-31.json` is an exploratory native-default-target
run and is not the build used for the table above.

These runs show no clear CPU saving from SQ versus HQ on this particular fixture.
96 kHz had higher CPU cost in this measurement. Neither result establishes an
electrical-energy difference, RF behavior or an audible advantage. The absolute
values are hardware/workload/build dependent and unsuitable as a product guarantee.

## Native build readiness

Read-only preflight found no modern Visual Studio C++ toolchain, Windows SDK
headers or WDK headers in standard installation locations. Custom EWDK/NuGet
locations were not exhaustively searched. Native build readiness is **false**.

No SYS, INF, signed package, installed driver, live AVDTP negotiation or
Windows audio endpoint exists in M0. Those are the next implementation stages,
not passing tests hidden behind the offline results.
