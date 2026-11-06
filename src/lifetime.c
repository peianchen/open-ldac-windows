/* SPDX-License-Identifier: Apache-2.0 */
#include "lifetime.h"
#define OL_COUNTER_LIMIT 0xffffu
static int invalid(ol_lifetime *life) { life->fault = 1; return 0; }
void ol_life_init(ol_lifetime *life) {
    ol_lifetime initial = {0};
    *life = initial;
    life->owner = 1;
}
int ol_life_take(ol_lifetime *life, ol_lease lease) {
    unsigned *counter;
    if (life->disposing || life->fault) return 0;
    switch (lease) {
    case OL_LEASE_CALL: counter = &life->calls; break;
    case OL_LEASE_IO:
        if (!ol_life_can_io(life) || life->io >= OL_LIFE_MAX_IO) return 0;
        counter = &life->io; break;
    case OL_LEASE_CALLBACK:
        if (life->retired) return invalid(life);
        life->callback_seen = 1;
        counter = &life->callbacks; break;
    default: return invalid(life);
    }
    if (*counter >= OL_COUNTER_LIMIT) return invalid(life);
    ++*counter;
    return 1;
}
int ol_life_drop(ol_lifetime *life, ol_lease lease) {
    unsigned *counter;
    if (life->disposing) return invalid(life);
    switch (lease) {
    case OL_LEASE_CALL: counter = &life->calls; break;
    case OL_LEASE_IO: counter = &life->io; break;
    case OL_LEASE_CALLBACK: counter = &life->callbacks; break;
    default: return invalid(life);
    }
    if (!*counter) return invalid(life);
    --*counter;
    if (lease == OL_LEASE_CALLBACK && !life->callbacks) life->retired = 1;
    return 1;
}
int ol_life_open_begin(ol_lifetime *life) {
    if (!life->owner || life->close_wanted || life->opening || life->opened || life->fault || life->disposing)
        return 0;
    life->opening = 1;
    return 1;
}
int ol_life_open_end(ol_lifetime *life, int succeeded) {
    if (!life->opening || life->disposing) return invalid(life);
    life->opening = 0;
    life->opened = succeeded ? 1u : 0u;
    return 1;
}
void ol_life_detach(ol_lifetime *life) { life->owner = 0; life->close_wanted = 1; }
void ol_life_close_wanted(ol_lifetime *life) { life->close_wanted = 1; }
void ol_life_remote_disconnect(ol_lifetime *life) {
    life->close_wanted = 1;
    life->disconnected = 1; /* CloseNow asks the provider to close, NOT retire callbacks. */
}
int ol_life_close_begin(ol_lifetime *life) {
    if (!life->close_wanted || !life->opened || life->opening || life->closing ||
        life->retired || life->disconnected || life->close_succeeded || life->close_failed ||
        life->attempts >= OL_CLOSE_ATTEMPTS || life->disposing || life->fault) return 0;
    life->closing = 1;
    ++life->attempts;
    return 1;
}
int ol_life_close_not_sent(ol_lifetime *life) {
    if (!life->closing || life->disposing) return invalid(life);
    life->closing = 0;
    return 1; /* No completion is promised; provider retirement is still required. */
}
int ol_life_close_end(ol_lifetime *life, int succeeded) {
    if (!life->closing || life->disposing) return invalid(life);
    life->closing = 0;
    if (succeeded) life->close_succeeded = 1;
    else life->close_failed = 1;
    return 1; /* Neither result substitutes for provider callback retirement. */
}
int ol_life_can_io(const ol_lifetime *life) {
    return life->owner && life->opened && !life->opening && !life->close_wanted &&
           !life->retired && !life->fault && !life->disposing;
}
int ol_life_ready(const ol_lifetime *life) {
    return !life->owner && !life->opening && !life->closing && !life->callbacks &&
           !life->io && !life->calls && !life->fault &&
           (!life->opened || life->retired);
}
int ol_life_claim_dispose(ol_lifetime *life) {
    if (life->disposing || !ol_life_ready(life)) return 0;
    life->disposing = 1;
    return 1;
}
