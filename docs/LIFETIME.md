# Nonblocking close and cancellation

Status: implemented source, host fault models, and an executed real-WDF synthetic
provider baseline/standard Verifier run. The [2026-11-01 record](validation/KERNEL_LAB_2026-11-01.md)
describes the cases and limits. Real BthPort callback behavior, full IRQL/race/fault
coverage and actual device removal are not established.

## Ownership

A connection is a driver-parented WDF root, not a child of a file or device. It
holds WDF device/target and WDM device references until destruction. Root capacity
is two per device, including detached roots; file capacity is separately two.
Allocation failures before open delete the partially initialized root. Close
request and BRB memory are allocated before registering any Bluetooth callback.

The freestanding engine in `src/lifetime.c` is compiled into both the driver and
host tests. The adapter serializes transitions with a spin lock. Lifetime includes:

| Lease | Acquisition | Retirement |
| --- | --- | --- |
| File owner | Connection creation | File cleanup detaches pointer under file lock |
| Open pending | Before submitting OPEN | Actual completion or failed submission |
| Provider | IndicationAddReference | IndicationReleaseReference |
| Data I/O (max 8) | Before format/send; slot published under lock | Actual completion or failed format/submission |
| Close pending | Before reserved CLOSE request is reused | Actual completion or failed submission |
| Active call | Borrow pointer or enter completion/callback/dispatcher | Last access in that call |

No explicit deletion is allowed until the owner, open, close, callback, data and
active-call leases are gone. A successfully opened channel additionally requires
observed provider retirement. Closing a file never waits on an event. It prevents
new work, clears the file pointer, requests cancellation and starts asynchronous
close. Delayed open completion closes a channel opened after cleanup. A caller
still returning from Send protects state against inline completion.

Close reuse/format/send failure permits at most two immediate attempts using
reserved storage. Asynchronous close failure is terminal for that close request.
Neither success nor failure of CLOSE proves callback retirement. If retirement
never arrives, retain the bounded root; report live and retained counts through
status ABI v2. Repeated opens eventually return busy, rather than accumulating
contexts or forcing a premature free. This is containment, not guaranteed recovery
from a nonresponsive provider. It may retain device/power resources as well as
memory. There is no timer, forced-free timeout or background retry worker.

## Cancellation races

Cleanup snapshots and references pending OPEN and data requests under the root
lock, marks cancellation wanted, then cancels outside the lock. Dispatch retains
a request reference across formatting, Send, inline completion and a post-send
cancellation check. Checking before and after Send covers cleanup occurring while
the request is not yet sent. Cancel's return value never drops an I/O lease or
frees a BRB/OVERLAPPED buffer. Completion removes its slot exactly once. Remote
disconnect marks closing before the cancellation snapshot so new I/O cannot slip
between those operations. Remote disconnect responds with CloseNow TRUE and
marks a distinct disconnected state so later file cleanup does not submit duplicate
CLOSE. Hardware removal atomically marks the device removed and stops its local
I/O target without waiting. Pre/post-Send removal checks cancel work queued during
the race; the stopped target prevents delivery to the lower driver. The PnP surprise
removal callback also marks/stops it once, without sending BRBs. Queue stop marks cancellation and asks the target to
cancel; completion remains responsible for retiring the request.

OPEN uses only the bound XM6 Audio Sink address and assigned AVDTP PSM 0x0019.
Removing synchronous per-open SDP eliminates that uncancelable work from this
diagnostic path. This does not negotiate or validate LDAC capabilities.

## Validation and remaining gates

Host tests cover failed close submissions/retry cap, asynchronous close errors,
late release and I/O completion, cancelled/failed open, inline completion, missing
provider retirement, I/O capacity and invalid bookkeeping. They exhaust 1,020
enabled event schedules across four close outcomes. A small host cancellation
model also checks the two-check ordering; it is not the WDF adapter itself.
A second host target compiles the actual client/connection/control/queue function
bodies against a modeled WDF boundary. It covers allocation/reuse/format/Send
failures, live file destruction, retained-root and eight-I/O caps, inline/late
completion, cancel-before-Send, remote CloseNow and target-stop races. The shim
models API contracts, not IRQL, real concurrency or Bluetooth behavior. ASan/UBSan
CI applies to both host targets, separately from the encoder.

The isolated VM now passed 363 synthetic-provider checks both at baseline and
under targeted standard Verifier, followed by normal unload. Additional fault
profiles and actual BthPort/PnP cases in the [required matrix](KERNEL_TEST_MATRIX.md)
remain pending. The read-only collector cannot turn host or metadata evidence
into a kernel acceptance result.

Before a laptop trial, a separate kernel test environment must establish:

1. Actual AddReference/ReleaseReference behavior with the required non-null
   `ReferenceObject`, including failed/cancelled OPEN and successful/failed CLOSE.
   Do not assume the host event model establishes the Bluetooth provider contract.
2. WDF/Driver Verifier under low resources, failed format/send, delayed callbacks,
   cancellation at each send boundary, removal, radio-off and sleep/resume. Check
   for hangs, double completion, late access and retained-root recovery.
3. Valid signing and restoration of the exact original XM6 service binding. This
   transport has no Windows audio endpoint; rebinding alone removes normal XM6
   A2DP playback until restored.
4. Only then, real MediaTek/XM6 protocol, playback and power/coexistence tests.
   HQ remains the default on AC and battery; a close fault must never silently
   select a lower codec quality.

Primary contracts:

- [INDICATION_CODE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ne-bthddi-_indication_code): ReleaseReference says the callback will no longer be called.
- [OPEN channel](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_brb_l2ca_open_channel): ReferenceObject is an object-manager reference, not a WDF context handle. WDK headers require it when requesting a callback; it remains non-null here.
- [Cancel sent request](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestcancelsentrequest): cancellation races require request references and actual completion.
- [WDF object lifecycle](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/framework-object-life-cycle): cleanup/deletion and final reference release differ.

- [CloseNow](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_indication_parameters): remote disconnect can ask the provider to close without another CLOSE BRB.
- [Target stop](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfiotarget/nf-wdfiotarget-wdfiotargetstop): stopped targets can accept queued requests, so post-Send cancellation remains necessary.
