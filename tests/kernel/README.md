# Isolated kernel lab — synthetic provider

This separate KMDF test service compiles the production MS-PL
client/connection/control/queue and original lifecycle implementation against
**real WDF**, with an original synthetic provider. It has no dependency on a
physical radio and creates no Windows audio endpoint. It does not establish
BthPort, Audio Sink PDO/PnP, real MediaTek/XM6 or electrical-power correctness.

Status: source and local WDK-header compile implemented; runtime/Verifier results
must be recorded separately. Never interpret a build as a passed kernel test.

## Design

Two control devices keep the synthetic lower target distinct from the tested
transport. The transport target forwards real requests to the synthetic port,
which completes OPEN/CLOSE/write and holds reads for actual WDF cancellation.
File/request contexts, root lifetimes, I/O slots and callback handling use the
production implementations. Queues use passive dispatch without an automatic
queue lock across Send, allowing immediate completion and callback reentry.

The synthetic provider explicitly implements AddReference/ReleaseReference and
references the supplied WDM object. A wait lock serializes provider indications
and terminal retirement. Fault controls can fail OPEN/CLOSE, defer callback
retirement, emit remote disconnect and invoke the transport's removal callback.
This models a provider contract; it does not prove the real Bluetooth stack follows
that event order. Real PnP/sleep/removal needs the separate production-driver gate.

The lab ABI is compiled only into `OpenLdacKernelLab.sys`. It is absent from the
production `OpenLdacTransport` project. Driver load requires the explicit local
`HKLM\SOFTWARE\OpenLdacKernelLab\Enabled=1` marker. The runner additionally requires
`--isolated-guest`. These are accidental-use guards, not a security boundary against
an administrator. Test signatures/trust must exist only in the isolated guest.

## Build and run boundary

`driver/OpenLdacKernelLab.vcxproj` uses the same pinned WDK. The separate workflow
builds an **unsigned VM-only artifact** and records that nothing has been loaded.
The artifact retains the same build's PDB symbols and analysis log. The manifest
hashes both SYS and PDB; keep these exact files with any Verifier dump rather than
substituting symbols from a later rebuild. A packaged PDB is debugging readiness,
not evidence that the driver has run or passed Verifier.
The runner is a user-mode executable built from `lab_runner.c` with setup headers
and Advapi32. Its pending operations cancel and await actual completion before
releasing stack buffers. The host must never run the lab service or enable its
registry marker or test-signing state.

Before guest load: retain the clean VM snapshot, verify artifact SHA, create a
guest-only test certificate/trust and configure guest test-signing. Record every
guest change and the exact build revision. Baseline the runner before enabling
Verifier. Then select only `OpenLdacKernelLab.sys` for standard Verifier and retain
settings/runtime output, runner JSON, relevant dumps and post-run counters.

Baseline cases: repeated connect/write/cancel-read/close, failed OPEN, failed CLOSE
with delayed provider release, retained-root capacity, every remote disconnect
reason and target-stop/removal. Verifier/low-resource/forced-pending/delay scenarios
require their own executed evidence. Successful runner output alone does not show
Verifier was active. A crash is a failed test to diagnose, not an acceptable pass.

## Retention

Use the dedicated task VM folder. Keep one clean pre-Verifier snapshot, source,
hashes and validation results. User intends to reuse the VM with a suitable Windows
license later. Clean task-owned installer/ISO/cache after testing and detaching
media, retaining the VM and clean snapshot. Never delete unrelated user data or
publish guest credentials/unattended setup files.

Licenses: original lab glue and runner Apache-2.0; compiled Microsoft-derived
transport MS-PL. Both notices/licenses remain part of this source repository.
