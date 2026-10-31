# M1 native verification — 2026-10-31

Target: existing Windows Bluetooth stack/MediaTek radio and the observed
WH-1000XM6 Audio Sink model. No service binding, boot, pairing or radio setting
has been changed on the development laptop.

## Current lifecycle revision

- Local eight-translation-unit compile against official WDK/SDK 26100.6584 passed.
  This secondary check does not link a SYS; Clang emits SDK-header visibility
  warnings, so the separate MSVC zero-warning CI gate remains required.
- The actual shared lifecycle engine passed 18,976 host checks across 1,020
  exhaustive event schedules. These are serialized host-model transitions,
  not WDF/IRQL/provider or real-device fault injection.
- Existing core: 2,850 checks; WAV/CLI integration: 54 cases, passed locally.
- Passive probe: `driver_interfaces: 0`, `bluetooth_channel_opened: false`.
  No project driver is installed. No Discover, binding or RF trial was run.
- Native MSBuild/analysis/InfVerif/package and Linux lifecycle ASan/UBSan results
  for this revision are tracked in PR #1 checks, separate from historical runs.

Historical native zero-warning analysis/package baseline at `5aa78a2`:
https://github.com/peianchen/open-ldac-windows/actions/runs/41195587086
Historical Windows/Linux offline baseline at that commit:
https://github.com/peianchen/open-ldac-windows/actions/runs/41195587061
Those historical passes do not validate the new lifecycle adapter.

Earlier failing runs were build failures (tool path and kernel ABI CRT headers),
not failed installations on the laptop.

## Installation gates not passed

The synchronous close hang is removed in source. Cleanup now detaches immediately,
cancels referenced requests and uses a preallocated asynchronous close request,
with bounded failed-submit attempts. It releases a connection only after its
leases retire; uncertainty remains as at most two retained roots per device.
This contains failure without proving provider recovery. See [lifecycle](LIFETIME.md).

Still required: actual callback-retirement contract with the non-null WDM
ReferenceObject, kernel Verifier/low-resource/cancel/removal/sleep tests, valid
signing and exact service-binding restoration on a separate test setup. The
unsigned diagnostic transport has no Windows audio endpoint. It is not ready for
installation on the daily-use laptop.

## Not measured

- Encrypted L2CAP/XM6 AVDTP response, LDAC configuration or streaming.
- Headset audio, listening quality, glitches, latency or reconnect behavior.
- Electrical consumption, battery life or shared-radio coexistence.

HQ remains the default on AC and battery. These tests make no quality, power or
hardware compatibility claim.
