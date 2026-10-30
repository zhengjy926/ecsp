#include "ecsp/host.h"

#if ECSP_ENABLE_HOST

#include <string.h>

#include "ecsp/frame.h"

/* Same command, including the first send, at most this many times (spec 6.2).
 * Not a config knob: the spec does not let a product change the retry count. */
#define ECSP_HOST_ATTEMPTS 3u

#if ECSP_RESP_HANDSHAKE_MS > 0x7FFFFFFFu || ECSP_RESP_QUERY_MS > 0x7FFFFFFFu
#error "response deadlines must be below 2^31 ms to stay wrap-safe"
#endif

/* phase: private to the slot. */
enum {
    PHASE_SEND_HS = 0,
    PHASE_WAIT_HS,
    PHASE_SEND_VER,
    PHASE_WAIT_VER,
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
    for (i = 0u; i < cfg->slave_count; i++) {
        ecsp_host_slot_t *slot = &h->slots[i];

        slot->board_addr = cfg->slaves[i].board_addr;
        slot->expect_version = cfg->slaves[i].expect_version;
        slot->session = (uint8_t)ECSP_SESS_DISCONNECTED;
        slot->phase = (uint8_t)PHASE_SEND_HS;
        slot->attempt = 0u;
        slot->sent_ms = 0u;
        slot->user_pending = 0u;
        slot->user_cmd = 0u;
        slot->user_len = 0u;
    }
    return ECSP_OK;
}

int ecsp_host_submit(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                     const uint8_t *payload, uint16_t len,
                     ecsp_txn_cb_t cb, void *user)
{
    ecsp_host_slot_t *slot;

    (void)cb;
    (void)user;
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
    if (slot->user_pending != 0u) {
        return ECSP_E_CAP;
    }
    slot->user_cmd = cmd;
    slot->user_len = len;
    if (len > 0u) {
        memcpy(slot->user_payload, payload, len);
    }
    slot->user_pending = 1u;
    return ECSP_OK;
}

static void stop_bringup(ecsp_host_t *h, ecsp_host_slot_t *slot,
                         ecsp_host_event_t event)
{
    slot->session = (uint8_t)ECSP_SESS_DISCONNECTED;
    slot->phase = (uint8_t)PHASE_DONE;
    slot->attempt = 0u;
    report_event(h, slot, event);
}

/* One outstanding request: on its deadline, resend or give up. The attempt
 * counter advances only once the frame is in the transmit FIFO. A frame that
 * does not fit is a link fault (spec 6.2), not another silent probe. */
static void progress_slot(ecsp_host_t *h, ecsp_host_slot_t *slot, uint32_t now_ms)
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
            stop_bringup(h, slot, ECSP_HOST_EV_PROBE_FAILED);
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
        stop_bringup(h, slot, ECSP_HOST_EV_LINK_FAULT);
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

/* A handshake report has an empty payload. Only a 0x00 response (spec 5.2.1)
 * completes the probe and arms the version read (spec 4.1.1). */
static void accept_frame(ecsp_host_t *h, const ecsp_frame_t *frame)
{
    ecsp_host_slot_t *slot;

    if (frame->product_addr != h->cfg.product_addr) {
        return;
    }
    slot = (ecsp_host_slot_t *)lookup_slot(h, frame->board_addr);
    if (slot == NULL) {
        return;
    }
    if (slot->phase == (uint8_t)PHASE_WAIT_HS &&
        frame->cmd == (uint8_t)ECSP_CMD_HANDSHAKE) {
        /* An empty payload is a handshake report, not an answer. */
        if (frame->payload_len == 0u) {
            return;
        }
        if (frame->payload_len == 1u &&
            frame->payload[0] == (uint8_t)ECSP_RS_OK) {
            slot->session = (uint8_t)ECSP_SESS_HANDSHAKEN;
            slot->phase = (uint8_t)PHASE_SEND_VER;
            slot->attempt = 0u;
            return;
        }
        /* spec table 6-1: 0x01/0x02/... are defects. Do not retry them as
         * if the peer had said nothing. */
        stop_bringup(h, slot, ECSP_HOST_EV_REJECTED);
        return;
    }

    /* spec 9.2: the 7 version bytes are compared as raw bytes. */
    if (slot->phase == (uint8_t)PHASE_WAIT_VER &&
        frame->cmd == (uint8_t)ECSP_CMD_GET_VERSION) {
        if (frame->payload_len == 0u) {
            return;
        }
        if (frame->payload_len == (uint16_t)(1u + ECSP_VERSION_LEN) &&
            frame->payload[0] == (uint8_t)ECSP_RS_OK) {
            ecsp_version_t got;

            memcpy(got.b, &frame->payload[1], ECSP_VERSION_LEN);
            slot->phase = (uint8_t)PHASE_DONE;
            slot->attempt = 0u;
            if (ecsp_version_equals(&got, &slot->expect_version)) {
                slot->session = (uint8_t)ECSP_SESS_NORMAL;
            } else {
                slot->session = (uint8_t)ECSP_SESS_VERSION_MISMATCH;
                report_event(h, slot, ECSP_HOST_EV_VERSION_MISMATCH);
            }
            return;
        }
        stop_bringup(h, slot, ECSP_HOST_EV_REJECTED);
    }
}

static void shift_front(uint8_t *buf, size_t *len, size_t n);

static void pump_rx(ecsp_host_t *h)
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
        accept_frame(h, &frame);
        shift_front(h->cfg.rx_buf, &h->rx_len, consumed);
    }
}

void ecsp_host_step(ecsp_host_t *h, uint32_t now_ms)
{
    size_t i;

    if (h == NULL || h->in_step != 0u) {
        return;
    }
    h->in_step = 1u;
    pump_rx(h);
    for (i = 0u; i < h->slave_count; i++) {
        ecsp_host_slot_t *slot = &h->slots[i];
        const uint8_t *payload;

        progress_slot(h, slot, now_ms);
        if (slot->user_pending == 0u ||
            slot->session != (uint8_t)ECSP_SESS_NORMAL) {
            continue;
        }
        payload = (slot->user_len > 0u) ? slot->user_payload : NULL;
        if (queue_frame(h, slot->board_addr, slot->user_cmd,
                        payload, slot->user_len)) {
            slot->user_pending = 0u;
        }
    }
    h->in_step = 0u;
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
