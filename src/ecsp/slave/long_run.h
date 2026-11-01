#ifndef ECSP_SLAVE_LONG_RUN_H
#define ECSP_SLAVE_LONG_RUN_H

/*
 * Long-running command axis (spec 4.3, 4.4, ADR-0007). Private to
 * src/ecsp/slave/: engine.c drives it. One command may be in flight
 * (0x08 or 0x0C). The product finishes it with ecsp_slave_long_run_complete;
 * the engine queues the result event from ecsp_long_run_flush.
 */

#include <stddef.h>
#include <stdint.h>

#include "ecsp/slave.h"

/* Zero the execution axis. Does not touch the fault word: init copies that
 * from the caller-owned configuration. */
void ecsp_long_run_init(ecsp_slave_t *s);

/* 1 while a long-running command is in flight. */
int ecsp_long_run_is_busy(const ecsp_slave_t *s);

/* Start or stop. On ECSP_RS_OK, resp holds T_exec_max (u16le) and *resp_len
 * is 2. A rejection leaves *resp_len at 0 so the answer carries no field. */
int ecsp_long_run_start(ecsp_slave_t *s, uint8_t cmd,
                        uint8_t *resp, size_t cap, size_t *resp_len);
int ecsp_long_run_stop(ecsp_slave_t *s, uint8_t cmd,
                       uint8_t *resp, size_t cap, size_t *resp_len);

/* Get-result (0x09 or 0x0D). ECSP_RS_OK writes one result byte. */
int ecsp_long_run_query(const ecsp_slave_t *s, uint8_t query_cmd,
                        uint8_t *resp, size_t cap, size_t *resp_len);

/* Spec 4.1.4: an accepted handshake stops in-flight work. No result event.
 * The safe-abort callback runs when one is registered. The handshake answer
 * stays 0x00 either way; a failed sequence does not latch the fault bit. */
void ecsp_long_run_on_handshake(ecsp_slave_t *s);

/* Spec 4.4. ECSP_RS_OK: idle, or the actuator is safe and the in-flight
 * command was dropped with no result event. ECSP_RS_FAILED: cannot abort;
 * the command keeps running and the fault bit is latched. */
int ecsp_long_run_preempt_for_reset(ecsp_slave_t *s);

/* Queue at most one pending fault report and one pending result event.
 * `emit` returns 1 when the frame was queued. A 0 leaves that frame pending
 * so the next step retries it; a later frame waits behind it. */
typedef int (*ecsp_long_run_emit_t)(ecsp_slave_t *s, uint8_t cmd,
                                    const uint8_t *payload, uint16_t len,
                                    uint32_t now_ms);

void ecsp_long_run_flush(ecsp_slave_t *s, uint32_t now_ms,
                         ecsp_long_run_emit_t emit);

void ecsp_long_run_drop_held(ecsp_slave_t *s);

#endif /* ECSP_SLAVE_LONG_RUN_H */
