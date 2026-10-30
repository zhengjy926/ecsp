#ifndef ECSP_HOST_H
#define ECSP_HOST_H

/*
 * L4 host link (docs/specs/ecsp-impl-spec.md section 7, ADR-0003, ADR-0005).
 *
 * Ticket #9: after the first step the host probes each configured slave with
 * a handshake request (0x01), retries on silence, and on a 0x00 answer reads
 * the software version (0x02) and compares it byte for byte with the version
 * baked into the configuration. A match enters normal communication and a
 * business command may be submitted; a mismatch stays at version-mismatch and
 * a submit is refused locally. Three unanswered probes return the session to
 * disconnected and report the failure. The caller pushes time and drains the
 * transmit FIFO; the library never reads a clock.
 */

#include <stddef.h>
#include <stdint.h>

#include "ecsp/cmds.h"
#include "ecsp/config.h"

typedef struct ecsp_host ecsp_host_t;

/* Session axis for one slave (spec table 4-1). The transaction axis is a
 * later ticket; offline is part of the table and is not entered here. */
typedef enum {
    ECSP_SESS_DISCONNECTED = 0, /* 未连接 */
    ECSP_SESS_HANDSHAKING,      /* 握手中 */
    ECSP_SESS_HANDSHAKEN,       /* 已握手 */
    ECSP_SESS_NORMAL,           /* 正常通信 */
    ECSP_SESS_VERSION_MISMATCH, /* 版本不符 */
    ECSP_SESS_OFFLINE           /* 掉线 */
} ecsp_session_t;

/* Reported from ecsp_host_step when the link itself concludes something the
 * caller did not submit. */
typedef enum {
    ECSP_HOST_EV_PROBE_FAILED = 1, /* bring-up got no answer: three handshake
                                    * probes, or three version reads */
    ECSP_HOST_EV_VERSION_MISMATCH, /* 0x02 answered a different version */
    ECSP_HOST_EV_REJECTED,         /* peer answered, and the code was not 0x00;
                                    * not retried (spec table 6-1) */
    ECSP_HOST_EV_LINK_FAULT        /* the frame would not fit in the transmit
                                    * FIFO (spec 6.2); not "no answer" */
} ecsp_host_event_t;

typedef void (*ecsp_host_event_cb_t)(ecsp_host_t *h, uint8_t board_addr,
                                     ecsp_host_event_t event, void *user);

/* One submitted transaction's outcome. A later ticket retains this and
 * delivers it once on the step stack. This slice accepts the pointer and
 * neither retains nor calls it. */
typedef void (*ecsp_txn_cb_t)(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                              uint8_t response_code, const uint8_t *payload,
                              uint16_t len, void *user);

/* One slave the host brings up. `expect_version` is the 7-byte software
 * version this product requires (spec 9.2); it is compared raw, not parsed. */
typedef struct {
    uint8_t        board_addr;
    ecsp_version_t expect_version;
} ecsp_host_slave_cfg_t;

/* Caller-owned buffers (ADR-0005). `slaves` is copied at init and need not
 * outlive it. `on_event` may be NULL. */
typedef struct {
    uint8_t product_addr;
    const ecsp_host_slave_cfg_t *slaves;
    size_t slave_count;
    uint8_t *rx_buf;
    size_t   rx_cap;
    uint8_t *tx_buf;
    size_t   tx_cap;
    ecsp_host_event_cb_t on_event;
    void *user;
} ecsp_host_cfg_t;

/* Private per-slave slot. Stack allocation needs the layout here; callers
 * use ecsp_host_session rather than these fields. */
typedef struct {
    uint8_t        board_addr;
    ecsp_version_t expect_version;
    uint8_t        session; /* ecsp_session_t */
    uint8_t        phase;
    uint8_t        attempt;
    uint32_t       sent_ms;
    uint8_t        user_pending;
    uint8_t        user_cmd;
    uint16_t       user_len;
    uint8_t        user_payload[ECSP_MAX_FRAME];
} ecsp_host_slot_t;

struct ecsp_host {
    ecsp_host_cfg_t cfg;
    size_t rx_len;
    size_t tx_len;
    size_t slave_count;
    uint8_t in_step;
    ecsp_host_slot_t slots[ECSP_HOST_SLAVES];
};

int ecsp_host_init(ecsp_host_t *h, const ecsp_host_cfg_t *cfg);

int ecsp_host_submit(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                     const uint8_t *payload, uint16_t len,
                     ecsp_txn_cb_t cb, void *user);

void ecsp_host_step(ecsp_host_t *h, uint32_t now_ms);

void ecsp_host_rx(ecsp_host_t *h, const uint8_t *data, size_t len);

ecsp_session_t ecsp_host_session(const ecsp_host_t *h, uint8_t board_addr);

size_t ecsp_host_tx_pending(const ecsp_host_t *h);

void ecsp_host_tx_drain(ecsp_host_t *h, size_t n);

#endif /* ECSP_HOST_H */
