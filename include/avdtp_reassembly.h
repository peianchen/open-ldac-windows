/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_AVDTP_REASSEMBLY_H
#define WL_AVDTP_REASSEMBLY_H
#include <stddef.h>
#include <stdint.h>

/* Bounded receive-side signaling subset, not media fragmentation. The owner
 * enforces each physical L2CAP MTU and a transaction deadline/read budget.
 * This module has no clock, heap, I/O or session semantics. */
#define WL_AVDTP_REASSEMBLY_MAX_PACKET 1024u
#define WL_AVDTP_REASSEMBLY_MAX_FRAGMENTS 16u
enum {
    WL_AVDTP_REASSEMBLY_COMPLETE = 0,
    WL_AVDTP_REASSEMBLY_MORE = 1,
    WL_AVDTP_REASSEMBLY_INVALID_ARGUMENT = -1,
    WL_AVDTP_REASSEMBLY_MALFORMED = -2,
    WL_AVDTP_REASSEMBLY_LIMIT = -3,
    WL_AVDTP_REASSEMBLY_SEQUENCE = -4,
    WL_AVDTP_REASSEMBLY_POISONED = -5
};
typedef struct {
    uint8_t packet[WL_AVDTP_REASSEMBLY_MAX_PACKET];
    size_t size;
    uint8_t tuple, remaining, active, poisoned;
} wl_avdtp_reassembly;

/* Initialize with {0} or reset before use; reset after a completed message
 * is optional. On timeout/disconnect/new epoch the owner discards/resets state.
 * After any error, reset is mandatory; never splice a later fragment into it. */
void wl_avdtp_reassembly_reset(wl_avdtp_reassembly *state);
/* COMPLETE exposes state-owned bytes normalized to a SINGLE packet; signal
 * RFA bits are cleared. Output is valid until next push/reset. MORE/error
 * always return NULL/0 outputs. Partial data is never exposed. No borrowed
 * packet pointer is retained. Header-only fragments are accepted within the
 * bounded fragment count; content validity belongs to the session parser.
 * No transaction/signal filtering or peer-command dispatch occurs here. */
int wl_avdtp_reassembly_push(wl_avdtp_reassembly *state,
                            const uint8_t *packet, size_t size,
                            const uint8_t **message, size_t *message_size);
#endif
