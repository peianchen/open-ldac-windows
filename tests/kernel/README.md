# Isolated kernel lab — synthetic provider

This separate KMDF test service compiles the production MS-PL
client/connection/control/queue and original lifecycle implementation against
**real WDF**, with an original synthetic provider. It has no dependency on a
physical radio and creates no Windows audio endpoint. It does not establish
BthPort, Audio Sink PDO/PnP, real MediaTek/XM6 or electrical-power correctness.

Status: source/build plus executed isolated-VM baseline and targeted standard
Verifier cases on `36bc35f`, including unload. See the [dated record](../../docs/validation/KERNEL_LAB_2026-11-01.md)
for hashes, the initial crash/fix, 363 checks, runtime evidence and remaining gaps.
Never interpret a build or this limited lab run as real Bluetooth acceptance.

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

## Deferred OPEN extension (ABI 2, 2026-11-03)

The new runner adds 140 controlled cases: seven schedules, callback retirement
before/after completion, repeated ten times. These are test definitions; a build
or earlier ABI-1 pass does not establish their runtime results.

| Schedule | Lower provider ownership | Expected completion |
|---|---|---|
| Cancel queued OPEN | WDF manual queue, cancel callback owns retirement | Cancelled once |
| Complete queued OPEN | Retrieved once from manual queue | Success once |
| Cancel, then late success | Driver retains OPEN until control command | Cancelled; CLOSE if provider not retired |
| File cleanup, then late success | Driver retains OPEN; separate thread closes file | Same, with cleanup observed before completion |
| Cancel, then failure | Driver retains OPEN | Failure once; no CLOSE |
| File cleanup, then failure | Driver retains OPEN | Failure once; no CLOSE |
| Cancel versus retrieve race | Worker cancels while control retrieves manual queue | Either winner, one completion |

Every case observes the pending state, keeps its input/OVERLAPPED alive through
an IOCP completion packet (also after file close), checks no duplicate packet,
and compares per-case OPEN/CLOSE, callback and root counters. A held cancelled
OPEN must remain incomplete with its root alive until the completion command.
Provider retirement before completion suppresses further CLOSE submission.
Cancel-queue callbacks take no passive wait lock. Failures terminate the runner
process immediately so no worker can write an expired stack frame; a failed
held request may remain quarantined in the guest and is not an unload pass.

File/root and user-handle retirement are different observations. A bounded
user-mode wait checks the synthetic status `active_files` before reusing the
two-file capacity; a successful `CloseHandle` or zero roots alone does not prove
that WDF has delivered `EvtFileClose`. Wait iterations are reported explicitly.
`--isolated-guest --inspect-only` queries root/I/O/callback state without running
the fault matrix; it cannot establish a baseline pass.

WDF references: [manual queue forwarding](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestforwardtoioqueue),
[cancellation ownership and IRQL](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfio/nc-wdfio-evt_wdf_io_queue_io_canceled_on_queue),
[retrieval transfers ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfio/nf-wdfio-wdfioqueueretrievenextrequest).

Run standard and force-pending Verifier as distinct selected-driver profiles;
retain actual runtime evidence for each. Resource-failure testing additionally
needs an expected-failure-aware runner and evidence that failures were injected.
Kernel synchronization delay fuzzing is deprecated on Windows build 19042 and
later; deterministic schedules above do not claim to replace all concurrency,
IRQL, physical PnP or BthPort testing.

## Retention

Use the dedicated task VM folder. Keep one clean pre-Verifier snapshot, source,
hashes and validation results. User intends to reuse the VM with a suitable Windows
license later. Clean task-owned installer/ISO/cache after testing and detaching
media, retaining the VM and clean snapshot. Never delete unrelated user data or
publish guest credentials/unattended setup files.

Licenses: original lab glue and runner Apache-2.0; compiled Microsoft-derived
transport MS-PL. Both notices/licenses remain part of this source repository.
