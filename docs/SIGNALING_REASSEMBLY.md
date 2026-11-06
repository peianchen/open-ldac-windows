# Bounded incoming signaling reassembly

This module handles AVDTP signaling packet framing during an outgoing playback
transaction. It does not implement media fragmentation, peer-command dispatch,
reconfiguration or a Windows audio endpoint, and is not Bluetooth qualification.

## Wire and ownership

SINGLE has a two-byte header. START has three bytes including the total packet
count and signal identifier. CONTINUE/END have one byte; the signal identifier
comes from START. The transaction label and message type must stay consistent.
START counts both itself and END. END must arrive exactly where the count says;
no interleaving START or SINGLE is accepted during an active assembly.

`wl_avdtp_reassembly` owns a fixed 1,024-byte normalized SINGLE buffer. At most
16 fragments are accepted; these are project limits, not claimed Bluetooth
maximums. More than these limits fails closed. No heap, timers or I/O exist in
this module. The caller supplies the actual L2CAP MTU constraint.

`push` yields MORE with NULL/0 outputs until complete. COMPLETE exposes owned
bytes valid only until the next push/reset; no input pointer is retained.
An error poisons the assembly until reset. A new exchange gets fresh state,
so late fragments cannot be joined to another transaction. Signal RFA bits
are normalized away; signal zero/unknown semantics remain in the session layer,
which matches the complete response tuple before semantic validation.

Header-only fragments are allowed under the fixed count, preserving empty
accept responses and compatibility with observed primary implementation logic.
This policy is not presented as a normative requirement. Duplicate/reordered
CONTINUE payloads with identical headers cannot be detected by this framing
format alone; L2CAP ordered delivery and semantic validation remain necessary.

## Bounded transaction waiting

The owner samples monotonic milliseconds before sending a command. Sending and
all receives share 5,000 ms; each receive gets the remaining budget. A complete
reply at or beyond the deadline is rejected. A clock reading earlier than the transaction start fails closed; all readings
are required to be monotonic by the adapter contract.
It also caps each exchange at 64 wire receives and 16 unmatched complete
responses, including when a fake/faulty clock does not advance. A timeout or
framing error invalidates the session epoch and closes media before signaling.

There is one in-progress assembly, not a queue of simultaneous incoming
transactions. Malformed unrelated fragments can terminate this bounded subset;
only complete unmatched responses are ignored. Peer commands remain unsupported.
Unsolicited streaming signaling still uses the existing fail-closed monitor.
All emitted commands fit the minimum signaling MTU, so outbound fragmentation
has not been introduced. No new radio service, kernel ABI, or install path exists.

## Quality and verification boundary

The change preserves the exact-rate LDAC configuration, HQ default, manual
bitrate, no battery downgrade, terminal sender drain and 40 ms media pacing
protection. Fixed storage and event-based waits avoid periodic polling or a
new global timer setting; this is design evidence, not measured power savings.

The [dated execution report](validation/SIGNALING_REASSEMBLY_2026-11-06.md)
separates fake-peer codec testing, actual Windows I/O, VM SCM and still-unmeasured
MediaTek/XM6 behavior. Reassembly must not be described as actual headset playback.

## Sources

The Bluetooth SIG [AVDTP TS p22](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2025/02/AVDTP.TS_.p22.pdf),
section 4.3.3.2 (pages 129–130), describes a capability response split into three
48-byte L2CAP packets and checks equality after reconstruction. Section 4.3.3.1
also checks START/CONTINUE/END message types and transaction consistency.
The [BlueZ primary implementation](https://kernel.googlesource.com/pub/scm/bluetooth/bluez.git/+/master/profiles/audio/avdtp.c)
was inspected for header sizes and empty-payload handling (observed blob
`30f80a0c2fd152a604309bb2fbd231ff2f53731b`). The project implementation is separate;
no BlueZ source body is vendored. The official test suite is not the core
specification, and this work does not claim full conformance.
