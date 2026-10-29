#include "session.h"

#if ECSP_ENABLE_SLAVE

#include "ecsp/cmds.h"
#include "ecsp/config.h"

/* Spec 4.1.2: while it waits for a handshake the slave reports 0x01 at a fixed
 * 1 Hz until the handshake succeeds. The spec states neither a tolerance nor a
 * product override for this period, so it is deliberately not a config macro -
 * unlike the heartbeat, which 5.2.16 allows a product to retune (together with
 * the dropout threshold of 7.2.1). */
#define ECSP_HANDSHAKE_REPORT_MS 1000u

/* Both periods are compared through a signed difference (impl spec section 3),
 * which is only defined while they fit in an int32_t. */
#if ECSP_HEARTBEAT_MS > 0x7FFFFFFFu || ECSP_HANDSHAKE_REPORT_MS > 0x7FFFFFFFu
#error "reporting periods must be below 2^31 ms to stay wrap-safe"
#endif

static void clock_latch(ecsp_slave_clock_t *clock, uint32_t now_ms)
{
    if (clock->valid == 0u) {
        clock->at_ms = now_ms;
        clock->valid = 1u;
    }
}

static void clock_restart(ecsp_slave_clock_t *clock, uint32_t now_ms)
{
    clock->at_ms = now_ms;
    clock->valid = 1u;
}

/* 1 when a full `period_ms` has passed since `clock` was set. The subtraction
 * is signed so a now_ms wrap past 0xFFFFFFFF neither looks like a huge forward
 * jump nor a negative one (impl spec section 3). */
static int clock_elapsed(const ecsp_slave_clock_t *clock, uint32_t now_ms,
                         uint32_t period_ms)
{
    if (clock->valid == 0u) {
        return 0;
    }
    return ((int32_t)(now_ms - clock->at_ms) >= (int32_t)period_ms) ? 1 : 0;
}

void ecsp_session_start(ecsp_slave_t *s, uint32_t now_ms)
{
    clock_latch(&s->last_send, now_ms);
    clock_latch(&s->last_report, now_ms);
}

void ecsp_session_note_sent(ecsp_slave_t *s, uint32_t now_ms)
{
    clock_restart(&s->last_send, now_ms);
}

uint8_t ecsp_session_due_report(const ecsp_slave_t *s, uint32_t now_ms)
{
    if (s->ready != 0u) {
        return clock_elapsed(&s->last_send, now_ms, ECSP_HEARTBEAT_MS)
                   ? (uint8_t)ECSP_CMD_HEARTBEAT
                   : 0u;
    }
    return clock_elapsed(&s->last_report, now_ms, ECSP_HANDSHAKE_REPORT_MS)
               ? (uint8_t)ECSP_CMD_HANDSHAKE
               : 0u;
}

void ecsp_session_note_report(ecsp_slave_t *s, uint32_t now_ms)
{
    clock_restart(&s->last_report, now_ms);
}

#endif /* ECSP_ENABLE_SLAVE */
