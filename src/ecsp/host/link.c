#include "ecsp/host.h"

#if ECSP_ENABLE_HOST

#include <string.h>

#include "ecsp/frame.h"

#if ECSP_HOST_SLAVES > 255
#error "submission-queue slot index is stored in a uint8_t"
#endif

/* Same command, including the first send, at most this many times (spec 6.2).
 * Not a config knob: the spec does not let a product change the retry count. */
#define ECSP_HOST_ATTEMPTS 3u

/* Dropout is three heartbeat periods (spec 7.2.1). Scaling the heartbeat
 * macro scales the threshold with it. */
#define ECSP_DROPOUT_MS (3u * ECSP_HEARTBEAT_MS)

#if ECSP_RESP_HANDSHAKE_MS > 0x7FFFFFFFu || ECSP_RESP_QUERY_MS > 0x7FFFFFFFu \
    || ECSP_RESP_ACTION_MS > 0x7FFFFFFFu || ECSP_RESP_UPGRADE_MS > 0x7FFFFFFFu \
    || ECSP_SESSION_SETUP_MS > 0x7FFFFFFFu || ECSP_DROPOUT_MS > 0x7FFFFFFFu
#error "host deadlines must be below 2^31 ms to stay wrap-safe"
#endif

/* phase: private to the slot. PASSIVE is "未连接, waiting for a 0x01 report"
 * after three handshake probes (spec 4.1.1). */
enum {
    PHASE_SEND_HS = 0,
    PHASE_WAIT_HS,
    PHASE_SEND_VER,
    PHASE_WAIT_VER,
    PHASE_PASSIVE,
    PHASE_DONE
};

static int deadline_reached(uint32_t sent_ms, uint32_t now_ms, uint32_t limit_ms)
{
    return ((int32_t)(now_ms - sent_ms) >= (int32_t)limit_ms) ? 1 : 0;
}

static void report_event(ecsp_host_t *h, const ecsp_host_slot_t *slot,
                         ecsp_host_event_t event)
{
    if (h->cfg.on_event != NULL) {
        h->cfg.on_event(h, slot->board_addr, event, h->cfg.user);
    }
}

static int queue_frame(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                       const uint8_t *payload, uint16_t payload_len)
{
    size_t frame_len = ECSP_FRAME_MIN_LEN + (size_t)payload_len;
    size_t n;

    if (h->tx_len + frame_len > h->cfg.tx_cap) {
        return 0;
    }
    n = ecsp_frame_encode(&h->cfg.tx_buf[h->tx_len],
                          h->cfg.tx_cap - h->tx_len,
                          h->cfg.product_addr, board_addr,
                          cmd, payload, payload_len);
    if (n == 0u) {
        return 0;
    }
    h->tx_len += n;
    return 1;
}

static const ecsp_host_slot_t *lookup_slot(const ecsp_host_t *h,
                                          uint8_t board_addr)
{
    size_t i;

    for (i = 0u; i < h->slave_count; i++) {
        if (h->slots[i].board_addr == board_addr) {
            return &h->slots[i];
        }
    }
    return NULL;
}

static void note_rx(ecsp_host_slot_t *slot, uint32_t now_ms)
{
    slot->last_rx_ms = now_ms;
    slot->last_rx_valid = 1u;
}

/* Drop one slot from the submission queue. A slot that is not queued is
 * left alone, so finish can call this for every ending transaction. */
static void dequeue_submit(ecsp_host_t *h, const ecsp_host_slot_t *slot)
{
    size_t idx = (size_t)(slot - h->slots);
    size_t i;
    size_t w = 0u;

    for (i = 0u; i < h->submit_n; i++) {
        if (h->submit_q[i] != (uint8_t)idx) {
            h->submit_q[w] = h->submit_q[i];
            w++;
        }
    }
    h->submit_n = (uint8_t)w;
}

/* Copy the callback out before clearing the slot: the callback may submit
 * the next transaction, and must not see the one that just finished.
 * Unqueue before the callback, so a resubmit of this same slave stays queued. */
static void finish_txn(ecsp_host_t *h, ecsp_host_slot_t *slot, int status,
                       uint8_t response_code, const uint8_t *payload,
                       uint16_t len)
{
    ecsp_txn_cb_t cb = slot->txn_cb;
    void *user = slot->txn_user;
    uint8_t cmd = slot->txn_cmd;
    uint8_t board = slot->board_addr;

    dequeue_submit(h, slot);
    slot->txn_state = (uint8_t)ECSP_TXN_IDLE;
    slot->txn_cb = NULL;
    slot->txn_user = NULL;
    slot->txn_attempt = 0u;
    slot->txn_len = 0u;
    slot->txn_defer = 0u;
    if (cb != NULL) {
        cb(h, board, cmd, status, response_code, payload, len, user);
    }
}

int ecsp_host_init(ecsp_host_t *h, const ecsp_host_cfg_t *cfg)
{
    size_t i;

    if (h == NULL || cfg == NULL) {
        return ECSP_E_ARG;
    }
    if (cfg->slave_count > ECSP_HOST_SLAVES) {
        return ECSP_E_ARG;
    }
    if (cfg->slave_count > 0u && cfg->slaves == NULL) {
        return ECSP_E_ARG;
    }
    if (cfg->rx_buf == NULL || cfg->rx_cap < ECSP_MAX_FRAME) {
        return ECSP_E_ARG;
    }
    /* One minimum frame must fit. A larger business frame is refused at
     * submit when it cannot fit this FIFO (spec 6.2: the host must be able
     * to tell "no room" from "no answer"). */
    if (cfg->tx_buf == NULL || cfg->tx_cap < ECSP_FRAME_MIN_LEN) {
        return ECSP_E_ARG;
    }
    for (i = 0u; i < cfg->slave_count; i++) {
        size_t j;

        for (j = 0u; j < i; j++) {
            if (cfg->slaves[j].board_addr == cfg->slaves[i].board_addr) {
                return ECSP_E_ARG;
            }
        }
    }

    h->cfg = *cfg;
    h->rx_len = 0u;
    h->tx_len = 0u;
    h->slave_count = cfg->slave_count;
    h->in_step = 0u;
    h->submit_n = 0u;
    for (i = 0u; i < cfg->slave_count; i++) {
        ecsp_host_slot_t *slot = &h->slots[i];

        slot->board_addr = cfg->slaves[i].board_addr;
        slot->expect_version = cfg->slaves[i].expect_version;
        slot->session = (uint8_t)ECSP_SESS_DISCONNECTED;
        slot->phase = (uint8_t)PHASE_SEND_HS;
        slot->attempt = 0u;
        slot->sent_ms = 0u;
        slot->txn_state = (uint8_t)ECSP_TXN_IDLE;
        slot->txn_cmd = 0u;
        slot->txn_attempt = 0u;
        slot->txn_defer = 0u;
        slot->txn_len = 0u;
        slot->txn_sent_ms = 0u;
        slot->txn_cb = NULL;
        slot->txn_user = NULL;
        slot->last_rx_ms = 0u;
        slot->last_rx_valid = 0u;
        slot->bringup_ms = 0u;
        slot->bringup_valid = 0u;
        slot->bringup_open = 1u;
    }
    return ECSP_OK;
}

int ecsp_host_submit(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                     const uint8_t *payload, uint16_t len,
                     ecsp_txn_cb_t cb, void *user)
{
    ecsp_host_slot_t *slot;

    if (h == NULL || (len > 0u && payload == NULL)) {
        return ECSP_E_ARG;
    }
    if ((size_t)len > (ECSP_MAX_FRAME - ECSP_FRAME_MIN_LEN) ||
        (ECSP_FRAME_MIN_LEN + (size_t)len) > h->cfg.tx_cap) {
        return ECSP_E_CAP;
    }
    slot = (ecsp_host_slot_t *)lookup_slot(h, board_addr);
    if (slot == NULL) {
        return ECSP_E_ARG;
    }
    /* spec 4.1.1 / 9.2: business commands leave the host only in normal
     * communication. Anywhere else the refusal is local — nothing is queued. */
    if (slot->session != (uint8_t)ECSP_SESS_NORMAL) {
        return ECSP_E_STATE;
    }
    if (slot->txn_state != (uint8_t)ECSP_TXN_IDLE) {
        return ECSP_E_CAP;
    }
    slot->txn_cmd = cmd;
    slot->txn_len = len;
    if (len > 0u) {
        memcpy(slot->txn_payload, payload, len);
    }
    slot->txn_cb = cb;
    slot->txn_user = user;
    slot->txn_attempt = 0u;
    /* A submit from inside step only enqueues. The next step sends it,
     * and several of them leave in the order they were submitted (ADR-0005). */
    slot->txn_defer = (h->in_step != 0u) ? 1u : 0u;
    slot->txn_state = (uint8_t)ECSP_TXN_QUEUED;
    h->submit_q[h->submit_n] = (uint8_t)(slot - h->slots);
    h->submit_n++;
    return ECSP_OK;
}

static void stop_bringup(ecsp_host_t *h, ecsp_host_slot_t *slot,
                         ecsp_host_event_t event, uint8_t phase)
{
    slot->session = (uint8_t)ECSP_SESS_DISCONNECTED;
    slot->phase = phase;
    slot->attempt = 0u;
    report_event(h, slot, event);
}

static uint32_t response_limit(uint8_t cmd)
{
    if (cmd == (uint8_t)ECSP_CMD_HANDSHAKE) {
        return ECSP_RESP_HANDSHAKE_MS;
    }
    if (cmd == (uint8_t)ECSP_CMD_START_UPGRADE) {
        return ECSP_RESP_UPGRADE_MS;
    }
    /* Getters (spec 7.1). Everything else shares the 100 ms ceiling. */
    if (cmd == (uint8_t)ECSP_CMD_GET_VERSION ||
        cmd == (uint8_t)ECSP_CMD_GET_HW_VERSION ||
        cmd == (uint8_t)ECSP_CMD_GET_SERIAL ||
        cmd == (uint8_t)ECSP_CMD_GET_SELFTEST ||
        cmd == (uint8_t)ECSP_CMD_GET_UPGRADE ||
        cmd == (uint8_t)ECSP_CMD_GET_FAULT) {
        return ECSP_RESP_QUERY_MS;
    }
    return ECSP_RESP_ACTION_MS;
}

/* One outstanding bring-up request: on its deadline, resend or give up.
 * The attempt counter advances only once the frame is in the transmit FIFO.
 * Three silent handshake probes enter passive wait (spec 4.1.1); they do
 * not go offline until the session-establishment window runs out. */
static void progress_bringup(ecsp_host_t *h, ecsp_host_slot_t *slot,
                             uint32_t now_ms)
{
    int waiting_hs = (slot->phase == (uint8_t)PHASE_WAIT_HS) ? 1 : 0;
    int waiting_ver = (slot->phase == (uint8_t)PHASE_WAIT_VER) ? 1 : 0;
    uint8_t cmd;

    if (waiting_hs || waiting_ver) {
        uint32_t limit = waiting_hs ? ECSP_RESP_HANDSHAKE_MS : ECSP_RESP_QUERY_MS;

        if (!deadline_reached(slot->sent_ms, now_ms, limit)) {
            return;
        }
        if (slot->attempt >= ECSP_HOST_ATTEMPTS) {
            uint8_t phase = waiting_hs ? (uint8_t)PHASE_PASSIVE
                                       : (uint8_t)PHASE_DONE;

            stop_bringup(h, slot, ECSP_HOST_EV_PROBE_FAILED, phase);
            return;
        }
        slot->phase = waiting_hs ? (uint8_t)PHASE_SEND_HS
                                 : (uint8_t)PHASE_SEND_VER;
    }

    if (slot->phase == (uint8_t)PHASE_SEND_HS) {
        cmd = (uint8_t)ECSP_CMD_HANDSHAKE;
    } else if (slot->phase == (uint8_t)PHASE_SEND_VER) {
        cmd = (uint8_t)ECSP_CMD_GET_VERSION;
    } else {
        return;
    }

    if (!queue_frame(h, slot->board_addr, cmd, NULL, 0u)) {
        stop_bringup(h, slot, ECSP_HOST_EV_LINK_FAULT, (uint8_t)PHASE_DONE);
        return;
    }
    slot->attempt++;
    slot->sent_ms = now_ms;
    if (cmd == (uint8_t)ECSP_CMD_HANDSHAKE) {
        slot->session = (uint8_t)ECSP_SESS_HANDSHAKING;
        slot->phase = (uint8_t)PHASE_WAIT_HS;
    } else {
        slot->session = (uint8_t)ECSP_SESS_HANDSHAKEN;
        slot->phase = (uint8_t)PHASE_WAIT_VER;
    }
}

static void fail_txn_link(ecsp_host_t *h, ecsp_host_slot_t *slot)
{
    finish_txn(h, slot, ECSP_E_LINK, 0u, NULL, 0u);
    report_event(h, slot, ECSP_HOST_EV_LINK_FAULT);
}

/* One copy of this transaction into the transmit FIFO (spec 6.2). Returns 1
 * after the attempt count and send time advance. Returns 0 when the FIFO
 * cannot hold the frame; the transaction is already finished. */
static int transmit_txn(ecsp_host_t *h, ecsp_host_slot_t *slot, uint32_t now_ms)
{
    const uint8_t *payload;

    payload = (slot->txn_len > 0u) ? slot->txn_payload : NULL;
    if (!queue_frame(h, slot->board_addr, slot->txn_cmd, payload, slot->txn_len)) {
        fail_txn_link(h, slot);
        return 0;
    }
    slot->txn_attempt++;
    slot->txn_sent_ms = now_ms;
    return 1;
}

/* Table 4-2: a waiting request is retried up to three times with the original
 * payload (so the transaction sequence is unchanged). Exhausting the attempts
 * drops the session, then the transaction goes idle. The first send is not
 * done here: emit_queued walks the submission queue so callback submits leave
 * in the order they were handed in. */
static void progress_txn(ecsp_host_t *h, ecsp_host_slot_t *slot, uint32_t now_ms)
{
    if (slot->session != (uint8_t)ECSP_SESS_NORMAL) {
        return;
    }
    if (slot->txn_state != (uint8_t)ECSP_TXN_WAIT_RESP) {
        return;
    }
    if (!deadline_reached(slot->txn_sent_ms, now_ms,
                          response_limit(slot->txn_cmd))) {
        return;
    }
    if (slot->txn_attempt >= ECSP_HOST_ATTEMPTS) {
        slot->session = (uint8_t)ECSP_SESS_OFFLINE;
        slot->phase = (uint8_t)PHASE_DONE;
        finish_txn(h, slot, ECSP_E_TIMEOUT, 0u, NULL, 0u);
        report_event(h, slot, ECSP_HOST_EV_OFFLINE);
        return;
    }

    (void)transmit_txn(h, slot, now_ms);
}

/* First transmission of each queued submit, oldest first. A submit made while
 * this step is on the stack stays deferred until the next step. */
static void emit_queued(ecsp_host_t *h, uint32_t now_ms)
{
    size_t i = 0u;

    while (i < h->submit_n) {
        ecsp_host_slot_t *slot = &h->slots[h->submit_q[i]];

        if (slot->txn_state != (uint8_t)ECSP_TXN_QUEUED) {
            dequeue_submit(h, slot);
            continue;
        }
        if (slot->txn_defer != 0u ||
            slot->session != (uint8_t)ECSP_SESS_NORMAL) {
            i++;
            continue;
        }
        if (!transmit_txn(h, slot, now_ms)) {
            continue;
        }
        slot->txn_state = (uint8_t)ECSP_TXN_WAIT_RESP;
        dequeue_submit(h, slot);
    }
}

/* spec 7.2.1: three heartbeat periods with no valid frame from this slave. */
static int drop_if_silent(ecsp_host_t *h, ecsp_host_slot_t *slot, uint32_t now_ms)
{
    if (slot->session != (uint8_t)ECSP_SESS_NORMAL || slot->last_rx_valid == 0u) {
        return 0;
    }
    if (!deadline_reached(slot->last_rx_ms, now_ms, ECSP_DROPOUT_MS)) {
        return 0;
    }
    slot->session = (uint8_t)ECSP_SESS_OFFLINE;
    slot->phase = (uint8_t)PHASE_DONE;
    if (slot->txn_state != (uint8_t)ECSP_TXN_IDLE) {
        finish_txn(h, slot, ECSP_E_OFFLINE, 0u, NULL, 0u);
    }
    report_event(h, slot, ECSP_HOST_EV_OFFLINE);
    return 1;
}

/* spec 4.1.1: passive wait may not outlast the session-establishment window.
 * The window closes once the slave has reached normal communication or
 * version-mismatch, and once this timeout has already fired. */
static int setup_expired(ecsp_host_t *h, ecsp_host_slot_t *slot, uint32_t now_ms)
{
    if (slot->bringup_open == 0u) {
        return 0;
    }
    if (slot->bringup_valid == 0u) {
        slot->bringup_ms = now_ms;
        slot->bringup_valid = 1u;
        return 0;
    }
    if (slot->session == (uint8_t)ECSP_SESS_NORMAL ||
        slot->session == (uint8_t)ECSP_SESS_VERSION_MISMATCH ||
        slot->session == (uint8_t)ECSP_SESS_OFFLINE) {
        slot->bringup_open = 0u;
        return 0;
    }
    if (!deadline_reached(slot->bringup_ms, now_ms, ECSP_SESSION_SETUP_MS)) {
        return 0;
    }
    slot->bringup_open = 0u;
    slot->session = (uint8_t)ECSP_SESS_OFFLINE;
    slot->phase = (uint8_t)PHASE_DONE;
    slot->attempt = 0u;
    if (slot->txn_state != (uint8_t)ECSP_TXN_IDLE) {
        finish_txn(h, slot, ECSP_E_OFFLINE, 0u, NULL, 0u);
    }
    report_event(h, slot, ECSP_HOST_EV_OFFLINE);
    return 1;
}

static void arm_probe(ecsp_host_slot_t *slot)
{
    slot->session = (uint8_t)ECSP_SESS_DISCONNECTED;
    slot->phase = (uint8_t)PHASE_SEND_HS;
    slot->attempt = 0u;
}

/* A handshake report has an empty payload. Only a 0x00 response (spec 5.2.1)
 * completes the probe and arms the version read (spec 4.1.1). */
static void accept_frame(ecsp_host_t *h, const ecsp_frame_t *frame,
                         uint32_t now_ms)
{
    ecsp_host_slot_t *slot;

    if (frame->product_addr != h->cfg.product_addr) {
        return;
    }
    slot = (ecsp_host_slot_t *)lookup_slot(h, frame->board_addr);
    if (slot == NULL) {
        return;
    }
    note_rx(slot, now_ms);

    if (frame->cmd == (uint8_t)ECSP_CMD_HANDSHAKE && frame->payload_len == 0u) {
        /* spec 4.1.1: a report during passive wait or after dropout starts
         * a fresh probe. It is not itself an answer, and the host does not
         * reply to it. */
        if (slot->phase == (uint8_t)PHASE_PASSIVE ||
            slot->session == (uint8_t)ECSP_SESS_OFFLINE) {
            arm_probe(slot);
        }
        return;
    }

    if (slot->phase == (uint8_t)PHASE_WAIT_HS &&
        frame->cmd == (uint8_t)ECSP_CMD_HANDSHAKE) {
        if (frame->payload_len == 1u &&
            frame->payload[0] == (uint8_t)ECSP_RS_OK) {
            slot->session = (uint8_t)ECSP_SESS_HANDSHAKEN;
            slot->phase = (uint8_t)PHASE_SEND_VER;
            slot->attempt = 0u;
            return;
        }
        if (frame->payload_len >= 1u) {
            /* spec table 6-1: 0x01/0x02/... are defects. Do not retry them
             * as if the peer had said nothing. */
            stop_bringup(h, slot, ECSP_HOST_EV_REJECTED, (uint8_t)PHASE_DONE);
        }
        return;
    }

    /* spec 9.2: the 7 version bytes are compared as raw bytes. */
    if (slot->phase == (uint8_t)PHASE_WAIT_VER &&
        frame->cmd == (uint8_t)ECSP_CMD_GET_VERSION) {
        if (frame->payload_len == (uint16_t)(1u + ECSP_VERSION_LEN) &&
            frame->payload[0] == (uint8_t)ECSP_RS_OK) {
            ecsp_version_t got;

            memcpy(got.b, &frame->payload[1], ECSP_VERSION_LEN);
            slot->phase = (uint8_t)PHASE_DONE;
            slot->attempt = 0u;
            slot->bringup_open = 0u;
            if (ecsp_version_equals(&got, &slot->expect_version)) {
                slot->session = (uint8_t)ECSP_SESS_NORMAL;
            } else {
                slot->session = (uint8_t)ECSP_SESS_VERSION_MISMATCH;
                report_event(h, slot, ECSP_HOST_EV_VERSION_MISMATCH);
            }
            return;
        }
        if (frame->payload_len >= 1u) {
            stop_bringup(h, slot, ECSP_HOST_EV_REJECTED, (uint8_t)PHASE_DONE);
        }
        return;
    }

    if (slot->session == (uint8_t)ECSP_SESS_NORMAL &&
        slot->txn_state == (uint8_t)ECSP_TXN_WAIT_RESP &&
        frame->cmd == slot->txn_cmd &&
        frame->payload_len >= 1u) {
        finish_txn(h, slot, ECSP_OK, frame->payload[0],
                   &frame->payload[1],
                   (uint16_t)(frame->payload_len - 1u));
    }
}

static void shift_front(uint8_t *buf, size_t *len, size_t n);

static void pump_rx(ecsp_host_t *h, uint32_t now_ms)
{
    for (;;) {
        ecsp_frame_t frame;
        size_t consumed = 0u;
        ecsp_scan_t sc = ecsp_frame_scan(h->cfg.rx_buf, h->rx_len,
                                         &frame, &consumed);

        if (sc == ECSP_SCAN_NEED_MORE) {
            shift_front(h->cfg.rx_buf, &h->rx_len, consumed);
            return;
        }
        if (sc == ECSP_SCAN_SKIPPED) {
            shift_front(h->cfg.rx_buf, &h->rx_len, consumed);
            continue;
        }
        accept_frame(h, &frame, now_ms);
        shift_front(h->cfg.rx_buf, &h->rx_len, consumed);
    }
}

int ecsp_host_step(ecsp_host_t *h, uint32_t now_ms)
{
    size_t i;

    if (h == NULL) {
        return ECSP_E_ARG;
    }
    /* A callback is on this stack. Do not advance, and do not touch the
     * retry count (ADR-0005). A different host has its own flag. */
    if (h->in_step != 0u) {
        return ECSP_E_REENT;
    }
    h->in_step = 1u;
    pump_rx(h, now_ms);
    for (i = 0u; i < h->slave_count; i++) {
        ecsp_host_slot_t *slot = &h->slots[i];

        if (setup_expired(h, slot, now_ms) || drop_if_silent(h, slot, now_ms)) {
            continue;
        }
        progress_bringup(h, slot, now_ms);
        progress_txn(h, slot, now_ms);
    }
    emit_queued(h, now_ms);
    for (i = 0u; i < h->slave_count; i++) {
        h->slots[i].txn_defer = 0u;
    }
    h->in_step = 0u;
    return ECSP_OK;
}

void ecsp_host_rx(ecsp_host_t *h, const uint8_t *data, size_t len)
{
    size_t room;
    size_t n;

    if (h == NULL || data == NULL || len == 0u) {
        return;
    }
    room = h->cfg.rx_cap - h->rx_len;
    n = (len < room) ? len : room;
    if (n > 0u) {
        memcpy(&h->cfg.rx_buf[h->rx_len], data, n);
        h->rx_len += n;
    }
}

ecsp_session_t ecsp_host_session(const ecsp_host_t *h, uint8_t board_addr)
{
    const ecsp_host_slot_t *slot;

    if (h == NULL) {
        return ECSP_SESS_DISCONNECTED;
    }
    slot = lookup_slot(h, board_addr);
    if (slot == NULL) {
        return ECSP_SESS_DISCONNECTED;
    }
    return (ecsp_session_t)slot->session;
}

ecsp_txn_state_t ecsp_host_txn(const ecsp_host_t *h, uint8_t board_addr)
{
    const ecsp_host_slot_t *slot;

    if (h == NULL) {
        return ECSP_TXN_IDLE;
    }
    slot = lookup_slot(h, board_addr);
    if (slot == NULL) {
        return ECSP_TXN_IDLE;
    }
    return (ecsp_txn_state_t)slot->txn_state;
}

size_t ecsp_host_tx_pending(const ecsp_host_t *h)
{
    if (h == NULL) {
        return 0u;
    }
    return h->tx_len;
}

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

void ecsp_host_tx_drain(ecsp_host_t *h, size_t n)
{
    if (h == NULL) {
        return;
    }
    shift_front(h->cfg.tx_buf, &h->tx_len, n);
}

#endif /* ECSP_ENABLE_HOST */
