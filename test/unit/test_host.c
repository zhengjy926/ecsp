/*
 * L4 host bring-up (ticket #9; spec 4.1.1, 5.2.1, 5.2.2, 6.2, 9.2).
 *
 * Seams, all on the public host API (impl spec section 7):
 *  - transmit FIFO after ecsp_host_step: which request frame went out;
 *  - ecsp_host_session: disconnected / handshaking / handshaken / normal /
 *    version-mismatch;
 *  - ecsp_host_submit: a business command is queued only in normal
 *    communication, and refused with an empty FIFO otherwise;
 *  - the event callback: three silent probes are reported once.
 *
 * Expected frames are the golden vectors (spec 5.2.1, 5.2.2, 5.2.18), not
 * bytes recomputed by the encoder under test. Timeouts use the spec's
 * numbers: 100 ms for a handshake answer (table 7-1 has no special-session
 * row, so the host waits the action-class ceiling) and 50 ms for the
 * get-version query (table 7-1).
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

#define HANDSHAKE_MS 100u
#define QUERY_MS     50u

/* spec 5.2.1 host handshake request, P=0x03 B=0x02. The report frame is the
 * same bytes; only a response carrying 0x00 is an answer. */
static const uint8_t handshake_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
};
static const uint8_t handshake_resp[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0x00, 0x50, 0x55, 0x0D
};
/* spec table 6-1: response 0x01 is a defect report, not a silence. */
static const uint8_t handshake_reject[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0x01, 0x91, 0x95, 0x0D
};

/* spec 5.2.2 get-version request / "1.0.0.0" response. */
static const uint8_t version_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x02, 0x02, 0x88, 0xA0, 0x0D
};
static const uint8_t version_ok[] = {
    0xFA, 0x11, 0x00, 0x03, 0x02, 0x02, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30,
    0xFA, 0x0E, 0x0D
};

/* Same frame as version_ok but the last version digit is '1' not '0'
 * (spec 9.2: one unequal byte is enough to refuse). CRC16-MODBUS over
 * 11 00 03 02 02 00 31 2E 30 2E 30 2E 31 is 0xCE3B. */
/* spec 5.2.2 / table 6-1: 0x02 is a length error. Do not retry it as silence. */
static const uint8_t version_reject[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x02, 0x02, 0x02, 0x21, 0x94, 0x0D
};

static const uint8_t version_bad[] = {
    0xFA, 0x11, 0x00, 0x03, 0x02, 0x02, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x31,
    0x3B, 0xCE, 0x0D
};

/* spec 5.2.18 fault query: a business command, empty request payload. */
static const uint8_t fault_query_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x12, 0x02, 0x85, 0x60, 0x0D
};

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static uint8_t g_tx_one_frame[ECSP_FRAME_MIN_LEN];
static uint8_t *g_tx_use;
static size_t g_tx_cap;
static ecsp_host_t g_host;
static ecsp_host_slave_cfg_t g_slaves[1];

static int g_events;
static int g_event;
static uint8_t g_event_board;

static void on_event(ecsp_host_t *h, uint8_t board_addr, ecsp_host_event_t event,
                     void *user)
{
    (void)h;
    (void)user;
    g_events++;
    g_event = (int)event;
    g_event_board = board_addr;
}

static void init_host(ecsp_host_event_cb_t on_event_fn, void *user)
{
    ecsp_host_cfg_t cfg;
    static const uint8_t expect[7] = { '1', '.', '0', '.', '0', '.', '0' };

    memcpy(g_slaves[0].expect_version.b, expect, 7u);
    g_slaves[0].board_addr = 0x02u;

    cfg.product_addr = 0x03u;
    cfg.slaves = g_slaves;
    cfg.slave_count = 1u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = (g_tx_use != NULL) ? g_tx_use : g_tx;
    cfg.tx_cap = (g_tx_use != NULL) ? g_tx_cap : sizeof g_tx;
    cfg.on_event = on_event_fn;
    cfg.user = user;
    CHECK_EQ(0, ecsp_host_init(&g_host, &cfg));
}

static void step_at(uint32_t now_ms)
{
    ecsp_host_step(&g_host, now_ms);
}

static void drain_all(void)
{
    ecsp_host_tx_drain(&g_host, ecsp_host_tx_pending(&g_host));
}

static void expect_nothing(void)
{
    CHECK_EQ(0u, ecsp_host_tx_pending(&g_host));
}

static void expect_bytes(const uint8_t *want, size_t want_len)
{
    size_t got = ecsp_host_tx_pending(&g_host);

    CHECK_MEM(want, want_len, g_tx, got);
    drain_all();
}

/* spec 4.1.1 step 2: power-up actively sends one handshake request. */
static void test_first_step_sends_handshake_request(void)
{
    init_host(NULL, NULL);
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));

    step_at(0u);
    expect_bytes(handshake_req, sizeof handshake_req);
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));

    /* Not yet timed out: a second step must not emit another probe. */
    step_at(HANDSHAKE_MS - 1u);
    expect_nothing();
}

/* spec 4.1.1 / 6.2: one probe, then retries, three sends in all. Silence
 * after the third returns the session to disconnected and reports once. */
static void test_three_silent_probes_report_failure(void)
{
    g_events = 0;
    init_host(on_event, NULL);

    step_at(0u);
    expect_bytes(handshake_req, sizeof handshake_req);

    step_at(HANDSHAKE_MS);
    expect_bytes(handshake_req, sizeof handshake_req);
    CHECK_EQ(0, g_events);
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));

    step_at(2u * HANDSHAKE_MS);
    expect_bytes(handshake_req, sizeof handshake_req);
    CHECK_EQ(0, g_events);

    step_at(3u * HANDSHAKE_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_PROBE_FAILED, g_event);
    CHECK_EQ(0x02, g_event_board);

    /* Does not probe again and does not report again. */
    step_at(3u * HANDSHAKE_MS + 5u * HANDSHAKE_MS);
    expect_nothing();
    CHECK_EQ(1, g_events);
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));
}

/* spec 4.1.1 step 3: a 0x00 handshake answer moves to handshaken and the
 * next thing on the wire is get-version. A handshake report (no response
 * code) is not that answer. */
static void test_handshake_ok_sends_version_request(void)
{
    init_host(NULL, NULL);
    step_at(0u);
    drain_all();

    ecsp_host_rx(&g_host, handshake_req, sizeof handshake_req);
    step_at(10u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));

    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    step_at(10u);
    expect_bytes(version_req, sizeof version_req);
    CHECK_EQ(ECSP_SESS_HANDSHAKEN, ecsp_host_session(&g_host, 0x02u));
}

/* Drive the host to "handshaken, version request already on the wire". */
static void reach_version_wait(void)
{
    init_host(on_event, NULL);
    step_at(0u);
    drain_all();
    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    step_at(10u);
    expect_bytes(version_req, sizeof version_req);
    CHECK_EQ(ECSP_SESS_HANDSHAKEN, ecsp_host_session(&g_host, 0x02u));
}

/* spec 9.2: equal 7 bytes -> normal communication, and a business command
 * may then be submitted. Submit only enqueues; the next step emits it. */
static void test_matching_version_allows_business_command(void)
{
    g_events = 0;
    reach_version_wait();

    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(0, g_events);

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, NULL, NULL));
    expect_nothing();
    step_at(20u);
    expect_bytes(fault_query_req, sizeof fault_query_req);
}

/* spec 9.2: an unequal version stays at version-mismatch, alarms once, and
 * a later business submit is refused without a frame leaving the host. */
static void test_version_mismatch_refuses_business_command(void)
{
    g_events = 0;
    reach_version_wait();

    ecsp_host_rx(&g_host, version_bad, sizeof version_bad);
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_VERSION_MISMATCH, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_VERSION_MISMATCH, g_event);
    CHECK_EQ(0x02, g_event_board);

    CHECK_EQ(ECSP_E_STATE,
             ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, NULL, NULL));
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_VERSION_MISMATCH, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
}

/* The 100 ms handshake window is measured with a signed difference, so a
 * now_ms wrap must neither retry early nor skip the retry (impl spec
 * section 3). */
static void test_handshake_retry_survives_clock_wrap(void)
{
    uint32_t t0 = 0xFFFFFFF0u;

    init_host(NULL, NULL);
    step_at(t0);
    expect_bytes(handshake_req, sizeof handshake_req);

    step_at(t0 + (HANDSHAKE_MS - 1u));
    expect_nothing();
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));

    step_at(t0 + HANDSHAKE_MS);
    expect_bytes(handshake_req, sizeof handshake_req);
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));
}

/* The version read is a normal request (spec 6.2): three unanswered sends,
 * then the session leaves "handshaken" instead of waiting forever. Bring-up
 * never reached normal communication, so this is the same failure report as
 * a silent probe, not the offline state. */
static void test_silent_version_read_reports_failure(void)
{
    g_events = 0;
    reach_version_wait();

    step_at(10u + QUERY_MS);
    expect_bytes(version_req, sizeof version_req);
    CHECK_EQ(0, g_events);

    step_at(10u + 2u * QUERY_MS);
    expect_bytes(version_req, sizeof version_req);

    step_at(10u + 3u * QUERY_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_PROBE_FAILED, g_event);

    CHECK_EQ(ECSP_E_STATE,
             ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, NULL, NULL));
    step_at(10u + 3u * QUERY_MS + QUERY_MS);
    expect_nothing();
    CHECK_EQ(1, g_events);
}

/* spec table 6-1: 0x01 is reported and not retried. It is not "no answer". */
static void test_rejected_handshake_is_not_retried_as_silence(void)
{
    g_events = 0;
    init_host(on_event, NULL);
    step_at(0u);
    drain_all();

    ecsp_host_rx(&g_host, handshake_reject, sizeof handshake_reject);
    step_at(10u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_REJECTED, g_event);

    step_at(10u + HANDSHAKE_MS);
    expect_nothing();
    CHECK_EQ(1, g_events);
}

/* spec table 6-1: a 0x02 version answer is a defect, not three more queries. */
static void test_rejected_version_is_not_retried_as_silence(void)
{
    g_events = 0;
    reach_version_wait();

    ecsp_host_rx(&g_host, version_reject, sizeof version_reject);
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_REJECTED, g_event);
    CHECK_EQ(ECSP_E_STATE,
             ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, NULL, NULL));
}

/* spec 6.2: a frame that cannot be handed to the link is not "no answer".
 * The session must leave handshaking and say so. */
static void test_full_fifo_reports_link_fault(void)
{
    g_events = 0;
    g_tx_use = g_tx_one_frame;
    g_tx_cap = sizeof g_tx_one_frame;
    init_host(on_event, NULL);
    g_tx_use = NULL;
    g_tx_cap = 0u;

    step_at(0u);
    CHECK_EQ(sizeof handshake_req, ecsp_host_tx_pending(&g_host));
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));

    step_at(HANDSHAKE_MS);
    CHECK_EQ(sizeof handshake_req, ecsp_host_tx_pending(&g_host));
    CHECK_MEM(handshake_req, sizeof handshake_req, g_tx_one_frame,
              sizeof handshake_req);
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_LINK_FAULT, g_event);
}

int main(void)
{
    test_first_step_sends_handshake_request();
    test_three_silent_probes_report_failure();
    test_handshake_retry_survives_clock_wrap();
    test_handshake_ok_sends_version_request();
    test_matching_version_allows_business_command();
    test_version_mismatch_refuses_business_command();
    test_silent_version_read_reports_failure();
    test_rejected_handshake_is_not_retried_as_silence();
    test_rejected_version_is_not_retried_as_silence();
    test_full_fifo_reports_link_fault();
    return ecsp_test_report();
}
