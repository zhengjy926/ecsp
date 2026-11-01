/*
 * Long-running command loop (ticket #8, spec 4.3, 4.4, 5.2.8-5.2.14).
 *
 * Seam: bytes in through ecsp_slave_rx, time through ecsp_slave_step, bytes
 * out through the transmit FIFO. The product side of the same seam is
 * ecsp_slave_long_run_cmd / ecsp_slave_long_run_complete and the safe-abort
 * callback. Tests do not read context fields.
 *
 * T_exec_max bytes below are the spec ceilings (5.2.8: 2000 ms = D0 07,
 * 5.2.12: 60000 ms = 60 EA), not a value computed the way the codec writes
 * them. Result and fault frames that the spec pins down are the golden bytes
 * from test/vectors/vectors.h.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

#define BOARD_ACT 0x30u

/* spec 5.2.8 recommended ceiling, little-endian. */
static const uint8_t selftest_tmax[2] = { 0xD0u, 0x07u };

/* spec 5.2.12 recommended ceiling, little-endian. */
static const uint8_t upgrade_tmax[2] = { 0x60u, 0xEAu };

/* spec 5.2.10 result 0x00. */
static const uint8_t selftest_report_ok[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0A, 0x02, 0x00, 0x21, 0x97, 0x0D
};

/* spec 5.2.14 result 0x00. */
static const uint8_t upgrade_report_ok[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0E, 0x02, 0x00, 0x60, 0x56, 0x0D
};

/* spec 5.2.17 fault word 0x01. */
static const uint8_t fault_report_bit0[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x11, 0x02, 0x01, 0x90, 0x50, 0x0D
};

/* spec 5.2.16. */
static const uint8_t heartbeat_frame[] = {
    0xFA, 0x09, 0x00, 0x03, 0x10, 0x02, 0x84, 0x00, 0x0D
};

static const uint8_t version_1_0_0_0[7] = { '1', '.', '0', '.', '0', '.', '0' };

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[4u * ECSP_MAX_FRAME];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;
static uint32_t g_now;
static int g_board_calls;
static int g_abort_calls;
static uint8_t g_abort_cmd;
static size_t g_tx_at_abort;
static int g_reset_at_abort;

static int board_act(ecsp_slave_t *s, const ecsp_frame_t *req,
                     uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)resp;
    (void)cap;
    g_board_calls++;
    *resp_len = 0u;
    return ECSP_RS_OK;
}

static const ecsp_cmd_entry_t g_board[] = {
    { BOARD_ACT, ECSP_CMD_TXN_SEQ, 1u, 0u, NULL, board_act }
};

static int abort_ok(ecsp_slave_t *s, uint8_t cmd)
{
    g_abort_calls++;
    g_abort_cmd = cmd;
    g_tx_at_abort = ecsp_slave_tx_pending(s);
    g_reset_at_abort = ecsp_slave_reset_requested(s);
    return ECSP_OK;
}

static int abort_fail(ecsp_slave_t *s, uint8_t cmd)
{
    g_abort_calls++;
    g_abort_cmd = cmd;
    g_tx_at_abort = ecsp_slave_tx_pending(s);
    (void)s;
    return ECSP_E_VAL;
}

static void drain(void)
{
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));
}

static void init_slave(void)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t hw[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_now = 0u;
    g_board_calls = 0;
    g_abort_calls = 0;
    g_abort_cmd = 0u;
    g_tx_at_abort = 0u;
    g_reset_at_abort = -1;

    memcpy(g_identity.hardware_version.b, hw, sizeof hw);
    memcpy(g_identity.serial_number.b, sn, sizeof sn);

    cfg.product_addr = 0x03u;
    cfg.board_addr = 0x02u;
    memcpy(cfg.version.b, version_1_0_0_0, sizeof version_1_0_0_0);
    cfg.identity = &g_identity;
    cfg.fault_word = 0x00u;
    cfg.upload_mode = ECSP_UPLOAD_ON;
    cfg.cmds.table = g_board;
    cfg.cmds.table_len = sizeof g_board / sizeof g_board[0];
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    CHECK_EQ(ECSP_OK, ecsp_slave_init(&g_slave, &cfg));
}

static void send_cmd(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n = ecsp_frame_encode(frame, sizeof frame, 0x03u, 0x02u, cmd,
                                 payload, len);

    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, g_now);
}

static void step(void)
{
    ecsp_slave_step(&g_slave, g_now);
}

static void expect_resp(uint8_t cmd, uint8_t rs,
                        const uint8_t *data, uint16_t data_len)
{
    uint8_t payload[ECSP_MAX_FRAME];
    uint8_t want[ECSP_MAX_FRAME];
    size_t want_len;

    payload[0] = rs;
    if (data_len > 0u) {
        memcpy(&payload[1], data, data_len);
    }
    want_len = ecsp_frame_encode(want, sizeof want, 0x03u, 0x02u, cmd,
                                 payload, (uint16_t)(1u + data_len));
    CHECK_EQ(want_len, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(want, want_len, g_tx, ecsp_slave_tx_pending(&g_slave));
    drain();
}

static void expect_raw(const uint8_t *frame, size_t n)
{
    CHECK_EQ(n, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(frame, n, g_tx, n);
    drain();
}

/* Answer frame, then a second already-built frame (event or fault report). */
static void expect_resp_then_raw(uint8_t cmd, uint8_t rs,
                                 const uint8_t *data, uint16_t data_len,
                                 const uint8_t *second, size_t second_len)
{
    uint8_t payload[ECSP_MAX_FRAME];
    uint8_t want[2u * ECSP_MAX_FRAME];
    size_t n;

    payload[0] = rs;
    if (data_len > 0u) {
        memcpy(&payload[1], data, data_len);
    }
    n = ecsp_frame_encode(want, sizeof want, 0x03u, 0x02u, cmd, payload,
                          (uint16_t)(1u + data_len));
    memcpy(&want[n], second, second_len);
    n += second_len;
    CHECK_EQ(n, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(want, n, g_tx, n);
    drain();
}

static void handshake(void)
{
    send_cmd(ECSP_CMD_HANDSHAKE, NULL, 0u);
    drain();
}

static void start_selftest(uint8_t seq)
{
    uint8_t payload[2];

    payload[0] = seq;
    payload[1] = ECSP_CTRL_START;
    send_cmd(ECSP_CMD_CONTROL_SELFTEST, payload, sizeof payload);
}

static void test_null_arguments(void)
{
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(NULL));
    CHECK_EQ(ECSP_E_ARG, ecsp_slave_long_run_complete(NULL, ECSP_RS_OK));
    ecsp_slave_set_safe_abort(NULL, abort_ok);
}

static void test_selftest_accept_sets_busy_and_carries_exec_max(void)
{
    init_slave();
    handshake();
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));

    start_selftest(0x01u);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_OK, selftest_tmax,
                sizeof selftest_tmax);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));
}

static void test_selftest_before_handshake_is_rejected(void)
{
    uint8_t payload[2] = { 0x01u, ECSP_CTRL_START };

    init_slave();
    send_cmd(ECSP_CMD_CONTROL_SELFTEST, payload, sizeof payload);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));
}

static void test_selftest_bad_length_and_control_are_rejected(void)
{
    uint8_t seq_only[1] = { 0x01u };
    uint8_t bad_ctrl[2] = { 0x02u, 0x05u };

    init_slave();
    handshake();

    send_cmd(ECSP_CMD_CONTROL_SELFTEST, seq_only, sizeof seq_only);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_BAD_LEN, NULL, 0u);

    send_cmd(ECSP_CMD_CONTROL_SELFTEST, bad_ctrl, sizeof bad_ctrl);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_BAD_PARAM, NULL, 0u);
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));
}

static void test_query_before_start_has_no_result_code(void)
{
    init_slave();
    handshake();

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_INVALID, NULL, 0u);

    send_cmd(ECSP_CMD_GET_UPGRADE, NULL, 0u);
    expect_resp(ECSP_CMD_GET_UPGRADE, ECSP_RS_INVALID, NULL, 0u);
}

static void test_completion_reports_once_and_query_returns_success(void)
{
    static const uint8_t succeeded[1] = { ECSP_RS_OK };

    init_slave();
    handshake();
    start_selftest(0x01u);
    drain();

    CHECK_EQ(ECSP_E_VAL,
             ecsp_slave_long_run_complete(&g_slave, ECSP_RS_BUSY));
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));
    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));
    CHECK_EQ(ECSP_E_ARG, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));

    step();
    expect_raw(selftest_report_ok, sizeof selftest_report_ok);

    /* The event is not repeated, including after the host has dropped it. */
    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_OK, succeeded, sizeof succeeded);

    /* Upgrade history is independent of the self-test result. */
    send_cmd(ECSP_CMD_GET_UPGRADE, NULL, 0u);
    expect_resp(ECSP_CMD_GET_UPGRADE, ECSP_RS_INVALID, NULL, 0u);
}

static void test_retransmit_of_start_does_not_start_again(void)
{
    static const uint8_t succeeded[1] = { ECSP_RS_OK };

    init_slave();
    handshake();
    start_selftest(0x01u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    step();
    drain();

    /* Same (0x08, seq) replays the accept and must not open a new run. */
    start_selftest(0x01u);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_OK, selftest_tmax,
                sizeof selftest_tmax);
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));
    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_OK, succeeded, sizeof succeeded);
}

static void test_past_exec_max_query_still_running(void)
{
    static const uint8_t still[1] = { ECSP_RS_BUSY };

    init_slave();
    handshake();
    start_selftest(0x01u);
    drain();

    /* One heartbeat period later: still running, and the heartbeat itself
     * is still sent (spec 4.3, 5.2.8). */
    g_now = ECSP_HEARTBEAT_MS;
    step();
    expect_raw(heartbeat_frame, sizeof heartbeat_frame);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));

    /* Past the 2000 ms ceiling measured from the accept at t = 0. The step
     * must not invent a result event. */
    g_now = ECSP_HEARTBEAT_MS + 2000u;
    step();
    expect_raw(heartbeat_frame, sizeof heartbeat_frame);

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_OK, still, sizeof still);
}

static void test_busy_rejects_ordinary_commands_without_exec_max(void)
{
    uint8_t second_start[2] = { 0x02u, ECSP_CTRL_START };
    uint8_t upgrade_start[2] = { 0x03u, ECSP_CTRL_START };
    uint8_t board_seq[1] = { 0x01u };
    uint8_t enter_lp[1] = { ECSP_LOW_POWER_ENTER };
    static const uint8_t still[1] = { ECSP_RS_BUSY };

    init_slave();
    handshake();
    start_selftest(0x01u);
    drain();

    /* A retransmission is a replay, not 0x11 (ADR-0002 step 6 before 7). */
    start_selftest(0x01u);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_OK, selftest_tmax,
                sizeof selftest_tmax);

    send_cmd(ECSP_CMD_CONTROL_SELFTEST, second_start, sizeof second_start);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_BUSY, NULL, 0u);

    send_cmd(ECSP_CMD_START_UPGRADE, upgrade_start, sizeof upgrade_start);
    expect_resp(ECSP_CMD_START_UPGRADE, ECSP_RS_BUSY, NULL, 0u);

    send_cmd(BOARD_ACT, board_seq, sizeof board_seq);
    expect_resp(BOARD_ACT, ECSP_RS_BUSY, NULL, 0u);
    CHECK_EQ(0, g_board_calls);

    send_cmd(ECSP_CMD_SET_LOW_POWER, enter_lp, sizeof enter_lp);
    expect_resp(ECSP_CMD_SET_LOW_POWER, ECSP_RS_BUSY, NULL, 0u);
    CHECK_EQ(ECSP_LOW_POWER_EXIT, ecsp_slave_low_power_mode(&g_slave));

    /* Getters still run (spec 4.3). The result byte 0x11 is "still executing",
     * not a rejection of the query. */
    send_cmd(ECSP_CMD_GET_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_VERSION, ECSP_RS_OK, version_1_0_0_0, 7u);

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_OK, still, sizeof still);
}

static void test_stop_is_accepted_and_result_becomes_aborted(void)
{
    uint8_t stop[2] = { 0x02u, ECSP_CTRL_STOP };
    uint8_t stop_again[2] = { 0x02u, ECSP_CTRL_STOP };
    uint8_t idle_stop[2] = { 0x09u, ECSP_CTRL_STOP };
    static const uint8_t aborted[1] = { ECSP_RS_ABORTED };
    uint8_t event[ECSP_MAX_FRAME];
    size_t event_len;

    init_slave();
    ecsp_slave_set_safe_abort(&g_slave, abort_ok);
    handshake();

    send_cmd(ECSP_CMD_CONTROL_SELFTEST, idle_stop, sizeof idle_stop);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_INVALID, NULL, 0u);
    CHECK_EQ(0, g_abort_calls);

    start_selftest(0x01u);
    drain();

    send_cmd(ECSP_CMD_CONTROL_SELFTEST, stop, sizeof stop);
    CHECK_EQ(1, g_abort_calls);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, g_abort_cmd);
    CHECK_EQ(0u, g_tx_at_abort);
    event_len = ecsp_frame_encode(event, sizeof event, 0x03u, 0x02u,
                                  ECSP_CMD_SELFTEST_REPORT, aborted,
                                  sizeof aborted);
    expect_resp_then_raw(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_OK, selftest_tmax,
                         sizeof selftest_tmax, event, event_len);
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));

    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_OK, aborted, sizeof aborted);

    /* Retransmit of the stop replays the accept and does not emit again. */
    send_cmd(ECSP_CMD_CONTROL_SELFTEST, stop_again, sizeof stop_again);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_OK, selftest_tmax,
                sizeof selftest_tmax);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    {
        static const uint8_t none[1] = { 0x00u };
        expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_OK, none, sizeof none);
    }
}

static void test_selftest_failure_sets_fault_and_success_clears_it(void)
{
    static const uint8_t failed_result[1] = { ECSP_RS_FAILED };
    static const uint8_t bit0[1] = { ECSP_FAULT_SELFTEST };
    static const uint8_t none[1] = { 0x00u };
    uint8_t event[ECSP_MAX_FRAME];
    uint8_t both[2u * ECSP_MAX_FRAME];
    size_t event_len;
    size_t n;

    init_slave();
    handshake();
    start_selftest(0x01u);
    drain();

    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_FAILED));
    step();
    event_len = ecsp_frame_encode(event, sizeof event, 0x03u, 0x02u,
                                  ECSP_CMD_SELFTEST_REPORT, failed_result,
                                  sizeof failed_result);
    memcpy(both, fault_report_bit0, sizeof fault_report_bit0);
    n = sizeof fault_report_bit0;
    memcpy(&both[n], event, event_len);
    n += event_len;
    expect_raw(both, n);

    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_OK, bit0, sizeof bit0);
    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_OK, failed_result,
                sizeof failed_result);

    start_selftest(0x02u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    step();
    expect_raw(selftest_report_ok, sizeof selftest_report_ok);

    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_OK, none, sizeof none);
}

static void test_reset_preempts_after_safe_abort(void)
{
    uint8_t seq1[1] = { 0x01u };
    uint8_t seq1_again[1] = { 0x01u };
    uint8_t seq2[1] = { 0x02u };

    init_slave();
    ecsp_slave_set_safe_abort(&g_slave, abort_ok);
    handshake();
    start_selftest(0x07u);
    drain();

    send_cmd(ECSP_CMD_SYSTEM_RESET, seq1, sizeof seq1);
    CHECK_EQ(1, g_abort_calls);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, g_abort_cmd);
    CHECK_EQ(0u, g_tx_at_abort);
    CHECK_EQ(0, g_reset_at_abort);
    expect_resp(ECSP_CMD_SYSTEM_RESET, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));

    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_INVALID, NULL, 0u);

    send_cmd(ECSP_CMD_SYSTEM_RESET, seq1_again, sizeof seq1_again);
    expect_resp(ECSP_CMD_SYSTEM_RESET, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_abort_calls);

    send_cmd(ECSP_CMD_SYSTEM_RESET, seq2, sizeof seq2);
    expect_resp(ECSP_CMD_SYSTEM_RESET, ECSP_RS_BUSY, NULL, 0u);
    CHECK_EQ(1, g_abort_calls);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));
}

static void test_unsafe_abort_refuses_reset_and_reports_fault(void)
{
    uint8_t seq1[1] = { 0x01u };
    uint8_t seq1_again[1] = { 0x01u };
    uint8_t seq2[1] = { 0x02u };
    static const uint8_t still[1] = { ECSP_RS_BUSY };
    static const uint8_t bit0[1] = { ECSP_FAULT_SELFTEST };

    init_slave();
    ecsp_slave_set_safe_abort(&g_slave, abort_fail);
    handshake();
    start_selftest(0x04u);
    drain();

    send_cmd(ECSP_CMD_SYSTEM_RESET, seq1, sizeof seq1);
    CHECK_EQ(1, g_abort_calls);
    CHECK_EQ(0u, g_tx_at_abort);
    expect_resp_then_raw(ECSP_CMD_SYSTEM_RESET, ECSP_RS_FAILED, NULL, 0u,
                         fault_report_bit0, sizeof fault_report_bit0);
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));

    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_OK, bit0, sizeof bit0);
    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_OK, still, sizeof still);

    /* Same transaction replays 0x20 and does not abort again. */
    send_cmd(ECSP_CMD_SYSTEM_RESET, seq1_again, sizeof seq1_again);
    expect_resp(ECSP_CMD_SYSTEM_RESET, ECSP_RS_FAILED, NULL, 0u);
    CHECK_EQ(1, g_abort_calls);

    /* A later transaction can still reset once the sequence succeeds.
     * bit0 is already 1, so there is no second fault report. */
    ecsp_slave_set_safe_abort(&g_slave, abort_ok);
    send_cmd(ECSP_CMD_SYSTEM_RESET, seq2, sizeof seq2);
    expect_resp(ECSP_CMD_SYSTEM_RESET, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(2, g_abort_calls);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));
    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_undefined_abort_refuses_reset(void)
{
    uint8_t seq[1] = { 0x03u };

    init_slave();
    handshake();
    start_selftest(0x05u);
    drain();

    send_cmd(ECSP_CMD_SYSTEM_RESET, seq, sizeof seq);
    CHECK_EQ(0, g_abort_calls);
    expect_resp_then_raw(ECSP_CMD_SYSTEM_RESET, ECSP_RS_FAILED, NULL, 0u,
                         fault_report_bit0, sizeof fault_report_bit0);
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));
}

static void test_upgrade_closed_loop_and_mutual_exclusion(void)
{
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    uint8_t stop_upgrade[2] = { 0x08u, ECSP_CTRL_STOP };
    static const uint8_t succeeded[1] = { ECSP_RS_OK };
    static const uint8_t none[1] = { 0x00u };

    init_slave();
    handshake();
    start_selftest(0x01u);
    drain();

    send_cmd(ECSP_CMD_START_UPGRADE, stop_upgrade, sizeof stop_upgrade);
    expect_resp(ECSP_CMD_START_UPGRADE, ECSP_RS_INVALID, NULL, 0u);

    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    step();
    drain();

    send_cmd(ECSP_CMD_START_UPGRADE, start, sizeof start);
    expect_resp(ECSP_CMD_START_UPGRADE, ECSP_RS_OK, upgrade_tmax,
                sizeof upgrade_tmax);
    CHECK_EQ(ECSP_CMD_START_UPGRADE, ecsp_slave_long_run_cmd(&g_slave));

    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    step();
    expect_raw(upgrade_report_ok, sizeof upgrade_report_ok);
    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_UPGRADE, NULL, 0u);
    expect_resp(ECSP_CMD_GET_UPGRADE, ECSP_RS_OK, succeeded, sizeof succeeded);

    /* A successful upgrade is not a self-test judgement, so bit0 stays clear. */
    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_OK, none, sizeof none);
}

static void test_handshake_stops_inflight_without_result_event(void)
{
    uint8_t enter_lp[1] = { ECSP_LOW_POWER_ENTER };

    init_slave();
    ecsp_slave_set_safe_abort(&g_slave, abort_ok);
    handshake();
    start_selftest(0x01u);
    drain();

    send_cmd(ECSP_CMD_HANDSHAKE, NULL, 0u);
    CHECK_EQ(1, g_abort_calls);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, g_abort_cmd);
    CHECK_EQ(0u, g_tx_at_abort);
    expect_resp(ECSP_CMD_HANDSHAKE, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));

    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_INVALID, NULL, 0u);

    send_cmd(ECSP_CMD_SET_LOW_POWER, enter_lp, sizeof enter_lp);
    expect_resp(ECSP_CMD_SET_LOW_POWER, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));
}

static void test_handshake_stops_even_if_abort_fails(void)
{
    static const uint8_t none[1] = { 0x00u };

    init_slave();
    ecsp_slave_set_safe_abort(&g_slave, abort_fail);
    handshake();
    start_selftest(0x01u);
    drain();

    /* Spec 4.1.4 still answers 0x00. A failed sequence does not latch the
     * fault bit; that report is the failed 0x07 path (spec 4.4). */
    send_cmd(ECSP_CMD_HANDSHAKE, NULL, 0u);
    expect_resp(ECSP_CMD_HANDSHAKE, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_abort_calls);
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));

    send_cmd(ECSP_CMD_GET_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_VERSION, ECSP_RS_OK, version_1_0_0_0, 7u);
    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_OK, none, sizeof none);
    send_cmd(ECSP_CMD_GET_SELFTEST, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SELFTEST, ECSP_RS_INVALID, NULL, 0u);
    step();
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

int main(void)
{
    test_null_arguments();
    test_selftest_accept_sets_busy_and_carries_exec_max();
    test_selftest_before_handshake_is_rejected();
    test_selftest_bad_length_and_control_are_rejected();
    test_query_before_start_has_no_result_code();
    test_completion_reports_once_and_query_returns_success();
    test_retransmit_of_start_does_not_start_again();
    test_past_exec_max_query_still_running();
    test_busy_rejects_ordinary_commands_without_exec_max();
    test_stop_is_accepted_and_result_becomes_aborted();
    test_selftest_failure_sets_fault_and_success_clears_it();
    test_reset_preempts_after_safe_abort();
    test_unsafe_abort_refuses_reset_and_reports_fault();
    test_undefined_abort_refuses_reset();
    test_upgrade_closed_loop_and_mutual_exclusion();
    test_handshake_stops_inflight_without_result_event();
    test_handshake_stops_even_if_abort_fails();
    return ecsp_test_report();
}
