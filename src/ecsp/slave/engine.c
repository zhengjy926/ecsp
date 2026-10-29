#include "ecsp/slave.h"

#if ECSP_ENABLE_SLAVE

#include <string.h>

#include "ecsp/cmds.h"
#include "ecsp/frame.h"

/* Largest frame this engine emits: a get-version response carries the response
 * code plus the 7-byte version (spec 5.2.2). A transmit buffer must hold at
 * least this many bytes. */
#define ECSP_VERSION_RESP_LEN (ECSP_FRAME_MIN_LEN + 1u + ECSP_VERSION_LEN)

int ecsp_slave_init(ecsp_slave_t *s, const ecsp_slave_cfg_t *cfg)
{
    if (s == NULL || cfg == NULL) {
        return ECSP_E_ARG;
    }
    if (cfg->rx_buf == NULL || cfg->rx_cap < ECSP_FRAME_MIN_LEN) {
        return ECSP_E_ARG;
    }
    if (cfg->tx_buf == NULL || cfg->tx_cap < ECSP_VERSION_RESP_LEN) {
        return ECSP_E_ARG;
    }

    s->cfg = *cfg;
    s->rx_len = 0u;
    s->tx_len = 0u;
    s->ready = 0u;
    return ECSP_OK;
}

void ecsp_slave_rx(ecsp_slave_t *s, const uint8_t *data, size_t len)
{
    size_t room;
    size_t n;

    if (s == NULL || data == NULL || len == 0u) {
        return;
    }

    room = s->cfg.rx_cap - s->rx_len;
    n = (len < room) ? len : room;
    if (n > 0u) {
        memcpy(&s->cfg.rx_buf[s->rx_len], data, n);
        s->rx_len += n;
    }
    /* Bytes beyond the receive capacity are dropped; the sender's retransmit
     * (spec 6.2) recovers them. */
}

size_t ecsp_slave_tx_pending(const ecsp_slave_t *s)
{
    if (s == NULL) {
        return 0u;
    }
    return s->tx_len;
}

/* Remove the `n` oldest bytes from `buf` (which currently holds `*len` bytes),
 * shifting the remainder to the front. */
static void shift_front(uint8_t *buf, size_t *len, size_t n)
{
    if (n == 0u || *len == 0u) {
        return;
    }
    if (n >= *len) {
        *len = 0u;
        return;
    }
    memmove(buf, buf + n, *len - n);
    *len -= n;
}

void ecsp_slave_tx_drain(ecsp_slave_t *s, size_t n)
{
    if (s == NULL) {
        return;
    }
    shift_front(s->cfg.tx_buf, &s->tx_len, n);
}

static void rx_drop_front(ecsp_slave_t *s, size_t n)
{
    shift_front(s->cfg.rx_buf, &s->rx_len, n);
}

/* Emit a response frame into the transmit FIFO. Returns 1 on success, 0 when
 * the FIFO lacks room for the whole frame (the caller keeps the request frame
 * and resumes after draining). */
static int answer(ecsp_slave_t *s, uint8_t cmd,
                  const uint8_t *payload, uint16_t payload_len)
{
    size_t frame_len = ECSP_FRAME_MIN_LEN + (size_t)payload_len;
    size_t n;

    if (s->tx_len + frame_len > s->cfg.tx_cap) {
        return 0;
    }
    n = ecsp_frame_encode(&s->cfg.tx_buf[s->tx_len],
                          s->cfg.tx_cap - s->tx_len,
                          s->cfg.product_addr, s->cfg.board_addr,
                          cmd, payload, payload_len);
    if (n == 0u) {
        return 0;
    }
    s->tx_len += n;
    return 1;
}

/* Emit a single-byte (response-code only) answer. */
static int answer_rs(ecsp_slave_t *s, uint8_t cmd, uint8_t rs)
{
    return answer(s, cmd, &rs, 1u);
}

/* Accept a handshake: answer 0x00 and enter the ready state (spec 5.2.1). */
static int answer_handshake(ecsp_slave_t *s)
{
    if (!answer_rs(s, ECSP_CMD_HANDSHAKE, ECSP_RS_OK)) {
        return 0;
    }
    s->ready = 1u;
    return 1;
}

/* Answer a get-version request with 0x00 + the injected 7-byte version
 * (spec 5.2.2), copied verbatim (never NUL-terminated). */
static int answer_version(ecsp_slave_t *s)
{
    uint8_t payload[1u + ECSP_VERSION_LEN];
    payload[0] = ECSP_RS_OK;
    memcpy(&payload[1], s->cfg.version.b, ECSP_VERSION_LEN);
    return answer(s, ECSP_CMD_GET_VERSION, payload, 1u + ECSP_VERSION_LEN);
}

void ecsp_slave_step(ecsp_slave_t *s, uint32_t now_ms)
{
    (void)now_ms;

    if (s == NULL) {
        return;
    }

    for (;;) {
        ecsp_frame_t frame;
        size_t consumed = 0u;
        ecsp_scan_t sc = ecsp_frame_scan(s->cfg.rx_buf, s->rx_len,
                                         &frame, &consumed);

        if (sc == ECSP_SCAN_NEED_MORE) {
            /* Drop leading garbage; keep any partial frame for the next call. */
            rx_drop_front(s, consumed);
            break;
        }
        if (sc == ECSP_SCAN_SKIPPED) {
            rx_drop_front(s, consumed);
            continue;
        }

        /* ECSP_SCAN_OK: a complete, well-formed frame. The scanner already
         * verified CRC and tail; the address filter comes first (spec 2.1). */
        if (frame.product_addr != s->cfg.product_addr ||
            frame.board_addr != s->cfg.board_addr) {
            rx_drop_front(s, consumed);
            continue;
        }

        if (frame.cmd == ECSP_CMD_HANDSHAKE) {
            if (frame.payload_len == 0u) {
                if (!answer_handshake(s)) {
                    /* No room: keep the frame and resume after the caller
                     * drains the transmit FIFO. */
                    break;
                }
            } else {
                /* Length gate (ADR-0002 step 5): a handshake request must
                 * carry an empty payload (spec 5.2.1). */
                if (!answer_rs(s, ECSP_CMD_HANDSHAKE, ECSP_RS_BAD_LEN)) {
                    break;
                }
            }
        } else if (!s->ready) {
            /* Session gate (spec 4.1.2, ADR-0002 step 4): any non-handshake
             * command before the handshake is rejected 0x10, no data. */
            if (!answer_rs(s, frame.cmd, ECSP_RS_NOT_HANDSHAKEN)) {
                break;
            }
        } else if (frame.cmd == ECSP_CMD_GET_VERSION) {
            if (frame.payload_len == 0u) {
                if (!answer_version(s)) {
                    break;
                }
            } else {
                /* Length gate (ADR-0002 step 5): a get-version request must
                 * carry an empty payload (spec 5.2.2). */
                if (!answer_rs(s, ECSP_CMD_GET_VERSION, ECSP_RS_BAD_LEN)) {
                    break;
                }
            }
        }
        /* else: remaining commands after handshake are handled by later
         * tickets (0x04 for unknown, passthrough commands in #5). */

        rx_drop_front(s, consumed);
    }
}

#endif /* ECSP_ENABLE_SLAVE */
