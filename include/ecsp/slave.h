#ifndef ECSP_SLAVE_H
#define ECSP_SLAVE_H

/*
 * L3 slave engine (docs/specs/ecsp-impl-spec.md section 6, ADR-0005).
 *
 * Bytes fed into the receive buffer are scanned into frames; the engine walks
 * each frame through the decision pipeline once, in the fixed ADR-0002 order,
 * and queues an outbound response frame into the caller-owned transmit buffer.
 *
 * Pipeline order (one implementation, in engine.c):
 *   1 frame CRC / tail failure  -> silent
 *   2 address mismatch          -> silent
 *   3 undefined command (including a Slave->Host event code) -> 0x04
 *   4 session not established and command != 0x01 -> 0x10
 *   5 payload length mismatch   -> 0x02
 *   6 transaction cache hit     -> replay (later ticket)
 *   7 long-running busy         -> 0x11 (later ticket)
 *   8 invalid parameter         -> 0x03
 *   9 handler                   -> the handler's return code
 *
 * Ticket #5 implements the passthrough commands 0x01-0x07, 0x0B and 0x12.
 * Common-command semantics live in the library; board commands (0x30-0xEF)
 * come from the caller's compile-time table (ADR-0001).
 */

#include <stddef.h>
#include <stdint.h>

#include "ecsp/cmds.h"
#include "ecsp/config.h"
#include "ecsp/frame.h"

typedef struct ecsp_slave ecsp_slave_t;

/* Optional parameter validator, run at step 8 before the handler. It returns
 * ECSP_OK when the payload parameters are acceptable, ECSP_E_LEN when the
 * variable-length payload is malformed (answered 0x02), or ECSP_E_VAL when a
 * value is illegal (answered 0x03). */
typedef int (*ecsp_cmd_validate_t)(const ecsp_frame_t *req);

/* Command handler. Called only after the pipeline's address, command, session,
 * length and parameter gates have accepted the frame, so the handler must not
 * repeat those checks.
 *
 * `resp` receives the response payload DATA (the response code is NOT part of
 * it); `cap` is its capacity. The return value is the response code (0..0xFF)
 * placed in the frame; the engine never overrides it. Data is sent only when
 * the return value is ECSP_RS_OK - an error response carries only the response
 * code.
 *
 * The engine reserves room for the command's largest possible answer before
 * calling the handler, so a full transmit FIFO does not cause the handler to
 * run twice. */
typedef int (*ecsp_slave_handler_t)(ecsp_slave_t *s, const ecsp_frame_t *req,
                                    uint8_t *resp, size_t cap, size_t *resp_len);

typedef enum {
    ECSP_CMD_NONE       = 0,
    ECSP_CMD_PREEMPTIVE = 1u << 0,  /* must be accepted while long-running (4.4) */
    ECSP_CMD_IDEMPOTENT = 1u << 1   /* idempotent action: carries no transaction seq */
} ecsp_cmd_flags_t;

/* Exact request payload length for a command that has not declared one; the
 * command's validator owns the length check (returning ECSP_E_LEN -> 0x02). */
#define ECSP_CMD_LEN_ANY 0xFFFFu

typedef struct {
    uint8_t  cmd;
    uint8_t  flags;    /* ecsp_cmd_flags_t bits */
    uint16_t req_len;  /* exact request payload length, or ECSP_CMD_LEN_ANY */
    uint16_t resp_max; /* largest response DATA length (response code excluded) */
    ecsp_cmd_validate_t validate; /* optional step-8 parameter gate */
    ecsp_slave_handler_t handler;
} ecsp_cmd_entry_t;

/* Caller-owned compile-time table of board commands (0x30-0xEF). Lookup is a
 * linear scan; the table does not need to be sorted. A command code that is
 * missing from the table is answered 0x04. */
typedef struct {
    const ecsp_cmd_entry_t *table;
    size_t table_len;
} ecsp_cmd_table_t;

/* Caller-owned identity storage. The library does not persist anything: a
 * successful 0x03/0x05 writes these bytes in place so the caller can commit
 * them to NVM at its own pace (spec 5.2.3, 5.2.5). */
typedef struct {
    ecsp_version_t hardware_version; /* "X.Y.Z.N" (5.2.3 / 5.2.4) */
    ecsp_serial_t  serial_number;    /* "SN" + 10 digits (5.2.5 / 5.2.6) */
} ecsp_slave_identity_t;

/* All buffers are caller-owned (ADR-0005): the library stores no pointer to
 * memory it does not receive here. */
typedef struct {
    uint8_t        product_addr; /* this node's product address */
    uint8_t        board_addr;   /* this node's board address */
    ecsp_version_t version;      /* software full version "X.Y.Z.N" (5.2.2) */
    ecsp_slave_identity_t *identity; /* hardware version + serial storage */
    uint8_t        fault_word;   /* current fault word reported by 0x12 (5.2.18) */
    ecsp_cmd_table_t cmds;       /* board command table (0x30-0xEF) */
    uint8_t       *rx_buf;       /* receive accumulation buffer */
    size_t         rx_cap;       /* rx_buf capacity in bytes */
    uint8_t       *tx_buf;       /* outbound FIFO; drain via ecsp_slave_tx_drain */
    size_t         tx_cap;       /* tx_buf capacity in bytes */
} ecsp_slave_cfg_t;

/* Context; allocate on the stack or statically. Treat the fields as private. */
struct ecsp_slave {
    ecsp_slave_cfg_t cfg;
    size_t rx_len;       /* bytes currently held in rx_buf */
    size_t tx_len;       /* bytes currently queued in tx_buf */
    uint8_t ready;       /* 0 = waiting for handshake, 1 = ready (spec 4.1.2) */
    uint8_t fault_word;  /* copy of cfg.fault_word; runtime fault bits land here */
    uint8_t low_power_mode; /* last accepted 0x0B mode (5.2.11) */
    uint8_t pending_ready;  /* apply "ready" after the handshake answer is queued */
    uint8_t pending_reset;  /* apply the reset after the 0x07 answer is queued */
    uint8_t reset_requested; /* 0x07 answered; product must reset after the drain */
};

/* Initialise *s from *cfg. Returns ECSP_OK, or ECSP_E_ARG if a buffer/identity
 * pointer is NULL, rx_buf cannot hold the smallest legal frame, tx_buf cannot
 * hold the largest answer declared by the common commands or the board
 * command table, or the command table pointer/length disagree. */
int ecsp_slave_init(ecsp_slave_t *s, const ecsp_slave_cfg_t *cfg);

/* Append received bytes (ADR-0005: the only call allowed from an interrupt or
 * receive context). Bytes that do not fit the receive buffer are dropped. */
void ecsp_slave_rx(ecsp_slave_t *s, const uint8_t *data, size_t len);

/* Advance the state machine: drain the receive buffer through the frame
 * scanner and run every complete frame through the decision pipeline. `now_ms`
 * is reserved for the session-timing ticket and is currently unused. */
void ecsp_slave_step(ecsp_slave_t *s, uint32_t now_ms);

/* Last low-power mode accepted by 0x0B (ECSP_LOW_POWER_EXIT /
 * ECSP_LOW_POWER_ENTER, spec 5.2.11). The library only records the request;
 * the product applies it. Returns EXIT for a NULL context. */
uint8_t ecsp_slave_low_power_mode(const ecsp_slave_t *s);

/* 1 once a 0x07 answer has been queued (the session is back to waiting for a
 * handshake). The product must drain the transmit FIFO and then reset the
 * MCU; the library cannot perform the hardware reset itself. Stays set until
 * the context is re-initialised by that reset. Returns 0 for an unset context
 * or before a reset request. */
int ecsp_slave_reset_requested(const ecsp_slave_t *s);

/* Number of outbound bytes queued, oldest first. */
size_t ecsp_slave_tx_pending(const ecsp_slave_t *s);

/* Discard the `n` oldest outbound bytes (the caller has already handed them to
 * the link). `n` is clamped to the pending count. */
void ecsp_slave_tx_drain(ecsp_slave_t *s, size_t n);

#endif /* ECSP_SLAVE_H */
