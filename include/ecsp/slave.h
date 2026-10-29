#ifndef ECSP_SLAVE_H
#define ECSP_SLAVE_H

/*
 * L3 slave engine (docs/specs/ecsp-impl-spec.md section 6, ADR-0005).
 *
 * Ticket #3 scope: the first "bytes in, bytes out" seam. Bytes fed into the
 * receive buffer are scanned into frames; a handshake request (0x01) whose
 * product AND board addresses both match this node is answered with a single
 * 0x00 response frame. Every other input produces no output (spec 2.1, 6.2).
 *
 * The full nine-step pipeline (rejection codes 0x02/0x04/0x10/..., session,
 * transaction cache, long-running commands) lands in later tickets, so unknown
 * and non-handshake commands are silently ignored for now.
 */

#include <stddef.h>
#include <stdint.h>

#include "ecsp/config.h"

typedef struct ecsp_slave ecsp_slave_t;

/* All buffers are caller-owned (ADR-0005): the library stores no pointer to
 * memory it does not receive here. */
typedef struct {
    uint8_t  product_addr;  /* this node's product address */
    uint8_t  board_addr;    /* this node's board address */
    uint8_t *rx_buf;        /* receive accumulation buffer */
    size_t   rx_cap;        /* rx_buf capacity in bytes */
    uint8_t *tx_buf;        /* outbound FIFO; drain via ecsp_slave_tx_drain */
    size_t   tx_cap;        /* tx_buf capacity in bytes */
} ecsp_slave_cfg_t;

/* Context; allocate on the stack or statically. Treat the fields as private. */
struct ecsp_slave {
    ecsp_slave_cfg_t cfg;
    size_t rx_len;   /* bytes currently held in rx_buf */
    size_t tx_len;   /* bytes currently queued in tx_buf */
};

/* Initialise *s from *cfg. Returns ECSP_OK, or ECSP_E_ARG if a buffer pointer
 * is NULL, rx_buf cannot hold the smallest legal frame, or tx_buf cannot hold
 * the smallest frame this engine emits (a handshake response). */
int ecsp_slave_init(ecsp_slave_t *s, const ecsp_slave_cfg_t *cfg);

/* Append received bytes (ADR-0005: the only call allowed from an interrupt or
 * receive context). Bytes that do not fit the receive buffer are dropped. */
void ecsp_slave_rx(ecsp_slave_t *s, const uint8_t *data, size_t len);

/* Advance the state machine: drain the receive buffer through the frame
 * scanner and answer matching handshake requests. `now_ms` is reserved for the
 * timing of later tickets and is currently unused. */
void ecsp_slave_step(ecsp_slave_t *s, uint32_t now_ms);

/* Number of outbound bytes queued, oldest first. */
size_t ecsp_slave_tx_pending(const ecsp_slave_t *s);

/* Discard the `n` oldest outbound bytes (the caller has already handed them to
 * the link). `n` is clamped to the pending count. */
void ecsp_slave_tx_drain(ecsp_slave_t *s, size_t n);

#endif /* ECSP_SLAVE_H */
