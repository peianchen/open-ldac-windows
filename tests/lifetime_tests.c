/* SPDX-License-Identifier: Apache-2.0 */
#include "lifetime.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, schedules;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static ol_lifetime connected(void) {
    ol_lifetime life;
    ol_life_init(&life);
    CHECK(ol_life_open_begin(&life));
    CHECK(ol_life_take(&life, OL_LEASE_CALLBACK));
    CHECK(ol_life_open_end(&life, 1));
    CHECK(ol_life_can_io(&life));
    return life;
}

static void failures(void) {
    ol_lifetime life = connected();
    unsigned i;
    ol_life_detach(&life);
    for (i = 0; i < OL_CLOSE_ATTEMPTS; ++i) {
        CHECK(ol_life_close_begin(&life));
        CHECK(ol_life_close_not_sent(&life)); /* Format/reuse/send failed. */
    }
    for (i = 0; i < 10000; ++i) CHECK(!ol_life_close_begin(&life));
    CHECK(life.attempts == 2 && !life.closing);
    CHECK(!ol_life_can_io(&life));
    CHECK(!ol_life_claim_dispose(&life)); /* Provider still owns callback. */
    CHECK(ol_life_drop(&life, OL_LEASE_CALLBACK));
    CHECK(ol_life_claim_dispose(&life));
    CHECK(!ol_life_claim_dispose(&life));

    for (i = 0; i < 2; ++i) {
        life = connected();
        CHECK(ol_life_take(&life, OL_LEASE_IO));
        ol_life_detach(&life);
        CHECK(ol_life_close_begin(&life));
        CHECK(ol_life_close_end(&life, (int)i));
        CHECK(!ol_life_claim_dispose(&life)); /* Neither close result retires callbacks. */
        CHECK(ol_life_drop(&life, OL_LEASE_CALLBACK));
        CHECK(!ol_life_claim_dispose(&life)); /* Cancel(false/true) is NOT completion. */
        CHECK(ol_life_drop(&life, OL_LEASE_IO));
        CHECK(ol_life_claim_dispose(&life));
    }
    life = connected();
    for (i = 0; i < OL_LIFE_MAX_IO; ++i) CHECK(ol_life_take(&life, OL_LEASE_IO));
    CHECK(!ol_life_take(&life, OL_LEASE_IO));
    CHECK(life.io == OL_LIFE_MAX_IO);
    ol_life_detach(&life);
    CHECK(ol_life_drop(&life, OL_LEASE_CALLBACK));
    for (i = 0; i < OL_LIFE_MAX_IO; ++i) {
        CHECK(!ol_life_claim_dispose(&life));
        CHECK(ol_life_drop(&life, OL_LEASE_IO));
    }
    CHECK(ol_life_claim_dispose(&life));
}

static void opening(void) {
    unsigned success, callback, cleanup_first;
    for (success = 0; success < 2; ++success)
    for (callback = 0; callback < 2; ++callback)
    for (cleanup_first = 0; cleanup_first < 2; ++cleanup_first) {
        ol_lifetime life;
        ol_life_init(&life);
        CHECK(ol_life_take(&life, OL_LEASE_CALL)); /* Send dispatcher can still be returning. */
        CHECK(ol_life_open_begin(&life));
        if (callback) CHECK(ol_life_take(&life, OL_LEASE_CALLBACK));
        if (cleanup_first) ol_life_detach(&life);
        CHECK(!ol_life_claim_dispose(&life));
        CHECK(ol_life_open_end(&life, (int)success));
        if (!cleanup_first) ol_life_detach(&life);
        if (success) {
            CHECK(ol_life_close_begin(&life));
            CHECK(ol_life_close_end(&life, 1));
        }
        if (callback) CHECK(ol_life_drop(&life, OL_LEASE_CALLBACK));
        CHECK(!ol_life_claim_dispose(&life)); /* Inline completion cannot free dispatcher state. */
        CHECK(ol_life_drop(&life, OL_LEASE_CALL));
        if (success && !callback) CHECK(!ol_life_claim_dispose(&life));
        else CHECK(ol_life_claim_dispose(&life));
    }
    {
        ol_lifetime life;
        ol_life_init(&life); /* Pre-open allocation/attach/format failure. */
        ol_life_detach(&life);
        CHECK(!ol_life_open_begin(&life));
        CHECK(ol_life_claim_dispose(&life));
    }
}

/* Exhaust every enabled ordering of cleanup, two I/O completions, provider
 * retirement, close completion and return from an inline Send dispatcher.
 * These are serialized transitions of the ACTUAL shared engine, not WDF calls. */
enum { CLEAN = 1, RELEASE = 2, IO_A = 4, IO_B = 8, RETURN = 16, CLOSE = 32 };
static void explore(ol_lifetime life, unsigned done, unsigned mode) {
    unsigned event;
    int disposed = ol_life_claim_dispose(&life);
    CHECK(!life.fault && life.attempts <= OL_CLOSE_ATTEMPTS);
    if (disposed) {
        CHECK((done & 31u) == 31u);
        CHECK(!life.io && !life.calls && !life.callbacks && !life.closing && !life.owner);
        CHECK(!ol_life_claim_dispose(&life));
        ++schedules;
        return;
    }
    for (event = 1; event <= CLOSE; event <<= 1) {
        ol_lifetime next = life;
        if (done & event) continue;
        if (event == CLEAN) {
            ol_life_detach(&next);
            if (mode == 0) {
                while (ol_life_close_begin(&next)) CHECK(ol_life_close_not_sent(&next));
            } else if (mode == 1) {
                if (ol_life_close_begin(&next)) {
                    CHECK(ol_life_close_not_sent(&next));
                    CHECK(ol_life_close_begin(&next));
                }
            } else (void)ol_life_close_begin(&next);
            CHECK(!ol_life_can_io(&next));
        } else if (event == RELEASE) CHECK(ol_life_drop(&next, OL_LEASE_CALLBACK));
        else if (event == IO_A || event == IO_B) CHECK(ol_life_drop(&next, OL_LEASE_IO));
        else if (event == RETURN) CHECK(ol_life_drop(&next, OL_LEASE_CALL));
        else {
            if (!next.closing) continue;
            CHECK(ol_life_close_end(&next, mode != 2));
        }
        explore(next, done | event, mode);
    }
}

/* Host model of the adapter's request-reference and two cancel-check protocol.
 * Cancel may fail before Send or after completion; it never frees the I/O lease.
 * WDF scheduling/IRQL/provider behavior still require kernel verification. */
static void cancellation(void) {
    unsigned when, inline_completion;
    for (when = 0; when < 4; ++when)
    for (inline_completion = 0; inline_completion < 2; ++inline_completion)
    {
        ol_lifetime life = connected();
        unsigned wanted = 0, completed = 0, cancel_calls = 0;
        CHECK(ol_life_take(&life, OL_LEASE_CALL));
        CHECK(ol_life_take(&life, OL_LEASE_IO));
        if (when == 0) { wanted = 1; ol_life_detach(&life); }
        if (!wanted) { /* Pre-send cancellation check passed. */
            if (when == 1) { wanted = 1; ol_life_detach(&life); ++cancel_calls; }
            /* Send accepts: completion can happen BEFORE Send returns. */
            if (inline_completion) { CHECK(ol_life_drop(&life, OL_LEASE_IO)); completed = 1; }
            if (when == 2) { wanted = 1; ol_life_detach(&life); ++cancel_calls; }
            if (wanted) ++cancel_calls; /* Post-send check closes the gap. */
            if (!completed) {
                CHECK(life.io == 1); /* Even cancel=true does not retire buffers. */
                CHECK(ol_life_drop(&life, OL_LEASE_IO)); completed = 1;
            }
        } else { CHECK(ol_life_drop(&life, OL_LEASE_IO)); completed = 1; }
        if (when >= 3) ol_life_detach(&life);
        CHECK(completed && life.io == 0);
        CHECK(when != 1 || cancel_calls >= 2);
        CHECK(ol_life_drop(&life, OL_LEASE_CALLBACK));
        CHECK(!ol_life_claim_dispose(&life));
        CHECK(ol_life_drop(&life, OL_LEASE_CALL));
        CHECK(ol_life_claim_dispose(&life));
    }
}

static void bad_contracts(void) {
    ol_lifetime life;
    ol_life_init(&life);
    CHECK(!ol_life_drop(&life, OL_LEASE_IO));
    ol_life_detach(&life);
    CHECK(life.fault && !ol_life_claim_dispose(&life)); /* Fail closed on bookkeeping faults. */
    life = connected();
    CHECK(ol_life_drop(&life, OL_LEASE_CALLBACK));
    CHECK(!ol_life_take(&life, OL_LEASE_CALLBACK)); /* Provider violated terminal release. */
    ol_life_detach(&life);
    CHECK(life.fault && !ol_life_claim_dispose(&life));
    life = connected();
    ol_life_detach(&life);
    CHECK(ol_life_close_begin(&life));
    CHECK(ol_life_close_end(&life, 1));
    CHECK(!ol_life_claim_dispose(&life)); /* Missing release: bounded retained root, not forced free. */
}

int main(void) {
    unsigned mode;
    failures(); opening(); cancellation(); bad_contracts();
    for (mode = 0; mode < 4; ++mode) {
        ol_lifetime life = connected();
        CHECK(ol_life_take(&life, OL_LEASE_IO));
        CHECK(ol_life_take(&life, OL_LEASE_IO));
        CHECK(ol_life_take(&life, OL_LEASE_CALL));
        explore(life, 0, mode);
    }
    CHECK(schedules >= 1000);
    printf("PASS: %u lifecycle checks, %u exhaustive schedules; host fault model only, no WDF/hardware proof.\n",
           checks, schedules);
    return 0;
}
