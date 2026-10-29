#ifndef ECSP_SLAVE_SESSION_H
#define ECSP_SLAVE_SESSION_H

/*
 * L3 session timing (docs/specs/ecsp-impl-spec.md section 6, "会话与心跳";
 * spec 4.1.2, 5.2.16, 7.2.2). Private to src/ecsp/slave/: engine.c drives it,
 * nothing outside the slave layer includes this header.
 *
 * Two wrap-safe clocks, both latched by the first step because the library
 * never reads a clock:
 *
 *   - `last_send`   gates the ready-state heartbeat: any queued frame restarts
 *                   the window (spec 7.2.2);
 *   - `last_report` drives the waiting-state 0x01 report at a fixed 1 Hz,
 *                   which answers and rejections must not starve (4.1.2).
 */

#include <stdint.h>

#include "ecsp/slave.h"

/* Latch both clocks on the first step, so the first periodic frame is due a
 * full period later instead of at once. Idempotent. */
void ecsp_session_start(ecsp_slave_t *s, uint32_t now_ms);

/* A frame was queued into the transmit FIFO: restart the heartbeat window. The
 * library cannot observe the caller draining the FIFO, so queueing is the send
 * (spec 7.2.2). */
void ecsp_session_note_sent(ecsp_slave_t *s, uint32_t now_ms);

/* The periodic frame due now, or 0 when none is due:
 *   - waiting for a handshake -> 0x01 report once per fixed 1 Hz period;
 *   - ready -> 0x10 heartbeat once a full period passed since the last send.
 * At most one frame per call, so a stalled clock cannot release a burst
 * (spec 4.1.2, 5.2.16, 7.2.2). */
uint8_t ecsp_session_due_report(const ecsp_slave_t *s, uint32_t now_ms);

/* The frame returned by ecsp_session_due_report was queued: advance the
 * waiting-state report cadence. A frame that could not be queued (full FIFO)
 * must not call this, so the report is retried on the next step. */
void ecsp_session_note_report(ecsp_slave_t *s, uint32_t now_ms);

#endif /* ECSP_SLAVE_SESSION_H */
