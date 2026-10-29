#include "ecsp/slave.h"

#if ECSP_ENABLE_SLAVE

#include <string.h>

#include "ecsp/cmds.h"
#include "ecsp/frame.h"
#include "session.h"
#include "txn_cache.h"

/* Maximum response DATA bytes a single answer may carry (the frame payload is
 * the response code plus this data). */
#define ECSP_RESP_DATA_CAP (ECSP_MAX_FRAME - ECSP_FRAME_MIN_LEN - 1u)

/* Every common-command answer (the largest is get-serial-number) must fit in
 * one frame; otherwise the engine could accept a command it can never answer. */
#if ECSP_MAX_FRAME < (ECSP_FRAME_MIN_LEN + 1u + ECSP_SERIAL_LEN)
#error "ECSP_MAX_FRAME is too small for the largest common-command response"
#endif

/* Internal descriptor for a common command implemented by the library. The
 * common table owns address/session/length/parameter gating, so handlers only
 * execute the action. */
typedef struct {
    uint8_t  cmd;
    uint8_t  flags;
    uint16_t req_len;
    uint16_t resp_max; /* largest response DATA length this handler emits */
    ecsp_cmd_validate_t validate;
    ecsp_slave_handler_t handler;
} common_cmd_t;

/* ---- parameter validators (ADR-0002 step 8) ---- */

static int valid_hw_version(const ecsp_frame_t *req)
{
    ecsp_version_t v;
    return ecsp_version_parse(req->payload, req->payload_len, &v);
}

static int valid_serial_number(const ecsp_frame_t *req)
{
    ecsp_serial_t sn;
    return ecsp_serial_parse(req->payload, req->payload_len, &sn);
}

static int valid_low_power_mode(const ecsp_frame_t *req)
{
    return ecsp_low_power_mode_valid(req->payload[0]) ? ECSP_OK : ECSP_E_VAL;
}

/* ---- handlers (ADR-0002 step 9) ---- */

static int h_handshake(ecsp_slave_t *s, const ecsp_frame_t *req,
                       uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)req;
    (void)resp;
    (void)cap;
    *resp_len = 0u;
    /* Answer first, enter the ready state afterwards (spec 5.2.1). The engine
     * applies the state change only once the answer is queued. When the slave
     * is already ready this is the "host reset unexpectedly" case: answer 0x00,
     * stay ready and do not resume the 0x01 report stream (spec 4.1.4). The
     * other half of 4.1.4 - aborting in-flight business - has no execution
     * axis to abort yet; it arrives with the long-running ticket. */
    s->pending_ready = 1u;
    return ECSP_RS_OK;
}

static int h_get_version(ecsp_slave_t *s, const ecsp_frame_t *req,
                         uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)req;
    (void)cap;
    memcpy(resp, s->cfg.version.b, ECSP_VERSION_LEN);
    *resp_len = ECSP_VERSION_LEN;
    return ECSP_RS_OK;
}

static int h_set_hw_version(ecsp_slave_t *s, const ecsp_frame_t *req,
                            uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)resp;
    (void)cap;
    *resp_len = 0u;
    /* The pipeline's validator already accepted the 7-byte "X.Y.Z.N" shape. */
    memcpy(s->cfg.identity->hardware_version.b, req->payload, ECSP_VERSION_LEN);
    return ECSP_RS_OK;
}

static int h_get_hw_version(ecsp_slave_t *s, const ecsp_frame_t *req,
                            uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)req;
    (void)cap;
    memcpy(resp, s->cfg.identity->hardware_version.b, ECSP_VERSION_LEN);
    *resp_len = ECSP_VERSION_LEN;
    return ECSP_RS_OK;
}

static int h_set_serial_number(ecsp_slave_t *s, const ecsp_frame_t *req,
                               uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)resp;
    (void)cap;
    *resp_len = 0u;
    /* The pipeline's validator already accepted "SN" + 10 digits. */
    memcpy(s->cfg.identity->serial_number.b, req->payload, ECSP_SERIAL_LEN);
    return ECSP_RS_OK;
}

static int h_get_serial_number(ecsp_slave_t *s, const ecsp_frame_t *req,
                               uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)req;
    (void)cap;
    memcpy(resp, s->cfg.identity->serial_number.b, ECSP_SERIAL_LEN);
    *resp_len = ECSP_SERIAL_LEN;
    return ECSP_RS_OK;
}

static int h_system_reset(ecsp_slave_t *s, const ecsp_frame_t *req,
                          uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)req;
    (void)resp;
    (void)cap;
    *resp_len = 0u;
    /* A previous reset sequence is still in progress: its answer is queued and
     * the product resets after draining. A second, different transaction must
     * not start a concurrent reset, so it is answered 0x11 (spec 5.2.7, 4.4).
     * A retransmission of the same transaction never reaches this handler - it
     * replays the cached answer. */
    if (s->reset_requested != 0u) {
        return ECSP_RS_BUSY;
    }
    /* Answer first, reset afterwards (spec 5.2.7); the engine applies the
     * reset once the answer is queued. */
    s->pending_reset = 1u;
    return ECSP_RS_OK;
}

static int h_set_low_power(ecsp_slave_t *s, const ecsp_frame_t *req,
                           uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)resp;
    (void)cap;
    *resp_len = 0u;
    s->low_power_mode = req->payload[0];
    return ECSP_RS_OK;
}

static int h_get_fault(ecsp_slave_t *s, const ecsp_frame_t *req,
                       uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)req;
    (void)cap;
    resp[0] = s->fault_word;
    *resp_len = 1u;
    return ECSP_RS_OK;
}

/* Common commands this slice implements, in command-code order. The session,
 * self-test and upgrade commands land with later tickets; until then their
 * codes are simply absent and answered 0x04. */
static const common_cmd_t g_common_cmds[] = {
    { ECSP_CMD_HANDSHAKE, ECSP_CMD_PREEMPTIVE, 0u, 0u, NULL,
      h_handshake },
    { ECSP_CMD_GET_VERSION, 0u, 0u, ECSP_VERSION_LEN, NULL,
      h_get_version },
    { ECSP_CMD_SET_HW_VERSION, 0u, ECSP_VERSION_LEN, 0u, valid_hw_version,
      h_set_hw_version },
    { ECSP_CMD_GET_HW_VERSION, 0u, 0u, ECSP_VERSION_LEN, NULL,
      h_get_hw_version },
    { ECSP_CMD_SET_SERIAL, 0u, ECSP_SERIAL_LEN, 0u, valid_serial_number,
      h_set_serial_number },
    { ECSP_CMD_GET_SERIAL, 0u, 0u, ECSP_SERIAL_LEN, NULL,
      h_get_serial_number },
    { ECSP_CMD_SYSTEM_RESET, (uint8_t)(ECSP_CMD_PREEMPTIVE | ECSP_CMD_TXN_SEQ),
      ECSP_TXN_SEQ_LEN, 0u, NULL, h_system_reset },
    { ECSP_CMD_SET_LOW_POWER, 0u, 1u, 0u, valid_low_power_mode,
      h_set_low_power },
    { ECSP_CMD_GET_FAULT, 0u, 0u, 1u, NULL, h_get_fault },
};

#define ECSP_COMMON_CMD_COUNT \
    (sizeof g_common_cmds / sizeof g_common_cmds[0])

static const common_cmd_t *find_common_cmd(uint8_t cmd)
{
    size_t i;
    for (i = 0u; i < ECSP_COMMON_CMD_COUNT; i++) {
        if (g_common_cmds[i].cmd == cmd) {
            return &g_common_cmds[i];
        }
    }
    return NULL;
}

static const ecsp_cmd_entry_t *find_board_cmd(const ecsp_slave_t *s, uint8_t cmd)
{
    size_t i;
    if (s->cfg.cmds.table == NULL) {
        return NULL;
    }
    for (i = 0u; i < s->cfg.cmds.table_len; i++) {
        if (s->cfg.cmds.table[i].cmd == cmd) {
            return &s->cfg.cmds.table[i];
        }
    }
    return NULL;
}

int ecsp_slave_init(ecsp_slave_t *s, const ecsp_slave_cfg_t *cfg)
{
    size_t min_tx;
    size_t i;

    if (s == NULL || cfg == NULL) {
        return ECSP_E_ARG;
    }
    if (cfg->rx_buf == NULL || cfg->rx_cap < ECSP_FRAME_MIN_LEN) {
        return ECSP_E_ARG;
    }
    if (cfg->identity == NULL) {
        return ECSP_E_ARG;
    }
    if (cfg->cmds.table == NULL && cfg->cmds.table_len != 0u) {
        return ECSP_E_ARG;
    }
    if (cfg->tx_buf == NULL) {
        return ECSP_E_ARG;
    }

    /* The transmit FIFO must hold the largest answer the engine can queue:
     * the largest common response or any response declared by the board
     * table. */
    min_tx = ECSP_FRAME_MIN_LEN + 1u + ECSP_SERIAL_LEN;
    for (i = 0u; i < cfg->cmds.table_len; i++) {
        size_t need;
        if (cfg->cmds.table[i].resp_max > ECSP_RESP_DATA_CAP) {
            return ECSP_E_ARG;
        }
        need = ECSP_FRAME_MIN_LEN + 1u + cfg->cmds.table[i].resp_max;
        if (need > min_tx) {
            min_tx = need;
        }
    }
    if (cfg->tx_cap < min_tx) {
        return ECSP_E_ARG;
    }

    s->cfg = *cfg;
    s->rx_len = 0u;
    s->tx_len = 0u;
    s->last_send.at_ms = 0u;
    s->last_send.valid = 0u;
    s->last_report.at_ms = 0u;
    s->last_report.valid = 0u;
    s->ready = 0u;
    s->fault_word = cfg->fault_word;
    s->low_power_mode = 0u;
    s->pending_ready = 0u;
    s->pending_reset = 0u;
    s->reset_requested = 0u;
    /* A soft reset empties the transaction cache (ADR-0004). Re-initialising
     * the context is how this library models that reset. */
    ecsp_txn_cache_clear(s);
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

/* Emit a frame into the transmit FIFO. Returns 1 on success, 0 when the FIFO
 * lacks room for the whole frame (the caller keeps the request frame and
 * resumes after draining). Every frame that is queued restarts the heartbeat
 * window: from this library's point of view a frame is sent once the caller
 * owns it in the FIFO (spec 7.2.2). */
static int send_frame(ecsp_slave_t *s, uint8_t cmd,
                      const uint8_t *payload, uint16_t payload_len,
                      uint32_t now_ms)
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
    ecsp_session_note_sent(s, now_ms);
    return 1;
}

/* Emit a single-byte (response-code only) answer. */
static int answer_rs(ecsp_slave_t *s, uint8_t cmd, uint8_t rs, uint32_t now_ms)
{
    return send_frame(s, cmd, &rs, 1u, now_ms);
}

/* Apply the state changes a handler requested, but only after its answer has
 * been queued. */
static void apply_pending_state(ecsp_slave_t *s)
{
    if (s->pending_ready) {
        s->pending_ready = 0u;
        s->ready = 1u;
        /* Every accepted handshake empties the transaction cache (ADR-0004):
         * the transaction sequence belongs to one run of the host, and a host
         * that restarts begins its counters at 0 again. A handshake the
         * pipeline rejected (wrong length) is not a restart signal and does not
         * get to change state. */
        ecsp_txn_cache_clear(s);
    }
    if (s->pending_reset) {
        s->pending_reset = 0u;
        /* The product performs the actual MCU reset after draining the queued
         * answer. That reset re-initialises this context, and the
         * re-initialisation is where the session goes back to "waiting for a
         * handshake" and the transaction cache is emptied - not here. Between
         * the answer and the reset a retransmitted 0x07 with the same sequence
         * has to replay its answer (spec 4.2); clearing the session now would
         * answer 0x10 instead, which the host cannot tell apart from a
         * rejection. */
        s->reset_requested = 1u;
    }
}

/* Emit the periodic frame when the session module says one is due. At most one
 * per step, so a stalled caller clock cannot release a burst. */
static void report_if_due(ecsp_slave_t *s, uint32_t now_ms)
{
    uint8_t cmd = ecsp_session_due_report(s, now_ms);

    /* A periodic report carries no payload and no response code (spec 5.2.1
     * and 5.2.16 examples are minimal frames). A full FIFO defers it: nothing
     * went out, so the report cadence must not advance and the next step
     * retries. */
    if (cmd != 0u && send_frame(s, cmd, NULL, 0u, now_ms)) {
        ecsp_session_note_report(s, now_ms);
    }
}

void ecsp_slave_step(ecsp_slave_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }

    /* The session clocks have no value before the first step: latch them so
     * the first periodic frame is due a full period later, not immediately
     * (spec 4.1.2 "after power-up ... reports at 1 Hz"). */
    ecsp_session_start(s, now_ms);

    for (;;) {
        ecsp_frame_t frame;
        size_t consumed = 0u;
        const common_cmd_t *common;
        const ecsp_cmd_entry_t *board;
        uint16_t req_len;
        size_t max_data;
        uint8_t flags;
        uint8_t seq;
        int has_seq;
        ecsp_scan_t sc = ecsp_frame_scan(s->cfg.rx_buf, s->rx_len,
                                         &frame, &consumed);

        /* Step 1 (framing, CRC, tail) is the scanner's job. */
        if (sc == ECSP_SCAN_NEED_MORE) {
            /* Drop leading garbage; keep any partial frame for the next call. */
            rx_drop_front(s, consumed);
            break;
        }
        if (sc == ECSP_SCAN_SKIPPED) {
            rx_drop_front(s, consumed);
            continue;
        }

        /* Step 2: address. A frame for another node is discarded silently
         * (spec 2.1), before any command-code decision. */
        if (frame.product_addr != s->cfg.product_addr ||
            frame.board_addr != s->cfg.board_addr) {
            rx_drop_front(s, consumed);
            continue;
        }

        /* Step 3: undefined command or wrong direction -> 0x04. Common
         * commands live in the library; board commands come from the caller's
         * table. Event-class codes are absent from both, so a host request
         * using one is answered 0x04 here. */
        common = NULL;
        board = NULL;
        if (frame.cmd >= ECSP_CMD_COMMON_MIN &&
            frame.cmd <= ECSP_CMD_COMMON_MAX) {
            common = find_common_cmd(frame.cmd);
        } else if (frame.cmd >= ECSP_CMD_BOARD_MIN &&
                   frame.cmd <= ECSP_CMD_BOARD_MAX) {
            board = find_board_cmd(s, frame.cmd);
        }
        if (common == NULL && board == NULL) {
            if (!answer_rs(s, frame.cmd, ECSP_RS_UNSUPPORTED, now_ms)) {
                break;
            }
            rx_drop_front(s, consumed);
            continue;
        }

        /* Step 4: session. Only the handshake may arrive before the session is
         * established (spec 4.1.2). */
        if (frame.cmd != ECSP_CMD_HANDSHAKE && !s->ready) {
            if (!answer_rs(s, frame.cmd, ECSP_RS_NOT_HANDSHAKEN, now_ms)) {
                break;
            }
            rx_drop_front(s, consumed);
            continue;
        }

        /* Step 5: payload length. */
        req_len = (common != NULL) ? common->req_len : board->req_len;
        if (req_len != ECSP_CMD_LEN_ANY && frame.payload_len != req_len) {
            if (!answer_rs(s, frame.cmd, ECSP_RS_BAD_LEN, now_ms)) {
                break;
            }
            rx_drop_front(s, consumed);
            continue;
        }

        /* Step 6: transaction cache. Only a non-idempotent action command is
         * deduplicated, and its first payload byte is the transaction sequence
         * (spec 4.2). A retransmit that hits the cache replays the earlier
         * answer frame byte for byte and never re-enters the handler, so a
         * physical action cannot happen twice (ADR-0004). The session gate is
         * earlier, so a cached entry can never resurrect a dead session. */
        flags = (common != NULL) ? common->flags : board->flags;
        has_seq = ((flags & (uint8_t)ECSP_CMD_TXN_SEQ) != 0u &&
                   frame.payload_len >= ECSP_TXN_SEQ_LEN) ? 1 : 0;
        seq = (has_seq != 0) ? frame.payload[0] : 0u;
        if (has_seq != 0) {
            const uint8_t *cached = NULL;
            size_t cached_len = 0u;

            if (ecsp_txn_cache_lookup(s, frame.cmd, seq, &cached, &cached_len)) {
                if (s->cfg.tx_cap - s->tx_len < cached_len) {
                    /* No room for the replay yet: keep the request frame and
                     * resume after the caller drains the FIFO. */
                    break;
                }
                memcpy(&s->cfg.tx_buf[s->tx_len], cached, cached_len);
                s->tx_len += cached_len;
                /* A replayed answer is still a frame on the wire (spec 7.2.2). */
                ecsp_session_note_sent(s, now_ms);
                rx_drop_front(s, consumed);
                continue;
            }
        }

        /* Step 7: long-running busy -> 0x11 (later ticket). */

        /* Step 8: parameters. The command's validator decides whether a bad
         * variable length is a length error (0x02) or an illegal value (0x03);
         * the handler never repeats this check. */
        {
            ecsp_cmd_validate_t validate =
                (common != NULL) ? common->validate : board->validate;
            if (validate != NULL) {
                int v = validate(&frame);
                if (v != ECSP_OK) {
                    uint8_t rs = (v == ECSP_E_LEN) ? ECSP_RS_BAD_LEN
                                                   : ECSP_RS_BAD_PARAM;
                    if (!answer_rs(s, frame.cmd, rs, now_ms)) {
                        break;
                    }
                    rx_drop_front(s, consumed);
                    continue;
                }
            }
        }

        /* Step 9: execute the handler and use its return value verbatim as the
         * response code. The engine never overrides it. */
        {
            uint8_t payload[ECSP_MAX_FRAME - ECSP_FRAME_MIN_LEN];
            size_t data_len = 0u;
            size_t tx_start;
            int rs;
            uint16_t payload_len;
            ecsp_slave_handler_t handler =
                (common != NULL) ? common->handler : board->handler;

            /* Reserve room for this command's largest possible answer before
             * running the handler, so a full FIFO cannot cause a side effect
             * to be executed twice. */
            max_data = (common != NULL)
                           ? (size_t)common->resp_max
                           : (size_t)board->resp_max;
            if (s->cfg.tx_cap - s->tx_len <
                ECSP_FRAME_MIN_LEN + 1u + max_data) {
                break;
            }

            rs = handler(s, &frame, &payload[1], sizeof payload - 1u,
                         &data_len);
            if (data_len > sizeof payload - 1u) {
                data_len = 0u;
            }
            payload[0] = (uint8_t)rs;
            payload_len = (rs == ECSP_RS_OK)
                              ? (uint16_t)(1u + data_len)
                              : 1u;
            tx_start = s->tx_len;
            if (!send_frame(s, frame.cmd, payload, payload_len, now_ms)) {
                break;
            }
            if (has_seq != 0) {
                /* Cache the answer exactly as queued, so a retransmit replays
                 * the same bytes without running the handler. An answer that
                 * does not fit a cache slot is still delivered to the host;
                 * store reports ECSP_E_CAP and nothing is remembered. */
                (void)ecsp_txn_cache_store(s, frame.cmd, seq,
                                           &s->cfg.tx_buf[tx_start],
                                           s->tx_len - tx_start);
            }
            apply_pending_state(s);
        }

        rx_drop_front(s, consumed);
    }

    /* Received frames were answered first: a ready slave that just answered
     * needs no heartbeat yet, because the answer restarted the window
     * (spec 7.2.2). The waiting-state 0x01 report is on its own fixed cadence
     * and is therefore not suppressed by answers (spec 4.1.2). */
    report_if_due(s, now_ms);
}

uint8_t ecsp_slave_low_power_mode(const ecsp_slave_t *s)
{
    if (s == NULL) {
        return ECSP_LOW_POWER_EXIT;
    }
    return s->low_power_mode;
}

int ecsp_slave_reset_requested(const ecsp_slave_t *s)
{
    if (s == NULL) {
        return 0;
    }
    return (s->reset_requested != 0u) ? 1 : 0;
}

#endif /* ECSP_ENABLE_SLAVE */
