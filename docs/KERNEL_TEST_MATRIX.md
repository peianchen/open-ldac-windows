# Kernel test gate and evidence collection

Current state, 2026-11-04: an isolated Windows VM passed the real-WDF synthetic
provider baseline, standard and forced-pending I/O/WDF oneboot profiles on
`4437db1`: 6,343 checks including 140 deferred OPEN cases per profile, plus
controlled unload. See the [latest executed report](validation/KERNEL_LAB_2026-11-03.md)
and [earlier 363-check report](validation/KERNEL_LAB_2026-11-01.md).
On `969a9ef`, a separate KMDF allocation-threshold sweep (0..128) observed 124 resource-failed calls across 18 sites, including all ten required transport sites. All final resource snapshots balanced, and the unpoisoned 6,343-check regression and 130/130 runtime load/unload passed. See the [low-resource report](validation/KERNEL_LOW_RESOURCE_2026-11-04.md). This is not random/DIF coverage or real BthPort acceptance.

This matrix remains incomplete: additional fault profiles, actual BthPort/PnP,
real hardware, production signing and Windows audio endpoint acceptance are pending.
Host CI and the host WDF shim do not establish kernel or BthPort acceptance.

## Prerequisites

Use an isolated Windows test installation with recoverable snapshot/backup and
kernel debugging/crash-dump access. Driver Verifier deliberately bugchecks on
violations. Do not enable it on the daily-use installation or select every driver.
A VM can test a simulated provider/WDF path but cannot prove real MediaTek/XM6
behavior without separately scoped access to the actual Bluetooth hardware.

Before loading, record exact SYS/INF/CAT hashes, code revision and original Audio
Sink service binding/restore procedure. A **test-signed package trusted only by the
isolated guest** is a prerequisite for kernel tests; production distribution
signing follows successful testing. Test certificates, test-signing and virtual
Secure Boot changes for the executed lab exist only inside its isolated guest;
the daily-use host was not changed. Signing a file does not make it safe to load.
A production package's signed catalog must cover the actual SYS/INF content;
the separate non-PnP lab service used an embedded guest test signature.

## Required runtime cases

| Case | Trigger / injection | Required observation |
| --- | --- | --- |
| Provider registration | OPEN success and failure with required non-null WDM ReferenceObject | Actual AddReference/ReleaseReference behavior; no guessed retirement |
| OPEN cancellation | Before format, between check/Send, while pending, completion inline/late | Exactly one completion; no forgotten successful channel; refs retained until retirement |
| Close failures | Low-resource allocation/reuse/format failures, rejected Send, asynchronous failed CLOSE | Nonblocking cleanup; max two local failed-submit attempts; no forced free; retained cap two |
| Data cancellation | Read/write pending, Cancel returns true/false, cleanup racing Send | Slot retires exactly once on actual completion or known failed submission; no late buffer access |
| Surprise removal | Pending OPEN/data/CLOSE, dispatch racing target stop | No new lower-stack BRB delivery after stop; no explicit CLOSE from removal callback; queued work cancels |
| Remote disconnect | HCI/L2CAP/radio-down/hardware-removal reasons | CloseNow TRUE, no duplicate CLOSE, callback retirement observed separately |
| Callback retirement | Release before/after CLOSE/data completion; callback itself still returning | Root remains until all callers/requests retire; final root/device/interface refs released |
| Missing retirement | Delayed or absent provider release | At most two roots retained; further connect reports busy; capture recovery and power behavior |
| PnP/power | Removal, disable/enable, radio off, sleep/resume, repeated cycles | No Verifier fault, hang, double completion or unbounded retained roots; shared devices remain usable |

Run baseline first, then targeted standard Verifier, then separate low-resource
and forced-pending profiles with deterministic concurrency schedules. Kernel
synchronization delay fuzzing is deprecated on Windows build19042 and later;
do not present its configuration as an executed modern-Windows test. Record the actual selected
rules and exercised fault sites; checking a Verifier option without hitting the
case is not evidence. Do not treat “no crash” or counter equality as complete
acceptance. Inspect crash dumps, WDF objects, pool/request/reference counts and
the lower-stack/provider event trace. Keep uncertainty explicit for unexercised
faults and unsupported provider behavior.

## Read-only capture tool

After a reviewed lab setup has loaded the test package, collect before/after each
case using the built probe and a new output folder:

```powershell
./tools/collect-kernel-evidence.ps1 -Probe ./build/wldac_probe.exe -OutputDirectory ./artifacts/kernel-case-001 -EnvironmentKind LabVM
```

Optional `-PackageDirectory` records hashes and Authenticode status.
Optional `-SignTool` additionally runs `verify /kp /v /c catalog sys`; catalog
verification and embedded signature checks are different. An unsigned embedded
SYS alone does not decide catalog-based loading, nor does a valid signature imply
hardware or security certification.

The collector only queries Verifier settings/runtime, the project's passive
list/status/diagnostics and driver load state. It never enables Verifier, resets
pairing, loads/rebinds a driver, runs DISCOVER, or changes boot/security settings.
Its report **always remains NOT_VERIFIED**: it captures raw evidence rather than
granting acceptance. Environment kind is caller-declared, not independently
verified. Store records privately and redact identifiers before public sharing.

Diagnostics ABI v1 exposes 14 per-device counters without MACs or kernel pointers.
Each counter read is atomic; the combined snapshot is not a coherent ordered
trace. SEND counts attempts; FAILURE includes failed submission/completion;
transfer retirement includes local format/send failure. Balanced Add/Release
counters alone cannot prove callback ordering or complete retirement.

## Follow-on gates

After kernel acceptance: production signing strategy, full AVDTP/LDAC streaming,
Windows PCM endpoint/service, then real HQ playback, sleep/coexistence and measured
power tests. The codec remains HQ on AC and battery; neither fault recovery nor
battery operation silently lowers quality. A transport-only driver is not a
working Windows LDAC audio endpoint.

Primary references:

- [Driver Verifier](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/driver-verifier)
- [Bluetooth removal contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/supporting-bluetooth-event-notifications)
- [Disconnect CloseNow](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_indication_parameters)
- [Target stop semantics](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetstop)
- [INDICATION_CODE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ne-bthddi-_indication_code)
