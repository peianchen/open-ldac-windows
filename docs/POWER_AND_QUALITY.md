# Laptop power and audio quality

Status: design requirements and offline encoder measurements only. No electrical
power, battery life, live Bluetooth, or perceptual audio measurements exist yet.

## Defaults

| Profile | LDAC target | Rate policy | Automatic link adaptation |
| --- | --- | --- | --- |
| Quality (default) | HQ: 909/990 kbps | Preserve 44.1/48 kHz; higher rates opt-in | Disabled; report unstable link |
| Balanced | Start at HQ | Same source-rate policy | Policy permits adaptation; runtime not implemented |
| Eco (explicit choice) | SQ: 606/660 kbps | 44.1/48 kHz | Policy permits adaptation; runtime not implemented |

909/606 kbps apply to the 44.1 kHz family, 990/660 to the 48 kHz family.
These are upstream codec specifications, not measured radio throughput.
Unplugging the laptop does not change the chosen quality. Never upsample every
stream to 96 kHz to display a higher number. The policy helper only recommends a
rate; M0 has **no resampler** and the offline encoder encodes the WAV's actual rate.
No claim of losslessness or a listening advantage follows from the bitrate.

## Runtime architecture requirements (not implemented in M0)

- Keep encoder in a user-mode process, using the floating-point hardware. Do not
  put codec math or dynamic allocations in the kernel streaming path.
- Use event-driven PCM delivery; wait on PCM, completed sends, stop and device
  notifications. No polling threads, busy-wait, `timeBeginPeriod(1)`, or global
  timer-resolution changes.
- Preallocate bounded PCM and encoded-packet buffers when a stream starts.
  A 10 ms application batch and a 40 ms queue ceiling are initial design targets,
  not measured optimal values or license to exceed the negotiated L2CAP MTU.
- The codec still processes 128 samples/channel per call. Batch multiple calls
  per audio event; do not wake a thread for every individual codec block.
- Stop encoding and release stream power references when no audio client runs.
  Silence within an active audio stream is still valid audio and must not cause
  an unsolicited disconnect. Suspend/resume must cancel/rebuild transport safely.
- Do not power down or rebind the shared Bluetooth radio. Mouse/keyboard and
  other profiles must remain functional.
- Drive adaptation from actual send completions/backlog, with hysteresis and a
  documented quality floor. Battery state alone is not an RF quality signal.
- Measure batching against added latency; large queues can waste retransmission
  energy and cause stale audio. Do not claim HQ is the most energy-efficient
  profile without RF/CPU/battery measurements.

## Reproducible measurement plan

1. Offline `wldac_cli bench`: reports process CPU time divided by represented
   audio duration, in one-core equivalent percent. Runs faster than real time,
   does not sleep, and measures **no watts**. Use repeated runs and keep raw JSON.
2. Native transport: compare SBC/AAC baseline, LDAC HQ and SQ at 44.1/48 kHz,
   with optional native-rate 96 kHz. Record drops/retries, negotiated codec and
   rate, queue depth, underruns and encode duration. No fabricated RF counters.
3. End-to-end: record Windows Performance Recorder traces for CPU scheduling,
   DPC/ISR, timer resolution, and audio glitches. Test stopped/idle, playing,
   pause/resume, lid/sleep, AC/battery and keyboard/mouse coexistence.
4. Electrical A/B: fixed laptop, screen brightness, Wi-Fi, track, headphone
   volume, distance and battery state. Randomize order, allow settling, repeat
   runs, retain duration and battery energy readings. An external power meter
   or validated battery energy counters are needed for energy conclusions.
   An offline CPU ratio cannot be converted directly into watts or battery hours.
5. Listening/quality: use a level-matched music corpus and blind comparisons,
   log codec configuration and dropout behavior. A periodic synthetic tone is
   solely an encoder smoke/throughput fixture.

## References

- Upstream API specification: `vendor/libldac/inc/ldacBT.h`
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio
- https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/audio-subsystem-power-management-for-modern-standby-platforms
