/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OL_LIFETIME_H
#define OL_LIFETIME_H
/* Freestanding. Callers serialize transitions; no heap, timers or OS types.
 * This exact implementation is compiled into the kernel and host fault tests. */
#define OL_CLOSE_ATTEMPTS 2u
#define OL_LIFE_MAX_IO 8u
typedef struct {
    unsigned owner, opening, opened, callbacks, callback_seen, retired;
    unsigned io, calls, close_wanted, closing, attempts;
    unsigned close_succeeded, close_failed, fault, disposing, disconnected;
} ol_lifetime;
typedef enum { OL_LEASE_CALL, OL_LEASE_IO, OL_LEASE_CALLBACK } ol_lease;
void ol_life_init(ol_lifetime *life);
int ol_life_take(ol_lifetime *life, ol_lease lease);
int ol_life_drop(ol_lifetime *life, ol_lease lease);
int ol_life_open_begin(ol_lifetime *life);
int ol_life_open_end(ol_lifetime *life, int succeeded);
void ol_life_detach(ol_lifetime *life);
void ol_life_close_wanted(ol_lifetime *life);
void ol_life_remote_disconnect(ol_lifetime *life);
int ol_life_close_begin(ol_lifetime *life);
int ol_life_close_not_sent(ol_lifetime *life);
int ol_life_close_end(ol_lifetime *life, int succeeded);
int ol_life_ready(const ol_lifetime *life);
int ol_life_claim_dispose(ol_lifetime *life);
int ol_life_can_io(const ol_lifetime *life);
#endif
