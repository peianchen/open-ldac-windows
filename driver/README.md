# Native Bluetooth transport — M1 development

This directory contains a KMDF L2CAP transport and a model-specific INF for the
observed Sony WH-1000XM6 Audio Sink service. It provides no Windows audio endpoint
or LDAC playback. No kernel driver has been installed or loaded on the laptop.

## Scope and power behavior

- Keep the current MediaTek radio driver and Microsoft Bluetooth stack. Bind only
  the XM6 Audio Sink model ID in the INF, never the shared radio or generic A2DP.
- Administrative/System access only. Ordinary file open/status does not open a
  channel. Explicit connect uses the bound XM6 Audio Sink address and fixed
  AVDTP PSM 0x0019, requesting an encrypted Basic L2CAP channel. No synchronous
  per-open SDP or arbitrary remote/PSM IOCTL is exposed.
- At most two file/channel slots, 1008-byte packet buffers, eight simultaneously
  presented transfers, plus actual negotiated MTU checks.
- Asynchronous, cancelable reads/writes; pending reads wait for data. No polling
  thread, timer-resolution change, codec math, or resident service.
- File cleanup detaches without waiting, requests cancellation and asynchronous
  close. Request buffers remain alive until actual I/O completion. Power-managed
  queue stop cancels sent I/O. Runtime sleep/removal correctness is unverified.
- Pointer-free status ABI v2 reports counters/MTUs and live/retained connection
  roots without MAC addresses. File handles and uncertain roots each have a cap
  of two per device, so repeated failures cannot grow the root list unboundedly.

The WDF waiting queue/request allocation is not a fixed streaming buffer pool.
M1 is diagnostic transport; allocation, batching, backpressure and power transitions
need runtime validation before enabling audio streaming. No energy saving is claimed.

## Build

With Visual Studio C++ and NuGet available:

```powershell
nuget restore driver/packages.config -PackagesDirectory driver/.packages -NonInteractive
msbuild driver/OpenLdacTransport.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off
```

Official WDK/SDK 10.0.26100.6584 is pinned. Native CI builds on `windows-2022`.
The output is an **unsigned development package**, not an end-user release.
`build-clang.ps1` checks eight translation units against extracted official WDK/SDK
headers; it does not link a SYS and does not replace MSBuild, analysis or signing.

## Diagnostic utility

Build `wldac_probe` through root CMake or `tools/build-zig.ps1`:

```powershell
./build/wldac_probe.exe --list
./build/wldac_probe.exe --status
# Only after separately reviewed driver installation:
./build/wldac_probe.exe --discover
```

List enumerates the project interface without opening a device. Status queries
ABI/MTU/counters without a channel connect. Discover explicitly opens a channel
and sends one AVDTP DISCOVER; it does not negotiate LDAC, start media or play audio.
The probe cancels timed-out I/O and waits for retirement before freeing OVERLAPPED
memory. Its soft timeout can be exceeded while the lower stack retires a request.

## Installation boundary

No auto-install, pairing reset, radio restart, test-signing or security-changing
script is included. Binding the XM6 Audio Sink service replaces its normal A2DP
function binding; this experimental driver provides no Windows sound output yet.
Other Bluetooth devices/radio bindings are not targeted by the INF.

Before a hardware trial: review lifetime/failure paths, build/INF/kernel analysis,
original Microsoft service-binding restoration, and a valid signing/test-machine
strategy. An unsigned CI artifact is not an install-ready LDAC driver.

The blocking close wait has been replaced by independent connection roots and
explicit owner/open/I/O/close/provider/dispatch leases. Close resources are reserved
before open, with at most two immediate failed-submit attempts. Close success or
failure alone never impersonates provider callback retirement. If retirement is
not observed, the root remains bounded and visible instead of being freed by a
timer. Further connects can return busy until it is released.

The callback contract with a non-null WDM `ReferenceObject`, device removal,
Verifier fault injection and a valid signing/rollback strategy are still **load
gates**, not established by host tests. See [lifecycle design](../docs/LIFETIME.md).

## Licensing

Microsoft-derived `transport/`, including modifications, uses [MS-PL](LICENSE-MS-PL).
Other original code remains Apache-2.0 unless stated otherwise.
See [UPSTREAM.md](UPSTREAM.md) for source provenance and changes.
