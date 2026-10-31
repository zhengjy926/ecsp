#ifndef ECSP_CONFIG_H
#define ECSP_CONFIG_H

/* Compile-time configuration and defaults (docs/specs/ecsp-impl-spec.md
 * section 3). Every macro may be overridden with -D. */

#include <stddef.h>
#include <stdint.h>

#ifndef ECSP_MAX_FRAME
#define ECSP_MAX_FRAME 128u
#endif

#ifndef ECSP_ENABLE_SLAVE
#define ECSP_ENABLE_SLAVE 1
#endif

#ifndef ECSP_ENABLE_HOST
#define ECSP_ENABLE_HOST 1
#endif

/* Transaction-sequence cache entries (spec 4.2). Each entry holds one complete
 * answer frame, so it costs about ECSP_MAX_FRAME + 4 bytes of context RAM;
 * 0 turns deduplication off (not recommended: a retransmitted non-idempotent
 * action would then execute twice). */
#ifndef ECSP_SLAVE_TXN_CACHE
#define ECSP_SLAVE_TXN_CACHE 4
#endif

#ifndef ECSP_HOST_SLAVES
#define ECSP_HOST_SLAVES 8
#endif

/* Heartbeat period in milliseconds (spec 5.2.16, 7.2.2). It is not a tick
 * interval: a ready slave sends at most one heartbeat per ecsp_slave_step and
 * only once a full period has passed since it last sent anything. A product
 * that overrides it must adjust the host's dropout threshold (three periods,
 * spec 7.2.1) to match. The waiting-state 0x01 report is fixed at 1 Hz by
 * spec 4.1.2 and has no such knob. */
#ifndef ECSP_HEARTBEAT_MS
#define ECSP_HEARTBEAT_MS 1000u
#endif

/* Host answer deadlines (spec 7.1, table 7-4: a product may only loosen
 * these, and the host times out at the declared value). Table 7-1 has no
 * row for the special-session class, so a handshake answer waits the
 * action-class ceiling. Get-version is a query. */
#ifndef ECSP_RESP_HANDSHAKE_MS
#define ECSP_RESP_HANDSHAKE_MS 100u
#endif

#ifndef ECSP_RESP_QUERY_MS
#define ECSP_RESP_QUERY_MS 50u
#endif

/* Set-class and action-class first answers (spec 7.1). Online upgrade's
 * first answer is the longer ceiling in the same table. Setting the
 * hardware version (0x03) or the serial number (0x05) is a non-volatile
 * write and uses ECSP_RESP_IDENTITY_MS instead. */
#ifndef ECSP_RESP_ACTION_MS
#define ECSP_RESP_ACTION_MS 100u
#endif

/* First answer for a non-volatile identity write (spec 7.1). A sector erase
 * may answer at about 2 s; only a full 3 s of silence is a timeout. A
 * product may only loosen this. */
#ifndef ECSP_RESP_IDENTITY_MS
#define ECSP_RESP_IDENTITY_MS 3000u
#endif

#ifndef ECSP_RESP_UPGRADE_MS
#define ECSP_RESP_UPGRADE_MS 500u
#endif

/* Power-up until business commands may be sent (spec 4.1.1, table 7-2).
 * A product may only loosen this. */
#ifndef ECSP_SESSION_SETUP_MS
#define ECSP_SESSION_SETUP_MS 5000u
#endif

/* Long-running accept field T_exec_max, milliseconds, uint16 little-endian
 * (spec 4.3, 5.2.8, 5.2.12). These are the recommended ceilings; a product
 * overrides them and declares the value in its product spec. Reaching the
 * deadline does not finish the command: a later get-result still answers
 * 0x00/0x11 until the product completes it or the host stops it. */
#ifndef ECSP_SELFTEST_EXEC_MAX_MS
#define ECSP_SELFTEST_EXEC_MAX_MS 2000u
#endif

#ifndef ECSP_UPGRADE_EXEC_MAX_MS
#define ECSP_UPGRADE_EXEC_MAX_MS 60000u
#endif

#if (ECSP_SELFTEST_EXEC_MAX_MS > 65535u) || (ECSP_UPGRADE_EXEC_MAX_MS > 65535u)
#error "T_exec_max must fit in a uint16 (spec 4.3)"
#endif

/* Local error codes (docs/specs/ecsp-impl-spec.md section 3): functions return
 * 0 (ECSP_OK) on success and a negative value on failure. These are library
 * failure reasons, never protocol response codes - those live only inside
 * frames (spec 3.6). Grown per ticket: only what this slice returns is defined
 * here; later tickets add codes as their functions need them. */
enum {
    ECSP_OK     = 0,
    ECSP_E_ARG  = -1,  /* invalid argument or configuration */
    ECSP_E_LEN  = -2,  /* payload length does not match the command (-> 0x02) */
    ECSP_E_VAL  = -3,  /* payload length is right but the value is illegal (-> 0x03) */
    ECSP_E_CAP   = -4, /* a destination buffer is too small for the data */
    ECSP_E_STATE   = -5, /* session is not in normal communication, so a
                          * business command is refused locally and no frame
                          * is queued (spec 4.1.1, 9.2) */
    ECSP_E_TIMEOUT = -6, /* the allowed attempts got no answer (spec 6.2) */
    ECSP_E_OFFLINE = -7, /* the session dropped out; an in-flight transaction
                          * was cancelled (spec 7.2.1) */
    ECSP_E_LINK    = -8, /* the frame would not fit in the transmit FIFO
                          * (spec 6.2); the peer was not judged silent */
    ECSP_E_REENT   = -9, /* ecsp_host_step called again from its own stack
                          * (ADR-0005); the call changes nothing */
    ECSP_E_RESET   = -10, /* the in-flight transaction was cancelled because
                           * the slave is resetting (spec 4.4, 4.1.4). Not a
                           * success and not a failure of that command: the
                           * result event will not arrive, and the outcome is
                           * whatever a later query says */
    ECSP_E_LOW_POWER = -11 /* the slave is confirmed in low power; this
                            * business command is refused locally and no
                            * frame is queued (spec 5.2.11). Distinct from
                            * ECSP_E_STATE */
};

#endif /* ECSP_CONFIG_H */
