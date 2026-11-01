/*
 * L3 slave session timing (ticket #7; spec 4.1.2, 4.1.3, 4.1.4, 5.2.16, 7.2.2).
 *
 * Seam under test: the caller-pushed `now_ms` of ecsp_slave_step and the
 * caller-owned transmit FIFO. The library never reads a clock, so each test
 * pushes its own fake millisecond value per step and asserts the frame that
 * appears in the FIFO.
 *
 * Expected behavior (spec 4.1.2 / 4.1.3 / 4.1.4 / 7.2.2):
 *  - waiting for a handshake: a 0x01 report every 1000 ms, on its own fixed
 *    cadence even while the host keeps sending other commands;
 *  - an accepted handshake: answer 0x00, stop the reports, enter "ready";
 *  - ready: a 0x10 heartbeat every 1000 ms of silence;
 *  - any frame the engine queues - heartbeat or answer - restarts that silence
 *    window ("last send time", spec 7.2.2);
 *  - a handshake request while already ready (host power-cycle) is answered
 *    0x00, leaves the slave ready, and never resumes 0x01 reports;
 *  - a now_ms wrap from 0xFFFFFFFF to 0 neither skips a frame nor releases a
 *    burst of back-dated ones.
 *
 * Timing is asserted with the spec's 1000 ms numbers, not with the
 * ECSP_HEARTBEAT_MS macro, so the tests pin the spec default rather than the
 * implementation's knob.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

#define PERIOD_MS 1000u

/* spec 5.2.1: the slave report and the host request are byte-identical. */
static const uint8_t handshake_report[] = {
    0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
};
static const uint8_t handshake_resp[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0x00, 0x50, 0x55, 0x0D
};

/* spec 5.2.16 heartbeat report (P=0x03, B=0x02). */
static const uint8_t heartbeat[] = {
    0xFA, 0x09, 0x00, 0x03, 0x10, 0x02, 0x84, 0x00, 0x0D
};

/* spec 5.2.2 get-version response for the injected "1.0.0.0" version. */
static const uint8_t version_resp[] = {
    0xFA, 0x11, 0x00, 0x03, 0x02, 0x02, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30,
    0xFA, 0x0E, 0x0D
};

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;

static ecsp_version_t make_version(const uint8_t b[7])
{
    ecsp_version_t v;
    memcpy(v.b, b, 7u);
    return v;
}

static void init_slave_cap(size_t tx_cap)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t sw_version[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t hw_version[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t serial[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_identity.hardware_version = make_version(hw_version);
    memcpy(g_identity.serial_number.b, serial, sizeof serial);

    cfg.product_addr = 0x03u;
    cfg.board_addr   = 0x02u;
    cfg.version      = make_version(sw_version);
    cfg.identity     = &g_identity;
    cfg.fault_word   = 0x00u;
    cfg.upload_mode  = ECSP_UPLOAD_ON;
    cfg.cmds.table   = NULL;
    cfg.cmds.table_len = 0u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = tx_cap;
    CHECK_EQ(0, ecsp_slave_init(&g_slave, &cfg));
}

static void init_slave(void)
{
    init_slave_cap(sizeof g_tx);
}

static void step_at(uint32_t now_ms)
{
    ecsp_slave_step(&g_slave, now_ms);
}

/* Feed one request for this node (P=0x03/B=0x02) and step the engine. */
static void send_at(uint8_t cmd, const uint8_t *payload, uint16_t len,
                    uint32_t now_ms)
{
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n = ecsp_frame_encode(frame, sizeof frame, 0x03u, 0x02u, cmd,
                                 payload, len);

    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    step_at(now_ms);
}

/* Build a response frame carrying [rs] + data. */
static size_t answer_frame(uint8_t *buf, uint8_t cmd, uint8_t rs,
                           const uint8_t *data, uint16_t data_len)
{
    uint8_t payload[ECSP_MAX_FRAME];
    size_t n;

    payload[0] = rs;
    if (data_len > 0u) {
        memcpy(&payload[1], data, data_len);
    }
    n = ecsp_frame_encode(buf, ECSP_MAX_FRAME, 0x03u, 0x02u, cmd,
                          payload, (uint16_t)(1u + data_len));
    CHECK(n > 0u);
    return n;
}

static void drain_all(void)
{
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));
}

/* The FIFO is empty: the engine queued nothing this step. */
static void expect_nothing(void)
{
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

/* The FIFO holds exactly `want`, oldest first; then drain it. */
static void expect_bytes(const uint8_t *want, size_t want_len)
{
    size_t got = ecsp_slave_tx_pending(&g_slave);

    CHECK_MEM(want, want_len, g_tx, got);
    drain_all();
}

/* spec 4.1.2: a waiting slave reports 0x01 once per 1000 ms. */
static void test_report_every_period_while_waiting(void)
{
    uint32_t t;
    int reports = 0;

    init_slave();
    /* The first step only latches the send-time reference: the slave has not
     * been silent for a period yet. */
    step_at(0u);
    expect_nothing();

    for (t = 1u; t <= 3u * PERIOD_MS + 500u; t++) {
        step_at(t);
        if (t % PERIOD_MS == 0u) {
            expect_bytes(handshake_report, sizeof handshake_report);
            reports++;
        } else {
            expect_nothing();
        }
    }
    CHECK_EQ(3, reports);
}

/* spec 4.1.3: answering the handshake stops the reports and enters ready. */
static void test_handshake_stops_reports_and_enters_ready(void)
{
    init_slave();
    step_at(0u);
    step_at(PERIOD_MS);
    expect_bytes(handshake_report, sizeof handshake_report);

    /* The host accepts at 1500 ms. The answer refreshes last_send_time, so no
     * periodic frame is due in this same step. */
    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 1500u);
    expect_bytes(handshake_resp, sizeof handshake_resp);

    /* No 0x01 report is ever sent again; the next periodic frame is a 0x10
     * heartbeat one period after the handshake answer. */
    step_at(1500u + PERIOD_MS - 1u);
    expect_nothing();
    step_at(1500u + PERIOD_MS);
    expect_bytes(heartbeat, sizeof heartbeat);
}

/* spec 7.2.2: any queued frame restarts the heartbeat window. */
static void test_any_outbound_frame_restarts_the_window(void)
{
    init_slave();
    step_at(0u);

    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 0u);
    expect_bytes(handshake_resp, sizeof handshake_resp);

    step_at(PERIOD_MS);
    expect_bytes(heartbeat, sizeof heartbeat);

    /* A get-version answer at 1500 ms postpones the next heartbeat by a full
     * period, because a frame did go out. */
    send_at(ECSP_CMD_GET_VERSION, NULL, 0u, 1500u);
    expect_bytes(version_resp, sizeof version_resp);

    step_at(1500u + PERIOD_MS - 1u);
    expect_nothing();
    step_at(1500u + PERIOD_MS);
    expect_bytes(heartbeat, sizeof heartbeat);
}

/* spec 4.1.4: a ready slave answers an unexpected host reset and stays ready. */
static void test_handshake_when_ready_answers_and_stays_ready(void)
{
    init_slave();
    step_at(0u);
    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 0u);
    expect_bytes(handshake_resp, sizeof handshake_resp);

    /* The host restarts and sends a fresh handshake request at 500 ms. */
    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 500u);
    expect_bytes(handshake_resp, sizeof handshake_resp);

    /* It must not fall back to "waiting for handshake": the periodic frames
     * stay 0x10 heartbeats. */
    step_at(500u + PERIOD_MS - 1u);
    expect_nothing();
    step_at(500u + PERIOD_MS);
    expect_bytes(heartbeat, sizeof heartbeat);
    step_at(500u + 2u * PERIOD_MS - 1u);
    expect_nothing();
    step_at(500u + 2u * PERIOD_MS);
    expect_bytes(heartbeat, sizeof heartbeat);
}

/* spec 7.2.2 with a clock that wraps through 0xFFFFFFFF -> 0. */
static void test_wrap_around_neither_skips_nor_bursts(void)
{
    /* -1024 ms: the first period ends just before the wrap and the second one
     * crosses it. */
    const uint32_t base = 0xFFFFFC00u;

    init_slave();
    step_at(base);
    expect_nothing();

    /* First period: ends at 0xFFFFFFE8, still below the wrap. */
    step_at(base + PERIOD_MS - 1u);
    expect_nothing();
    step_at(base + PERIOD_MS);
    expect_bytes(handshake_report, sizeof handshake_report);

    /* Second period: 0xFFFFFFE8 + 1000 wraps to 0x000003D0. */
    step_at(0x000003CFu);
    expect_nothing();
    step_at(0x000003D0u);
    expect_bytes(handshake_report, sizeof handshake_report);

    /* A long forward jump (clock stalled, not frames missed) releases exactly
     * one report, never a burst of them. */
    step_at(0x000003D0u + 10000u);
    expect_bytes(handshake_report, sizeof handshake_report);
    step_at(0x000003D0u + 10000u + 1u);
    expect_nothing();
}

/* A periodic frame that does not fit the FIFO is retried, and a failed attempt
 * must not count as a send (otherwise the heartbeat is lost for a period). */
static void test_periodic_frame_retries_when_fifo_is_full(void)
{
    uint8_t want[2u * ECSP_MAX_FRAME];
    size_t want_len = 0u;
    size_t n;

    /* Smallest legal transmit FIFO: one largest common answer. */
    init_slave_cap(ECSP_FRAME_MIN_LEN + 1u + ECSP_SERIAL_LEN);
    step_at(0u);

    /* Two pre-handshake rejections fill the FIFO and must stay there. */
    send_at(ECSP_CMD_GET_VERSION, NULL, 0u, 0u);
    send_at(ECSP_CMD_GET_HW_VERSION, NULL, 0u, 0u);
    n = answer_frame(want, ECSP_CMD_GET_VERSION, ECSP_RS_NOT_HANDSHAKEN,
                     NULL, 0u);
    want_len += n;
    n = answer_frame(&want[want_len], ECSP_CMD_GET_HW_VERSION,
                     ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
    want_len += n;
    CHECK_MEM(want, want_len, g_tx, ecsp_slave_tx_pending(&g_slave));

    /* The report is due but the FIFO is full: nothing is queued, and the due
     * time is not consumed. */
    step_at(PERIOD_MS);
    CHECK_MEM(want, want_len, g_tx, ecsp_slave_tx_pending(&g_slave));
    step_at(2u * PERIOD_MS);
    CHECK_MEM(want, want_len, g_tx, ecsp_slave_tx_pending(&g_slave));

    /* Once the caller drains, the still-due report goes out on the next step. */
    drain_all();
    step_at(2u * PERIOD_MS + 1u);
    expect_bytes(handshake_report, sizeof handshake_report);
}

/* spec 4.1.2: the waiting-state 0x01 report runs at a fixed 1 Hz "until the
 * handshake succeeds"; answers to a polling host must not starve it. */
static void test_waiting_reports_are_not_starved_by_traffic(void)
{
    uint8_t expect[2u * ECSP_MAX_FRAME];
    size_t reject_len = 0u;
    uint32_t t;
    int reports = 0;

    init_slave();
    step_at(0u);
    expect_nothing();

    for (t = 1u; t <= 3u * PERIOD_MS + 500u; t++) {
        int polled = (t % 500u == 0u) ? 1 : 0;

        if (polled) {
            /* A host that polls an unhandshaken slave every 500 ms gets a 0x10
             * rejection; the step queues that answer first. */
            send_at(ECSP_CMD_GET_VERSION, NULL, 0u, t);
            reject_len = answer_frame(expect, ECSP_CMD_GET_VERSION,
                                      ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
        } else {
            step_at(t);
        }

        if (t % PERIOD_MS == 0u) {
            size_t want_len = (polled != 0) ? reject_len : 0u;

            /* The report is still on its own 1 Hz grid, right after the answer
             * queued in the same step. */
            memcpy(&expect[want_len], handshake_report,
                   sizeof handshake_report);
            expect_bytes(expect, want_len + sizeof handshake_report);
            reports++;
        } else if (polled != 0) {
            expect_bytes(expect, reject_len);
        } else {
            expect_nothing();
        }
    }
    CHECK_EQ(3, reports);
}

int main(void)
{
    test_report_every_period_while_waiting();
    test_handshake_stops_reports_and_enters_ready();
    test_any_outbound_frame_restarts_the_window();
    test_handshake_when_ready_answers_and_stays_ready();
    test_wrap_around_neither_skips_nor_bursts();
    test_periodic_frame_retries_when_fifo_is_full();
    test_waiting_reports_are_not_starved_by_traffic();

    return ecsp_test_report();
}
