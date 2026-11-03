/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_AVDTP_SESSION_H
#define WL_AVDTP_SESSION_H
#include "avdtp.h"

/* Bounded, single-packet initiator subset, not complete AVDTP/A2DP compliance.
 * No heap, OS, clock, worker or transport I/O. Serialize access on the owner.
 * The caller supplies unprotected PCM at exactly source_rate. This module
 * proposes MediaTransport + stereo LDAC only; it neither negotiates content
 * protection nor changes HQ/manual-bitrate encoder policy or resamples PCM. */
#define WL_AVDTP_SESSION_MAX_SEPS 62u
#define WL_AVDTP_SESSION_MAX_PACKET 1024u

typedef enum {
    WL_AVDTP_SESSION_UNINITIALIZED = 0,
    WL_AVDTP_SESSION_DISCOVER,
    WL_AVDTP_SESSION_CAPABILITIES,
    WL_AVDTP_SESSION_CONFIGURE,
    WL_AVDTP_SESSION_OPEN,
    WL_AVDTP_SESSION_WAIT_MEDIA,
    WL_AVDTP_SESSION_START,
    WL_AVDTP_SESSION_STREAMING,
    WL_AVDTP_SESSION_SUSPEND,
    WL_AVDTP_SESSION_SUSPENDED,
    WL_AVDTP_SESSION_ABORT,
    WL_AVDTP_SESSION_ABORTED,
    WL_AVDTP_SESSION_FAILED
} wl_avdtp_session_state;

typedef enum {
    WL_AVDTP_SESSION_OK = 0,
    WL_AVDTP_SESSION_INVALID_ARGUMENT = -1,
    WL_AVDTP_SESSION_WRONG_GENERATION = -2,
    WL_AVDTP_SESSION_UNMATCHED_RESPONSE = -3,
    WL_AVDTP_SESSION_MALFORMED = -4,
    WL_AVDTP_SESSION_UNSUPPORTED_FRAGMENT = -5,
    WL_AVDTP_SESSION_REMOTE_COMMAND = -6,
    WL_AVDTP_SESSION_REJECTED = -7,
    WL_AVDTP_SESSION_NO_ENDPOINT = -8,
    WL_AVDTP_SESSION_NO_LDAC = -9,
    WL_AVDTP_SESSION_CONVERSION_REQUIRED = -10,
    WL_AVDTP_SESSION_BUSY = -11,
    WL_AVDTP_SESSION_INVALID_STATE = -12,
    WL_AVDTP_SESSION_TRANSPORT_INVALID = -13,
    WL_AVDTP_SESSION_TOKEN_MISMATCH = -14,
    WL_AVDTP_SESSION_TIMEOUT = -15,
    WL_AVDTP_SESSION_SEND_FAILED = -16,
    WL_AVDTP_SESSION_TRANSPORT_FAILED = -17,
    WL_AVDTP_SESSION_SEQUENCE_EXHAUSTED = -18
} wl_avdtp_session_result;

typedef struct {
    uint64_t generation;
    uint64_t serial;
    uint8_t transaction;
    uint8_t signal;
} wl_avdtp_session_token;

typedef struct {
    uint64_t generation, serial;
    wl_avdtp_session_state state;
    int last_error;
    unsigned source_rate;
    uint8_t local_seid, remote_seid;
    uint8_t candidates[WL_AVDTP_SESSION_MAX_SEPS];
    uint8_t candidate_count, candidate_index;
    uint8_t codec[8];
    uint8_t next_transaction, pending, transport_valid, configured;
    uint8_t conversion_required_seen;
    uint8_t last_reject_category, last_reject_error;
    wl_avdtp_session_token outstanding;
} wl_avdtp_session;

/* First initialize the storage with {0}. A restart requires a strictly larger
 * nonzero generation and the previous transport to be invalid/retired. The
 * transport owner must close the old L2CAP channel before reinitialization. */
int wl_avdtp_session_init(wl_avdtp_session *session, uint64_t generation,
                         uint8_t local_seid, unsigned source_rate);

/* Returns a command length, or zero when no command can be emitted. Capacity,
 * NULL argument and already-pending failures leave session/counters unchanged.
 * The packet buffer must remain alive until the transport completes the send. */
size_t wl_avdtp_session_next_command(wl_avdtp_session *session,
                                    uint8_t *out, size_t capacity,
                                    wl_avdtp_session_token *token);

/* Parse/consume immediately: no borrowed packet pointer is retained. Only a
 * matching response advances the session. Wrong generation/transaction/signal
 * or no outstanding transaction is rejected without state changes. Malformed
 * matching responses, peer commands and fragmentation invalidate transport.
 * A reject terminates this subset; it does not silently reconfigure/retry. */
int wl_avdtp_session_receive(wl_avdtp_session *session, uint64_t generation,
                            const uint8_t *packet, size_t size);

/* OPEN accept only reaches WAIT_MEDIA. START is emitted only after the owner
 * reports that the separate media transport is ready for this generation. */
int wl_avdtp_session_media_ready(wl_avdtp_session *session, uint64_t generation);
int wl_avdtp_session_request_suspend(wl_avdtp_session *session);
int wl_avdtp_session_request_resume(wl_avdtp_session *session);
/* BUSY while a command is outstanding. Before configuration, abort is local
 * terminal cleanup; after configuration an ABORT command must be accepted. */
int wl_avdtp_session_request_abort(wl_avdtp_session *session);

/* Tokens protect stale local completion/timer callbacks. A matching timeout or
 * send failure invalidates the entire epoch, not merely the 4-bit label. The
 * caller owns deadlines; this module never polls or invokes a timer. */
int wl_avdtp_session_timeout(wl_avdtp_session *session,
                            const wl_avdtp_session_token *token);
int wl_avdtp_session_send_failed(wl_avdtp_session *session,
                                const wl_avdtp_session_token *token);
int wl_avdtp_session_transport_failed(wl_avdtp_session *session,
                                     uint64_t generation);

/* Wire limitation: after a 4-bit label wraps, an arbitrary ancient duplicate
 * with identical label/signal cannot be distinguished on the same wire epoch.
 * Normal stale tuples and local tokens are rejected; timeout requires replacing
 * transport so a timed-out tuple can never be reused on that channel. */
#endif
