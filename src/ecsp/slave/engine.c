#include "ecsp/slave.h"

#if ECSP_ENABLE_SLAVE

#include <string.h>

#include "ecsp/frame.h"

/* A handshake response is the smallest request plus one response-code byte:
 * header + length(2) + product + cmd + board + RS + crc(2) + tail = 10. This
 * is also the smallest frame the engine ever emits, so a transmit buffer must
 * hold at least this many bytes. */
#define ECSP_HANDSHAKE_RESP_LEN (ECSP_FRAME_MIN_LEN + 1u)

int ecsp_slave_init(ecsp_slave_t *s, const ecsp_slave_cfg_t *cfg)
{
    if (s == NULL || cfg == NULL) {
        return ECSP_E_ARG;
    }
    if (cfg->rx_buf == NULL || cfg->rx_cap < ECSP_FRAME_MIN_LEN) {
        return ECSP_E_ARG;
    }
    if (cfg->tx_buf == NULL || cfg->tx_cap < ECSP_HANDSHAKE_RESP_LEN) {
        return ECSP_E_ARG;
    }

    s->cfg = *cfg;
    s->rx_len = 0u;
    s->tx_len = 0u;
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

/* Emit the 0x00 handshake response directly into the transmit FIFO. The caller
 * has already checked that ECSP_HANDSHAKE_RESP_LEN bytes are available. */
static void answer_handshake(ecsp_slave_t *s)
{
    uint8_t rs = 0x00u;
    size_t n = ecsp_frame_encode(&s->cfg.tx_buf[s->tx_len],
                                 s->cfg.tx_cap - s->tx_len,
                                 s->cfg.product_addr, s->cfg.board_addr,
                                 0x01u, &rs, 1u);
    if (n != 0u) {
        s->tx_len += n;
    }
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
         * verified CRC and tail, so only the address and command filters of
         * this ticket remain. A handshake request has an empty payload
         * (spec 5.2.1); a payload-bearing handshake is "其余情况" and is
         * silent here - its 0x02 rejection lands with the pipeline in #5. */
        if (frame.product_addr == s->cfg.product_addr &&
            frame.board_addr == s->cfg.board_addr &&
            frame.cmd == 0x01u &&
            frame.payload_len == 0u) {
            if (s->tx_len + ECSP_HANDSHAKE_RESP_LEN > s->cfg.tx_cap) {
                /* No room for the response: keep the frame and resume after
                 * the caller drains the transmit FIFO. */
                break;
            }
            answer_handshake(s);
        }
        rx_drop_front(s, consumed);
    }
}

#endif /* ECSP_ENABLE_SLAVE */
