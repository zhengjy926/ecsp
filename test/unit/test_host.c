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
 * Ticket #11, still on that public host API:
 *  - the transaction callback has run when ecsp_host_step returns, once
 *    for an answer (timeout and dropout are already asserted below);
 *  - two submits from that callback leave on the next step, in the order
 *    they were submitted, and not on the step that delivered the callback;
 *  - ecsp_host_step from that callback returns ECSP_E_REENT, and the waiting
 *    transaction still has its original retries left;
 *  - a second host context keeps its own FIFO and can be stepped from the
 *    first host's callback.
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
#define ACTION_MS    100u
#define IDENTITY_MS  3000u /* spec 7.1: 0x03 / 0x05 first answer */
#define DROPOUT_MS   3000u
#define SETUP_MS     5000u

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
/* spec 5.2.18 answer, board 0x02: response 0x00 and fault word 0x01.
 * CRC16-MODBUS over 0B 00 03 12 02 00 01 is 0xB880. */
static const uint8_t fault_ans[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x12, 0x02, 0x00, 0x01, 0x80, 0xB8, 0x0D
};
/* spec 5.2.18 request, board 0x03. CRC16-MODBUS over 09 00 03 12 03 is 0xA044. */
static const uint8_t fault_req_b3[] = {
    0xFA, 0x09, 0x00, 0x03, 0x12, 0x03, 0x44, 0xA0, 0x0D
};
/* Same answer as fault_ans, addressed to board 0x03.
 * CRC16-MODBUS over 0B 00 03 12 03 00 01 is 0x78D1. */
static const uint8_t fault_ans_b3[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x12, 0x03, 0x00, 0x01, 0xD1, 0x78, 0x0D
};

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static uint8_t g_tx_one_frame[ECSP_FRAME_MIN_LEN];
static uint8_t *g_tx_use;
static size_t g_tx_cap;
static ecsp_host_t g_host;
static ecsp_host_slave_cfg_t g_slaves[2];

static int g_events;
static int g_event;
static uint8_t g_event_board;

static int g_txn_calls;
static int g_txn_status;
static uint8_t g_txn_cmd;
static uint8_t g_txn_rs;
static uint16_t g_txn_len;
static uint8_t g_txn_byte;
static int g_txn_user_got;
static int g_txn_marker;
static int g_chain;
static int g_reenter_rc;
static int g_peer_rc;
static int g_snap_once;
static ecsp_session_t g_snap_sess;
static ecsp_txn_state_t g_snap_txn;
static size_t g_snap_tx;
static ecsp_host_t *g_peer;

/* spec 5.2.7 reset request, transaction sequence 0x01. Retries must be
 * these same bytes (spec 4.2: a retransmission reuses the sequence). */
static const uint8_t reset_req[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x07, 0x02, 0x01, 0x71, 0x94, 0x0D
};
static const uint8_t reset_seq[] = { 0x01u };

/* spec 5.2.3 set hardware version "1.0.0.0", and the 0x00 / 0x01 answers. */
static const uint8_t hw_version[7] = {
    '1', '.', '0', '.', '0', '.', '0'
};
static const uint8_t hw_set_req[] = {
    0xFA, 0x10, 0x00, 0x03, 0x03, 0x02,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30,
    0xB2, 0xC1, 0x0D
};
static const uint8_t hw_set_ok[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x03, 0x02, 0x00, 0xF1, 0x95, 0x0D
};
static const uint8_t hw_set_fail[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x03, 0x02, 0x01, 0x30, 0x55, 0x0D
};

/* spec 5.2.5 set serial number "SN0000000000". */
static const uint8_t serial_number[12] = {
    'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
};
static const uint8_t sn_set_req[] = {
    0xFA, 0x15, 0x00, 0x03, 0x05, 0x02,
    0x53, 0x4E, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0xB3, 0x49, 0x0D
};

/* spec 5.2.11 set low-power mode, enter (0x01). */
static const uint8_t low_power_mode[1] = { 0x01u };
static const uint8_t low_power_req[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0B, 0x02, 0x01, 0xB1, 0x97, 0x0D
};

/* spec 5.2.16 heartbeat, used as a valid frame that is not a command answer. */
static const uint8_t heartbeat[] = {
    0xFA, 0x09, 0x00, 0x03, 0x10, 0x02, 0x84, 0x00, 0x0D
};

/* Second slave, P=0x03 B=0x03. CRC16-MODBUS over the length..payload bytes. */
static const uint8_t hs_req_b3[] = {
    0xFA, 0x09, 0x00, 0x03, 0x01, 0x03, 0x49, 0x90, 0x0D
};
static const uint8_t hs_resp_b3[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x01, 0x03, 0x00, 0x51, 0xC5, 0x0D
};
static const uint8_t ver_req_b3[] = {
    0xFA, 0x09, 0x00, 0x03, 0x02, 0x03, 0x49, 0x60, 0x0D
};
static const uint8_t ver_resp_b3[] = {
    0xFA, 0x11, 0x00, 0x03, 0x02, 0x03, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30, 0xF7, 0x9E, 0x0D
};

static void on_txn(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                   int status, uint8_t response_code, const uint8_t *payload,
                   uint16_t len, void *user)
{
    (void)h;
    (void)board_addr;
    (void)response_code;
    (void)payload;
    (void)len;
    g_txn_calls++;
    g_txn_status = status;
    g_txn_cmd = cmd;
    g_txn_rs = response_code;
    g_txn_len = len;
    g_txn_byte = (payload != NULL && len > 0u) ? payload[0] : 0u;
    g_txn_user_got = (user == &g_txn_marker) ? 1 : 0;
}

/* The completing transaction's callback submits board 0x03, then board 0x02.
 * Slot order is the other way round, so a FIFO that follows the slot array
 * fails this. */
static void on_txn_chain(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                         int status, uint8_t response_code,
                         const uint8_t *payload, uint16_t len, void *user)
{
    (void)board_addr;
    (void)cmd;
    (void)response_code;
    (void)payload;
    (void)len;
    (void)user;
    g_txn_calls++;
    g_txn_status = status;
    if (g_chain == 0) {
        return;
    }
    g_chain = 0;
    CHECK_EQ(ECSP_OK, ecsp_host_submit(h, 0x03u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, NULL, NULL));
    CHECK_EQ(ECSP_OK, ecsp_host_submit(h, 0x02u, ECSP_CMD_SYSTEM_RESET,
                                      reset_seq, 1u, NULL, NULL));
}

/* now 200 is the first retry instant of a reset sent at 100. A step that
 * actually ran would put that retry on the FIFO. */
static void on_txn_reenter(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                           int status, uint8_t response_code,
                           const uint8_t *payload, uint16_t len, void *user)
{
    (void)board_addr;
    (void)cmd;
    (void)status;
    (void)response_code;
    (void)payload;
    (void)len;
    (void)user;
    g_txn_calls++;
    if (g_snap_once != 0) {
        return;
    }
    g_snap_once = 1;
    g_reenter_rc = ecsp_host_step(h, 200u);
    g_snap_sess = ecsp_host_session(h, 0x02u);
    g_snap_txn = ecsp_host_txn(h, 0x02u);
    g_snap_tx = ecsp_host_tx_pending(h);
}

static void on_txn_peer(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                        int status, uint8_t response_code,
                        const uint8_t *payload, uint16_t len, void *user)
{
    (void)board_addr;
    (void)cmd;
    (void)status;
    (void)response_code;
    (void)payload;
    (void)len;
    (void)user;
    g_txn_calls++;
    if (g_snap_once != 0) {
        return;
    }
    g_snap_once = 1;
    g_peer_rc = ecsp_host_step(g_peer, 0u);
    g_snap_sess = ecsp_host_session(h, 0x02u);
    g_snap_txn = ecsp_host_txn(h, 0x02u);
    g_snap_tx = ecsp_host_tx_pending(h);
}

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
    CHECK_EQ(ECSP_OK, ecsp_host_step(&g_host, now_ms));
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

static void reach_normal(void)
{
    reach_version_wait();
    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
}

/* spec table 4-2 / 4.2: while the reset is in flight the session stays
 * "normal communication". Three identical transmissions (same sequence
 * byte), then the session drops and the callback runs once. */
static void test_inflight_txn_keeps_session_and_retries_same_sequence(void)
{
    g_txn_calls = 0;
    g_txn_user_got = 0;
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SYSTEM_RESET,
                                      reset_seq, 1u, on_txn, &g_txn_marker));
    CHECK_EQ(ECSP_TXN_QUEUED, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    expect_nothing();

    step_at(20u);
    expect_bytes(reset_req, sizeof reset_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(0, g_txn_calls);

    step_at(20u + ACTION_MS);
    expect_bytes(reset_req, sizeof reset_req);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    step_at(20u + 2u * ACTION_MS);
    expect_bytes(reset_req, sizeof reset_req);

    step_at(20u + 3u * ACTION_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_OFFLINE, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_E_TIMEOUT, g_txn_status);
    CHECK_EQ(ECSP_CMD_SYSTEM_RESET, g_txn_cmd);
    CHECK_EQ(1, g_txn_user_got);

    step_at(20u + 4u * ACTION_MS);
    expect_nothing();
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_E_STATE,
             ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, NULL, NULL));
}

/* spec 7.2.1: three heartbeat periods with no valid frame drop the session.
 * An in-flight transaction is cancelled once, not also retried. */
static void test_heartbeat_silence_drops_session_and_cancels_txn_once(void)
{
    g_txn_calls = 0;
    g_events = 0;
    reach_normal();
    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SYSTEM_RESET,
                                      reset_seq, 1u, on_txn, &g_txn_marker));
    step_at(20u);
    drain_all();

    step_at(20u + DROPOUT_MS - 1u);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    /* That step is also past the answer deadline, so it may have queued one
     * retransmission. Dropout is about silence, not about that retry. */
    drain_all();

    step_at(20u + DROPOUT_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_OFFLINE, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_E_OFFLINE, g_txn_status);
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_OFFLINE, g_event);

    step_at(20u + DROPOUT_MS + DROPOUT_MS);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(1, g_events);
}

/* spec 4.1 / 7.2.1: after dropout a business command is refused until a
 * handshake report is accepted and the version matches again. */
static void test_dropout_recovery_requires_a_new_handshake(void)
{
    reach_normal();
    step_at(20u + DROPOUT_MS);
    CHECK_EQ(ECSP_SESS_OFFLINE, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_E_STATE,
             ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, NULL, NULL));
    expect_nothing();

    ecsp_host_rx(&g_host, handshake_req, sizeof handshake_req);
    step_at(20u + DROPOUT_MS);
    expect_bytes(handshake_req, sizeof handshake_req);
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));

    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    step_at(20u + DROPOUT_MS + 10u);
    expect_bytes(version_req, sizeof version_req);
    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    step_at(20u + DROPOUT_MS + 20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, NULL, NULL));
    step_at(20u + DROPOUT_MS + 20u);
    expect_bytes(fault_query_req, sizeof fault_query_req);
}

/* spec 4.1.1: three probes with no answer enter passive wait. A later
 * handshake report, still inside the 5 s window, is probed again and can
 * reach normal communication. */
static void test_late_slave_report_restarts_the_probe(void)
{
    g_events = 0;
    init_host(on_event, NULL);
    step_at(0u);
    drain_all();
    step_at(HANDSHAKE_MS);
    drain_all();
    step_at(2u * HANDSHAKE_MS);
    drain_all();
    step_at(3u * HANDSHAKE_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_PROBE_FAILED, g_event);

    step_at(1000u);
    expect_nothing();

    ecsp_host_rx(&g_host, handshake_req, sizeof handshake_req);
    step_at(1000u);
    expect_bytes(handshake_req, sizeof handshake_req);
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));

    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    step_at(1010u);
    expect_bytes(version_req, sizeof version_req);
    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    step_at(1020u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
}

/* spec 4.1.1: passive wait that outlasts the session-establishment window
 * is dropout, and it is reported. */
static void test_passive_wait_past_setup_window_goes_offline(void)
{
    g_events = 0;
    init_host(on_event, NULL);
    step_at(0u);
    drain_all();
    step_at(HANDSHAKE_MS);
    drain_all();
    step_at(2u * HANDSHAKE_MS);
    drain_all();
    step_at(3u * HANDSHAKE_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_DISCONNECTED, ecsp_host_session(&g_host, 0x02u));

    g_events = 0;
    step_at(SETUP_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_OFFLINE, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_events);
    CHECK_EQ((int)ECSP_HOST_EV_OFFLINE, g_event);
    CHECK_EQ(ECSP_E_STATE,
             ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, NULL, NULL));
}

static void init_two(void)
{
    ecsp_host_cfg_t cfg;
    static const uint8_t expect[7] = { '1', '.', '0', '.', '0', '.', '0' };

    memcpy(g_slaves[0].expect_version.b, expect, 7u);
    memcpy(g_slaves[1].expect_version.b, expect, 7u);
    g_slaves[0].board_addr = 0x02u;
    g_slaves[1].board_addr = 0x03u;

    cfg.product_addr = 0x03u;
    cfg.slaves = g_slaves;
    cfg.slave_count = 2u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    cfg.on_event = on_event;
    cfg.user = NULL;
    CHECK_EQ(0, ecsp_host_init(&g_host, &cfg));
}

/* spec 4.1, "各板卡地址的两条状态轴相互独立": one slave dropping does not
 * stop the other from accepting a business command. */
static void test_one_slave_dropping_leaves_the_other_usable(void)
{
    uint8_t both_hs[sizeof handshake_req + sizeof hs_req_b3];
    uint8_t both_ver[sizeof version_req + sizeof ver_req_b3];

    g_events = 0;
    init_two();
    memcpy(both_hs, handshake_req, sizeof handshake_req);
    memcpy(both_hs + sizeof handshake_req, hs_req_b3, sizeof hs_req_b3);
    memcpy(both_ver, version_req, sizeof version_req);
    memcpy(both_ver + sizeof version_req, ver_req_b3, sizeof ver_req_b3);

    step_at(0u);
    expect_bytes(both_hs, sizeof both_hs);
    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    ecsp_host_rx(&g_host, hs_resp_b3, sizeof hs_resp_b3);
    step_at(10u);
    expect_bytes(both_ver, sizeof both_ver);
    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    ecsp_host_rx(&g_host, ver_resp_b3, sizeof ver_resp_b3);
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x03u));

    ecsp_host_rx(&g_host, heartbeat, sizeof heartbeat);
    step_at(20u + DROPOUT_MS - 100u);
    step_at(20u + DROPOUT_MS);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_OFFLINE, ecsp_host_session(&g_host, 0x03u));
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x03u));

    CHECK_EQ(ECSP_E_STATE,
             ecsp_host_submit(&g_host, 0x03u, ECSP_CMD_GET_FAULT,
                              NULL, 0u, NULL, NULL));
    expect_nothing();
    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, NULL, NULL));
    step_at(20u + DROPOUT_MS);
    expect_bytes(fault_query_req, sizeof fault_query_req);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
}

static void reach_two_normal(void)
{
    uint8_t both_hs[sizeof handshake_req + sizeof hs_req_b3];
    uint8_t both_ver[sizeof version_req + sizeof ver_req_b3];

    init_two();
    memcpy(both_hs, handshake_req, sizeof handshake_req);
    memcpy(both_hs + sizeof handshake_req, hs_req_b3, sizeof hs_req_b3);
    memcpy(both_ver, version_req, sizeof version_req);
    memcpy(both_ver + sizeof version_req, ver_req_b3, sizeof ver_req_b3);

    step_at(0u);
    expect_bytes(both_hs, sizeof both_hs);
    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    ecsp_host_rx(&g_host, hs_resp_b3, sizeof hs_resp_b3);
    step_at(10u);
    expect_bytes(both_ver, sizeof both_ver);
    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    ecsp_host_rx(&g_host, ver_resp_b3, sizeof ver_resp_b3);
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x03u));
}

/* An answer runs the callback before step returns, once, with the fault
 * byte and without the response code. A later step does not call it again.
 * Timeout and dropout already assert the other two once-only endings. */
static void test_answer_invokes_callback_once_before_step_returns(void)
{
    g_txn_calls = 0;
    g_txn_user_got = 0;
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, on_txn, &g_txn_marker));
    step_at(30u);
    expect_bytes(fault_query_req, sizeof fault_query_req);
    CHECK_EQ(0, g_txn_calls);

    ecsp_host_rx(&g_host, fault_ans, sizeof fault_ans);
    step_at(40u);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_OK, g_txn_status);
    CHECK_EQ(ECSP_CMD_GET_FAULT, g_txn_cmd);
    CHECK_EQ(0x00, g_txn_rs);
    CHECK_EQ(1, g_txn_len);
    CHECK_EQ(0x01, g_txn_byte);
    CHECK_EQ(1, g_txn_user_got);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    expect_nothing();

    step_at(50u);
    CHECK_EQ(1, g_txn_calls);
}

/* Two submits inside the callback stay queued through that step. The next
 * step emits board 0x03's query and then board 0x02's reset: submit order,
 * not slot-array order. */
static void test_callback_submits_leave_in_submit_order(void)
{
    uint8_t want[sizeof fault_req_b3 + sizeof reset_req];

    g_txn_calls = 0;
    g_chain = 0;
    reach_two_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, on_txn_chain, NULL));
    step_at(100u);
    expect_bytes(fault_query_req, sizeof fault_query_req);

    g_chain = 1;
    ecsp_host_rx(&g_host, fault_ans, sizeof fault_ans);
    step_at(110u);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_OK, g_txn_status);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_QUEUED, ecsp_host_txn(&g_host, 0x03u));
    CHECK_EQ(ECSP_TXN_QUEUED, ecsp_host_txn(&g_host, 0x02u));

    memcpy(want, fault_req_b3, sizeof fault_req_b3);
    memcpy(want + sizeof fault_req_b3, reset_req, sizeof reset_req);
    step_at(110u);
    expect_bytes(want, sizeof want);
}

/* Stepping the same host from the callback is refused. The reset that is
 * already waiting keeps the retries it had: the original deadline still
 * produces the second and third copies, and only then times out once. */
static void test_reentering_step_does_not_spend_a_retry(void)
{
    g_txn_calls = 0;
    g_snap_once = 0;
    g_reenter_rc = 0;
    reach_two_normal();

    CHECK_EQ(ECSP_E_ARG, ecsp_host_step(NULL, 0u));

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SYSTEM_RESET,
                                      reset_seq, 1u, on_txn, &g_txn_marker));
    step_at(100u);
    expect_bytes(reset_req, sizeof reset_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x03u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, on_txn_reenter, NULL));
    step_at(100u);
    expect_bytes(fault_req_b3, sizeof fault_req_b3);

    g_txn_calls = 0;
    ecsp_host_rx(&g_host, fault_ans_b3, sizeof fault_ans_b3);
    step_at(110u);
    CHECK_EQ(ECSP_E_REENT, g_reenter_rc);
    CHECK_EQ(ECSP_SESS_NORMAL, g_snap_sess);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, g_snap_txn);
    CHECK_EQ(0u, g_snap_tx);
    CHECK_EQ(1, g_txn_calls);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    step_at(200u);
    expect_bytes(reset_req, sizeof reset_req);
    step_at(300u);
    expect_bytes(reset_req, sizeof reset_req);
    CHECK_EQ(1, g_txn_calls);

    step_at(400u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_OFFLINE, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(2, g_txn_calls);
    CHECK_EQ(ECSP_E_TIMEOUT, g_txn_status);
    CHECK_EQ(1, g_txn_user_got);
}

/* The reentrancy flag belongs to one context. Stepping a second host from
 * the callback brings that host up and does not touch the first host's
 * in-flight reset. */
static void test_callback_steps_a_different_host(void)
{
    ecsp_host_t peer;
    ecsp_host_slave_cfg_t slave;
    ecsp_host_cfg_t cfg;
    uint8_t rx[ECSP_MAX_FRAME];
    uint8_t tx[32];
    static const uint8_t expect[7] = { '1', '.', '0', '.', '0', '.', '0' };

    g_txn_calls = 0;
    g_snap_once = 0;
    g_peer_rc = 0;
    reach_two_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SYSTEM_RESET,
                                      reset_seq, 1u, on_txn, NULL));
    step_at(100u);
    expect_bytes(reset_req, sizeof reset_req);

    memcpy(slave.expect_version.b, expect, 7u);
    slave.board_addr = 0x02u;
    cfg.product_addr = 0x03u;
    cfg.slaves = &slave;
    cfg.slave_count = 1u;
    cfg.rx_buf = rx;
    cfg.rx_cap = sizeof rx;
    cfg.tx_buf = tx;
    cfg.tx_cap = sizeof tx;
    cfg.on_event = NULL;
    cfg.user = NULL;
    CHECK_EQ(ECSP_OK, ecsp_host_init(&peer, &cfg));
    g_peer = &peer;

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x03u, ECSP_CMD_GET_FAULT,
                                      NULL, 0u, on_txn_peer, NULL));
    step_at(100u);
    expect_bytes(fault_req_b3, sizeof fault_req_b3);

    ecsp_host_rx(&g_host, fault_ans_b3, sizeof fault_ans_b3);
    step_at(110u);
    CHECK_EQ(ECSP_OK, g_peer_rc);
    CHECK_EQ(1, g_txn_calls);
    CHECK_MEM(handshake_req, sizeof handshake_req, tx,
              ecsp_host_tx_pending(&peer));
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&peer, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, g_snap_sess);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, g_snap_txn);
    CHECK_EQ(0u, g_snap_tx);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    g_peer = NULL;
}

/* Two contexts in one process: stepping one does not write the other's
 * FIFO or move its session. */
static void test_two_hosts_keep_separate_fifos(void)
{
    ecsp_host_t a;
    ecsp_host_t b;
    ecsp_host_slave_cfg_t sa;
    ecsp_host_slave_cfg_t sb;
    ecsp_host_cfg_t ca;
    ecsp_host_cfg_t cb;
    uint8_t rx_a[ECSP_MAX_FRAME];
    uint8_t tx_a[32];
    uint8_t rx_b[ECSP_MAX_FRAME];
    uint8_t tx_b[32];
    static const uint8_t expect[7] = { '1', '.', '0', '.', '0', '.', '0' };

    memcpy(sa.expect_version.b, expect, 7u);
    memcpy(sb.expect_version.b, expect, 7u);
    sa.board_addr = 0x02u;
    sb.board_addr = 0x02u;

    ca.product_addr = 0x03u;
    ca.slaves = &sa;
    ca.slave_count = 1u;
    ca.rx_buf = rx_a;
    ca.rx_cap = sizeof rx_a;
    ca.tx_buf = tx_a;
    ca.tx_cap = sizeof tx_a;
    ca.on_event = NULL;
    ca.user = NULL;

    cb.product_addr = 0x03u;
    cb.slaves = &sb;
    cb.slave_count = 1u;
    cb.rx_buf = rx_b;
    cb.rx_cap = sizeof rx_b;
    cb.tx_buf = tx_b;
    cb.tx_cap = sizeof tx_b;
    cb.on_event = NULL;
    cb.user = NULL;

    CHECK_EQ(ECSP_OK, ecsp_host_init(&a, &ca));
    CHECK_EQ(ECSP_OK, ecsp_host_init(&b, &cb));
    CHECK_EQ(ECSP_OK, ecsp_host_step(&a, 0u));
    CHECK_MEM(handshake_req, sizeof handshake_req, tx_a, ecsp_host_tx_pending(&a));
    CHECK_EQ(0u, ecsp_host_tx_pending(&b));

    CHECK_EQ(ECSP_OK, ecsp_host_step(&b, 0u));
    CHECK_MEM(handshake_req, sizeof handshake_req, tx_a, ecsp_host_tx_pending(&a));
    CHECK_MEM(handshake_req, sizeof handshake_req, tx_b, ecsp_host_tx_pending(&b));
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&a, 0x02u));
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&b, 0x02u));
}

/* spec 7.1: an identity answer inside the old 100 ms ceiling ends the
 * transaction at once. The host does not sit out the 3 s window. */
static void test_identity_answer_within_100ms_ends_immediately(void)
{
    g_txn_calls = 0;
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SET_HW_VERSION,
                                      hw_version, sizeof hw_version,
                                      on_txn, &g_txn_marker));
    step_at(20u);
    expect_bytes(hw_set_req, sizeof hw_set_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(0, g_txn_calls);

    ecsp_host_rx(&g_host, hw_set_ok, sizeof hw_set_ok);
    step_at(20u + ACTION_MS);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_OK, g_txn_status);
    CHECK_EQ(ECSP_CMD_SET_HW_VERSION, g_txn_cmd);
    CHECK_EQ(0x00, g_txn_rs);
    CHECK_EQ(0u, g_txn_len);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    expect_nothing();

    step_at(20u + IDENTITY_MS);
    expect_nothing();
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
}

/* spec 7.1: 100 ms of silence is not a retry. An answer at 2 s still
 * succeeds, and the transaction ends on that step. */
static void test_identity_answer_at_2s_still_succeeds(void)
{
    g_txn_calls = 0;
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SET_HW_VERSION,
                                      hw_version, sizeof hw_version,
                                      on_txn, &g_txn_marker));
    step_at(20u);
    expect_bytes(hw_set_req, sizeof hw_set_req);

    step_at(20u + ACTION_MS);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(0, g_txn_calls);

    step_at(20u + 2000u - 1u);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    ecsp_host_rx(&g_host, hw_set_ok, sizeof hw_set_ok);
    step_at(20u + 2000u);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_OK, g_txn_status);
    CHECK_EQ(0x00, g_txn_rs);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    expect_nothing();

    step_at(20u + 2000u + 10u);
    expect_nothing();
    CHECK_EQ(1, g_txn_calls);
}

/* spec 6.2 / 7.1: three silent waits of 3 s, then one timeout. Dropout
 * does not end the transaction on the first wait. */
static void test_identity_silence_times_out_after_three_waits(void)
{
    g_txn_calls = 0;
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SET_HW_VERSION,
                                      hw_version, sizeof hw_version,
                                      on_txn, &g_txn_marker));
    step_at(20u);
    expect_bytes(hw_set_req, sizeof hw_set_req);

    step_at(20u + IDENTITY_MS - 1u);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));

    step_at(20u + IDENTITY_MS);
    expect_bytes(hw_set_req, sizeof hw_set_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(0, g_txn_calls);

    step_at(20u + 2u * IDENTITY_MS);
    expect_bytes(hw_set_req, sizeof hw_set_req);

    step_at(20u + 3u * IDENTITY_MS - 1u);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    step_at(20u + 3u * IDENTITY_MS);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_OFFLINE, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_E_TIMEOUT, g_txn_status);
    CHECK_EQ(ECSP_CMD_SET_HW_VERSION, g_txn_cmd);

    step_at(20u + 4u * IDENTITY_MS);
    expect_nothing();
    CHECK_EQ(1, g_txn_calls);
}

/* spec table 6-1: a 0x01 answer ends the identity transaction. It is not
 * silence, so the host does not send the request again. */
static void test_identity_reject_is_not_retried(void)
{
    g_txn_calls = 0;
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SET_HW_VERSION,
                                      hw_version, sizeof hw_version,
                                      on_txn, &g_txn_marker));
    step_at(20u);
    expect_bytes(hw_set_req, sizeof hw_set_req);

    ecsp_host_rx(&g_host, hw_set_fail, sizeof hw_set_fail);
    step_at(20u + 50u);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_OK, g_txn_status);
    CHECK_EQ(0x01, g_txn_rs);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));

    step_at(20u + IDENTITY_MS);
    expect_nothing();
    CHECK_EQ(1, g_txn_calls);
}

/* spec 7.1: a set command that is not a non-volatile write still uses
 * the 100 ms ceiling. */
static void test_low_power_still_retries_at_100ms(void)
{
    g_txn_calls = 0;
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SET_LOW_POWER,
                                      low_power_mode, sizeof low_power_mode,
                                      on_txn, &g_txn_marker));
    step_at(20u);
    expect_bytes(low_power_req, sizeof low_power_req);

    step_at(20u + ACTION_MS - 1u);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    step_at(20u + ACTION_MS);
    expect_bytes(low_power_req, sizeof low_power_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(0, g_txn_calls);
}

/* spec 7.1: setting the serial number waits the same 3 s, not 100 ms. */
static void test_serial_write_waits_3s_not_100ms(void)
{
    reach_normal();

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SET_SERIAL,
                                      serial_number, sizeof serial_number,
                                      on_txn, &g_txn_marker));
    step_at(20u);
    expect_bytes(sn_set_req, sizeof sn_set_req);

    step_at(20u + ACTION_MS);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    step_at(20u + IDENTITY_MS);
    expect_bytes(sn_set_req, sizeof sn_set_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
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
    test_inflight_txn_keeps_session_and_retries_same_sequence();
    test_heartbeat_silence_drops_session_and_cancels_txn_once();
    test_dropout_recovery_requires_a_new_handshake();
    test_late_slave_report_restarts_the_probe();
    test_passive_wait_past_setup_window_goes_offline();
    test_one_slave_dropping_leaves_the_other_usable();
    test_answer_invokes_callback_once_before_step_returns();
    test_callback_submits_leave_in_submit_order();
    test_reentering_step_does_not_spend_a_retry();
    test_callback_steps_a_different_host();
    test_two_hosts_keep_separate_fifos();
    test_identity_answer_within_100ms_ends_immediately();
    test_identity_answer_at_2s_still_succeeds();
    test_identity_silence_times_out_after_three_waits();
    test_identity_reject_is_not_retried();
    test_low_power_still_retries_at_100ms();
    test_serial_write_waits_3s_not_100ms();
    return ecsp_test_report();
}
