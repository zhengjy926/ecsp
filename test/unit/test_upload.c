/*
 * 上传模式 (spec 5.2.15).
 *
 * Seam: the caller-owned transmit FIFO and the caller-supplied millisecond
 * clock. Assertions are framed bytes, response codes, session state,
 * transaction state, and the transaction callback.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

/* spec 5.2.15 control 0x00, and the 0x00 answer. Same bytes.
 * CRC16-MODBUS over 0A 00 03 0F 02 00 is 0x9631. */
static const uint8_t upload_off_frame[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0F, 0x02, 0x00, 0x31, 0x96, 0x0D
};

/* spec 5.2.15 control 0x01. CRC16-MODBUS over 0A 00 03 0F 02 01 is 0x56F0. */
static const uint8_t upload_on_req[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0F, 0x02, 0x01, 0xF0, 0x56, 0x0D
};

/* spec 5.2.15 answers 0x10 and 0x11.
 * CRC over 0A 00 03 0F 02 10 is 0x5A30.
 * CRC over 0A 00 03 0F 02 11 is 0x9AF1. */
static const uint8_t upload_not_handshaken[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0F, 0x02, 0x10, 0x30, 0x5A, 0x0D
};
static const uint8_t upload_busy[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0F, 0x02, 0x11, 0xF1, 0x9A, 0x0D
};

/* spec 5.2.15 answer 0x03. CRC16-MODBUS over 0A 00 03 0F 02 03 is 0x9771. */
static const uint8_t upload_bad_param[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0F, 0x02, 0x03, 0x71, 0x97, 0x0D
};

/* spec 5.2.9: 0x00 plus result 0x00. CRC over 0B 00 03 09 02 00 00 is 0x9C47. */
static const uint8_t get_selftest_ok[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x09, 0x02, 0x00, 0x00, 0x47, 0x9C, 0x0D
};

/* spec 5.2.18: 0x00 plus fault word 0x01. */
static const uint8_t fault_query_bit0[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x12, 0x02, 0x00, 0x01, 0x80, 0xB8, 0x0D
};

/* spec 5.2.10 result 0x00. CRC16-MODBUS over 0A 00 03 0A 02 00 is 0x9721. */
static const uint8_t selftest_report_ok[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0A, 0x02, 0x00, 0x21, 0x97, 0x0D
};

/* spec 5.2.10 result 0x20. CRC16-MODBUS over 0A 00 03 0A 02 20 is 0x4F20. */
static const uint8_t selftest_report_failed[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0A, 0x02, 0x20, 0x20, 0x4F, 0x0D
};

/* spec 5.2.17 fault word 0x01. CRC16-MODBUS over 0A 00 03 11 02 01 is 0x5090. */
static const uint8_t fault_report_bit0[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x11, 0x02, 0x01, 0x90, 0x50, 0x0D
};

/* spec 5.2.13: 0x00 plus result 0x00. */
static const uint8_t get_upgrade_ok[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x0D, 0x02, 0x00, 0x00, 0x46, 0xAC, 0x0D
};

/* spec 5.2.4 get hardware version "1.0.0.0". */
static const uint8_t hw_get_resp[] = {
    0xFA, 0x11, 0x00, 0x03, 0x04, 0x02, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30,
    0x1A, 0x11, 0x0D
};

/* spec 5.2.1 report, spec 5.2.16 heartbeat. */
static const uint8_t handshake_report[] = {
    0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
};
static const uint8_t heartbeat_frame[] = {
    0xFA, 0x09, 0x00, 0x03, 0x10, 0x02, 0x84, 0x00, 0x0D
};

/* spec 5.2.11: enter and exit both answer 0x00. CRC over 0A 00 03 0B 02 00
 * is 0x5770. */
static const uint8_t low_power_ok[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0B, 0x02, 0x00, 0x70, 0x57, 0x0D
};

/* spec 5.2.9: 0x00 plus result 0x20. CRC over 0B 00 03 09 02 00 20 is 0x4446. */
static const uint8_t get_selftest_failed[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x09, 0x02, 0x00, 0x20, 0x46, 0x44, 0x0D
};

/* spec 5.2.9: 0x13, no result byte. CRC over 0A 00 03 09 02 13 is 0x5A90. */
static const uint8_t get_selftest_none[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x09, 0x02, 0x13, 0x90, 0x5A, 0x0D
};

/* spec 5.2.6 answer for serial "SN0000000000". */
static const uint8_t serial_get_resp[] = {
    0xFA, 0x16, 0x00, 0x03, 0x06, 0x02, 0x00,
    0x53, 0x4E, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0xCC, 0x72, 0x0D
};

/* spec 5.2.1 / 5.2.2 bring-up, and spec 5.2.8 accept with T_exec_max 2000. */
static const uint8_t handshake_resp[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0x00, 0x50, 0x55, 0x0D
};
static const uint8_t version_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x02, 0x02, 0x88, 0xA0, 0x0D
};
static const uint8_t version_ok[] = {
    0xFA, 0x11, 0x00, 0x03, 0x02, 0x02, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30,
    0xFA, 0x0E, 0x0D
};
static const uint8_t selftest_req[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x08, 0x02, 0x01, 0x00, 0x47, 0xF0, 0x0D
};
static const uint8_t selftest_accept[] = {
    0xFA, 0x0C, 0x00, 0x03, 0x08, 0x02, 0x00, 0xD0, 0x07, 0xBC, 0x16, 0x0D
};
static const uint8_t get_selftest_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x09, 0x02, 0x8F, 0x90, 0x0D
};

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[4u * ECSP_MAX_FRAME];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;

static void drain(void)
{
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));
}

static void init_slave_cap(uint8_t upload_mode, size_t tx_cap)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t ver[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    memcpy(g_identity.hardware_version.b, ver, sizeof ver);
    memcpy(g_identity.serial_number.b, sn, sizeof sn);

    cfg.product_addr = 0x03u;
    cfg.board_addr = 0x02u;
    memcpy(cfg.version.b, ver, sizeof ver);
    cfg.identity = &g_identity;
    cfg.fault_word = 0x00u;
    cfg.upload_mode = upload_mode;
    cfg.cmds.table = NULL;
    cfg.cmds.table_len = 0u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = tx_cap;
    CHECK_EQ(ECSP_OK, ecsp_slave_init(&g_slave, &cfg));
}

static void init_slave_mode(uint8_t upload_mode)
{
    init_slave_cap(upload_mode, sizeof g_tx);
}

static void init_slave(void)
{
    init_slave_mode(ECSP_UPLOAD_ON);
}

static void send_at(uint8_t cmd, const uint8_t *payload, uint16_t len,
                    uint32_t now_ms)
{
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n = ecsp_frame_encode(frame, sizeof frame, 0x03u, 0x02u, cmd,
                                 payload, len);

    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, now_ms);
}

static void expect_frame(const uint8_t *frame, size_t n)
{
    CHECK_EQ(n, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(frame, n, g_tx, n);
    drain();
}

static void expect_nothing(void)
{
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void expect_two(const uint8_t *first, size_t first_len,
                       const uint8_t *second, size_t second_len)
{
    uint8_t both[2u * ECSP_MAX_FRAME];

    CHECK(first_len + second_len <= sizeof both);
    memcpy(both, first, first_len);
    memcpy(both + first_len, second, second_len);
    expect_frame(both, first_len + second_len);
}

static void handshake(void)
{
    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 0u);
    drain();
}

/* Control 0x00 answers 0x00. */
static void test_off_answers_ok(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
}

static void finish_selftest_ok(void)
{
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };

    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    ecsp_slave_step(&g_slave, 0u);
}

/* A control byte other than 0x00 or 0x01 answers 0x03. */
static void test_other_control_answers_bad_param(void)
{
    static const uint8_t bad[1] = { 0x02u };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, bad, 1u, 0u);
    expect_frame(upload_bad_param, sizeof upload_bad_param);
}

/* While off, a finished self-test does not send 0x0A. 0x09 still returns it. */
static void test_off_holds_selftest_report_but_query_returns_it(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    finish_selftest_ok();
    expect_nothing();

    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, 0u);
    expect_frame(get_selftest_ok, sizeof get_selftest_ok);
}

/* A failed self-test while off does not send 0x11. 0x12 still returns bit0. */
static void test_off_holds_fault_report_but_query_returns_it(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_FAILED));
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_GET_FAULT, NULL, 0u, 0u);
    expect_frame(fault_query_bit0, sizeof fault_query_bit0);
}

/* Power-up off sends no 0x0A, before any 0x0F. */
static void test_power_up_off_holds_selftest_report(void)
{
    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    finish_selftest_ok();
    expect_nothing();
}

/* Power-up on, the value a product sets when it does not declare another,
 * still sends 0x0A. */
static void test_power_up_on_sends_selftest_report(void)
{
    init_slave_mode(ECSP_UPLOAD_ON);
    handshake();
    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);
}

/* 0x01 answers 0x00. Repeating it leaves the report flowing. */
static void test_on_and_repeat_keep_reports(void)
{
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);
}

/* Repeating off answers 0x00 and still sends no 0x0A. */
static void test_repeat_off_stays_quiet(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    finish_selftest_ok();
    expect_nothing();
}

/* 0x03 leaves an on slave reporting, and an off slave quiet. */
static void test_bad_control_leaves_mode(void)
{
    static const uint8_t bad[1] = { 0x02u };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, bad, 1u, 0u);
    expect_frame(upload_bad_param, sizeof upload_bad_param);
    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, bad, 1u, 0u);
    expect_frame(upload_bad_param, sizeof upload_bad_param);
    finish_selftest_ok();
    expect_nothing();
}

/* Turning the mode on is answered 0x00 and the next result is reported. */
static void test_turn_on_sends_later_report(void)
{
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);
}

static int init_result(uint8_t upload_mode)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t ver[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    memcpy(g_identity.hardware_version.b, ver, sizeof ver);
    memcpy(g_identity.serial_number.b, sn, sizeof sn);
    cfg.product_addr = 0x03u;
    cfg.board_addr = 0x02u;
    memcpy(cfg.version.b, ver, sizeof ver);
    cfg.identity = &g_identity;
    cfg.fault_word = 0x00u;
    cfg.upload_mode = upload_mode;
    cfg.cmds.table = NULL;
    cfg.cmds.table_len = 0u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    return ecsp_slave_init(&g_slave, &cfg);
}

/* Zero and any other value that is neither on nor off cannot initialise. */
static void test_init_rejects_other_upload_mode(void)
{
    CHECK_EQ(ECSP_E_ARG, init_result(0u));
    CHECK_EQ(ECSP_E_ARG, init_result(0x03u));
    CHECK_EQ(ECSP_E_ARG, init_result(0xFFu));
    CHECK_EQ(ECSP_E_ARG, init_result(ECSP_CTRL_UPLOAD_OFF));
    CHECK_EQ(ECSP_OK, init_result(ECSP_UPLOAD_ON));
    CHECK_EQ(ECSP_OK, init_result(ECSP_UPLOAD_OFF));
}

/* Re-init applies the configured power-up value again. */
static void test_reinit_returns_to_configured_power_up(void)
{
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    finish_selftest_ok();
    expect_nothing();

    init_slave_mode(ECSP_UPLOAD_ON);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    finish_selftest_ok();
    expect_nothing();

    init_slave_mode(ECSP_UPLOAD_ON);
    handshake();
    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);
}

/* While off, a finished upgrade does not send 0x0E. 0x0D still returns it. */
static void test_off_holds_upgrade_report_but_query_returns_it(void)
{
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    send_at(ECSP_CMD_START_UPGRADE, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_GET_UPGRADE, NULL, 0u, 0u);
    expect_frame(get_upgrade_ok, sizeof get_upgrade_ok);
}

/* Heartbeats keep their period while off. The waiting 0x01 report does too. */
static void test_heartbeat_and_waiting_report_while_off(void)
{
    init_slave_mode(ECSP_UPLOAD_OFF);
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();
    ecsp_slave_step(&g_slave, 999u);
    expect_nothing();
    ecsp_slave_step(&g_slave, 1000u);
    expect_frame(handshake_report, sizeof handshake_report);

    handshake();
    ecsp_slave_step(&g_slave, 999u);
    expect_nothing();
    ecsp_slave_step(&g_slave, 1000u);
    expect_frame(heartbeat_frame, sizeof heartbeat_frame);
}

/* Before the handshake, 0x0F answers 0x10 and the power-up mode stays. */
static void test_upload_before_handshake_is_rejected(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_not_handshaken, sizeof upload_not_handshaken);

    handshake();
    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);
}

/* While a long-running command is busy, 0x0F answers 0x11 and the mode stays. */
static void test_upload_while_busy_is_rejected(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));

    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_busy, sizeof upload_busy);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));

    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    ecsp_slave_step(&g_slave, 0u);
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);
}

static int g_commits;

static int on_identity_commit(ecsp_slave_t *s, const ecsp_slave_identity_t *next)
{
    (void)s;
    (void)next;
    g_commits++;
    return ECSP_OK;
}

/* 0x0F does not commit non-volatile identity storage. */
static void test_upload_does_not_commit_identity(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave();
    ecsp_slave_set_identity_commit(&g_slave, on_identity_commit);
    handshake();
    g_commits = 0;
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    CHECK_EQ(0, g_commits);

    send_at(ECSP_CMD_GET_HW_VERSION, NULL, 0u, 0u);
    expect_frame(hw_get_resp, sizeof hw_get_resp);
}

static uint8_t g_host_rx[ECSP_MAX_FRAME];
static uint8_t g_host_tx[2u * ECSP_MAX_FRAME];
static ecsp_host_t g_host;
static ecsp_host_slave_cfg_t g_slot;
static int g_txn_calls;
static int g_txn_status;
static uint8_t g_txn_cmd;
static uint8_t g_txn_rs;

static void on_txn(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                   int status, uint8_t response_code,
                   const uint8_t *payload, uint16_t len, void *user)
{
    (void)h;
    (void)board_addr;
    (void)payload;
    (void)len;
    (void)user;
    g_txn_calls++;
    g_txn_status = status;
    g_txn_cmd = cmd;
    g_txn_rs = response_code;
}

static void host_step(uint32_t now_ms)
{
    CHECK_EQ(ECSP_OK, ecsp_host_step(&g_host, now_ms));
}

static void expect_host(const uint8_t *want, size_t want_len)
{
    size_t got = ecsp_host_tx_pending(&g_host);

    CHECK_MEM(want, want_len, g_host_tx, got);
    ecsp_host_tx_drain(&g_host, got);
}

static void expect_host_nothing(void)
{
    CHECK_EQ(0u, ecsp_host_tx_pending(&g_host));
}

static void init_host(void)
{
    ecsp_host_cfg_t cfg;
    static const uint8_t expect[7] = { '1', '.', '0', '.', '0', '.', '0' };

    memcpy(g_slot.expect_version.b, expect, 7u);
    g_slot.board_addr = 0x02u;
    cfg.product_addr = 0x03u;
    cfg.slaves = &g_slot;
    cfg.slave_count = 1u;
    cfg.rx_buf = g_host_rx;
    cfg.rx_cap = sizeof g_host_rx;
    cfg.tx_buf = g_host_tx;
    cfg.tx_cap = sizeof g_host_tx;
    cfg.on_event = NULL;
    cfg.user = NULL;
    cfg.board_cmds = (ecsp_cmd_table_t){ NULL, 0u };
    CHECK_EQ(ECSP_OK, ecsp_host_init(&g_host, &cfg));
}

static void reach_normal(void)
{
    init_host();
    host_step(0u);
    ecsp_host_tx_drain(&g_host, ecsp_host_tx_pending(&g_host));
    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    host_step(10u);
    expect_host(version_req, sizeof version_req);
    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    host_step(20u);
    expect_host_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
}

/* 0x0F retries at 50 ms of silence, not at the 3 s identity ceiling.
 * An answer before that ends the transaction. */
static void test_upload_silence_ceiling_is_50ms(void)
{
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };
    uint32_t sent = 30u;

    g_txn_calls = 0;
    reach_normal();
    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_CONTROL_UPLOAD,
                                      on, 1u, on_txn, NULL));
    host_step(sent);
    expect_host(upload_on_req, sizeof upload_on_req);

    host_step(sent + 49u);
    expect_host_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(0, g_txn_calls);

    host_step(sent + 50u);
    expect_host(upload_on_req, sizeof upload_on_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    ecsp_host_rx(&g_host, upload_off_frame, sizeof upload_off_frame);
    host_step(sent + 60u);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_OK, g_txn_status);
    CHECK_EQ(ECSP_CMD_CONTROL_UPLOAD, g_txn_cmd);
    CHECK_EQ(0x00, g_txn_rs);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    expect_host_nothing();

    host_step(sent + 3000u);
    expect_host_nothing();
    CHECK_EQ(1, g_txn_calls);
}

/* An accepted handshake puts 上传模式 back on the configured power-up
 * value. A 0x0A that became ready while the mode was off stays out of the
 * transmit FIFO; 0x09 still returns it. The next result is sent, because
 * that power-up value is on. */
static void test_handshake_restores_power_up_on_without_replaying(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave_mode(ECSP_UPLOAD_ON);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    finish_selftest_ok();
    expect_nothing();

    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 0u);
    expect_frame(handshake_resp, sizeof handshake_resp);
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, 0u);
    expect_frame(get_selftest_ok, sizeof get_selftest_ok);

    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);
}

/* Power-up off: an accepted handshake turns 上传模式 back off, so the
 * next result event is not sent. */
static void test_handshake_restores_power_up_off(void)
{
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    finish_selftest_ok();
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);

    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 0u);
    expect_frame(handshake_resp, sizeof handshake_resp);
    finish_selftest_ok();
    expect_nothing();

    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, 0u);
    expect_frame(get_selftest_ok, sizeof get_selftest_ok);
}

/* A 0x0A armed while 上传模式 was on stays armed when 低功耗 was holding
 * it, or when only a full transmit FIFO was holding it. 0x0B leaving
 * 低功耗 while 上传模式 is off does not drop that frame. It goes out on
 * a later step, including a handshake that puts the power-up value back
 * to off. 0x09 still returns the result. */
static void test_armed_one_shot_survives_low_power_exit_while_upload_off(void)
{
    static const uint8_t enter[1] = { ECSP_LOW_POWER_ENTER };
    static const uint8_t leave[1] = { ECSP_LOW_POWER_EXIT };
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    size_t one_answer = ECSP_FRAME_MIN_LEN + 1u + ECSP_SERIAL_LEN;
    size_t held = sizeof upload_off_frame + sizeof low_power_ok;

    init_slave_cap(ECSP_UPLOAD_OFF, one_answer);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    send_at(ECSP_CMD_SET_LOW_POWER, enter, 1u, 0u);
    expect_frame(low_power_ok, sizeof low_power_ok);

    finish_selftest_ok();
    expect_nothing();

    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    CHECK_EQ(sizeof upload_off_frame, ecsp_slave_tx_pending(&g_slave));
    send_at(ECSP_CMD_SET_LOW_POWER, leave, 1u, 0u);
    CHECK_EQ(held, ecsp_slave_tx_pending(&g_slave));
    drain();

    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 0u);
    expect_two(handshake_resp, sizeof handshake_resp,
               selftest_report_ok, sizeof selftest_report_ok);
    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, 0u);
    expect_frame(get_selftest_ok, sizeof get_selftest_ok);

    init_slave_cap(ECSP_UPLOAD_ON, one_answer);
    handshake();
    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    send_at(ECSP_CMD_GET_SERIAL, NULL, 0u, 0u);
    expect_frame(serial_get_resp, sizeof serial_get_resp);

    send_at(ECSP_CMD_SET_LOW_POWER, enter, 1u, 0u);
    expect_frame(low_power_ok, sizeof low_power_ok);
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    send_at(ECSP_CMD_SET_LOW_POWER, leave, 1u, 0u);
    expect_two(low_power_ok, sizeof low_power_ok,
               selftest_report_ok, sizeof selftest_report_ok);
    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, 0u);
    expect_frame(get_selftest_ok, sizeof get_selftest_ok);
}

/* A 0x0A armed while 上传模式 was on is sent when 0x0B leaves 低功耗,
 * even though the mode is now off. Turning the mode back on does not send
 * it again. The heartbeat stays silent while entered and is due one period
 * after the exit answer. */
static void test_exit_sends_armed_report_then_resumes_heartbeat(void)
{
    static const uint8_t enter[1] = { ECSP_LOW_POWER_ENTER };
    static const uint8_t leave[1] = { ECSP_LOW_POWER_EXIT };
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };
    uint32_t left = 100u;

    init_slave();
    handshake();
    send_at(ECSP_CMD_SET_LOW_POWER, enter, 1u, 0u);
    expect_frame(low_power_ok, sizeof low_power_ok);
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));

    finish_selftest_ok();
    expect_nothing();

    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));

    send_at(ECSP_CMD_SET_LOW_POWER, leave, 1u, left);
    expect_two(low_power_ok, sizeof low_power_ok,
               selftest_report_ok, sizeof selftest_report_ok);
    CHECK_EQ(ECSP_LOW_POWER_EXIT, ecsp_slave_low_power_mode(&g_slave));
    ecsp_slave_step(&g_slave, left);
    expect_nothing();

    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, left);
    expect_frame(get_selftest_ok, sizeof get_selftest_ok);

    ecsp_slave_step(&g_slave, left + 999u);
    expect_nothing();
    ecsp_slave_step(&g_slave, left + 1000u);
    expect_frame(heartbeat_frame, sizeof heartbeat_frame);

    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, left + 1000u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    ecsp_slave_step(&g_slave, left + 1000u);
    expect_nothing();
}

/* A failed self-test armed while 上传模式 was on sends 0x11 and 0x0A when
 * 0x0B leaves 低功耗, even though the mode is now off. 0x12 still returns
 * the fault word. */
static void test_exit_sends_armed_fault_and_query_still_reads_it(void)
{
    static const uint8_t enter[1] = { ECSP_LOW_POWER_ENTER };
    static const uint8_t leave[1] = { ECSP_LOW_POWER_EXIT };
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    uint8_t out[48];
    size_t n;
    uint32_t left = 100u;

    init_slave();
    handshake();
    send_at(ECSP_CMD_SET_LOW_POWER, enter, 1u, 0u);
    expect_frame(low_power_ok, sizeof low_power_ok);

    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_FAILED));
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    send_at(ECSP_CMD_SET_LOW_POWER, leave, 1u, left);
    n = 0u;
    memcpy(out, low_power_ok, sizeof low_power_ok);
    n += sizeof low_power_ok;
    memcpy(out + n, fault_report_bit0, sizeof fault_report_bit0);
    n += sizeof fault_report_bit0;
    memcpy(out + n, selftest_report_failed, sizeof selftest_report_failed);
    n += sizeof selftest_report_failed;
    expect_frame(out, n);
    CHECK_EQ(ECSP_LOW_POWER_EXIT, ecsp_slave_low_power_mode(&g_slave));
    ecsp_slave_step(&g_slave, left);
    expect_nothing();

    send_at(ECSP_CMD_GET_FAULT, NULL, 0u, left);
    expect_frame(fault_query_bit0, sizeof fault_query_bit0);
}

/* Turning 上传模式 back on does not emit 0x0A, 0x0E, or 0x11 that became
 * ready while it was off. 0x09, 0x0D, and 0x12 still return them. */
static void test_turn_on_does_not_replay_missed_one_shots(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };
    uint8_t upgrade[2] = { 0x01u, ECSP_CTRL_START };
    uint8_t selftest[2] = { 0x02u, ECSP_CTRL_START };

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);

    send_at(ECSP_CMD_START_UPGRADE, upgrade, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_CONTROL_SELFTEST, selftest, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_FAILED));
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_GET_UPGRADE, NULL, 0u, 0u);
    expect_frame(get_upgrade_ok, sizeof get_upgrade_ok);
    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, 0u);
    expect_frame(get_selftest_failed, sizeof get_selftest_failed);
    send_at(ECSP_CMD_GET_FAULT, NULL, 0u, 0u);
    expect_frame(fault_query_bit0, sizeof fault_query_bit0);
}

/* 0x0A, 0x0E, and 0x11 that became ready while 上传模式 was off are not
 * flushed by leaving 低功耗. Heartbeats stay silent during 低功耗 and
 * resume one period after the exit answer. */
static void test_leave_low_power_while_upload_stays_off(void)
{
    static const uint8_t enter[1] = { ECSP_LOW_POWER_ENTER };
    static const uint8_t leave[1] = { ECSP_LOW_POWER_EXIT };
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };
    uint8_t upgrade[2] = { 0x01u, ECSP_CTRL_START };
    uint8_t selftest[2] = { 0x02u, ECSP_CTRL_START };
    uint32_t left = 200u;

    init_slave_mode(ECSP_UPLOAD_OFF);
    handshake();
    send_at(ECSP_CMD_SET_LOW_POWER, enter, 1u, 0u);
    expect_frame(low_power_ok, sizeof low_power_ok);

    ecsp_slave_step(&g_slave, 1000u);
    expect_nothing();

    send_at(ECSP_CMD_START_UPGRADE, upgrade, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_CONTROL_SELFTEST, selftest, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_FAILED));
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();

    send_at(ECSP_CMD_SET_LOW_POWER, leave, 1u, left);
    expect_frame(low_power_ok, sizeof low_power_ok);
    CHECK_EQ(ECSP_LOW_POWER_EXIT, ecsp_slave_low_power_mode(&g_slave));
    ecsp_slave_step(&g_slave, left + 999u);
    expect_nothing();
    ecsp_slave_step(&g_slave, left + 1000u);
    expect_frame(heartbeat_frame, sizeof heartbeat_frame);

    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, left + 1000u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    ecsp_slave_step(&g_slave, left + 1000u);
    expect_nothing();

    send_at(ECSP_CMD_GET_UPGRADE, NULL, 0u, left + 1000u);
    expect_frame(get_upgrade_ok, sizeof get_upgrade_ok);
    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, left + 1000u);
    expect_frame(get_selftest_failed, sizeof get_selftest_failed);
    send_at(ECSP_CMD_GET_FAULT, NULL, 0u, left + 1000u);
    expect_frame(fault_query_bit0, sizeof fault_query_bit0);
}

/* 低功耗 keeps suppressing heartbeats while 上传模式 is on. */
static void test_low_power_suppresses_heartbeats_while_upload_on(void)
{
    static const uint8_t enter[1] = { ECSP_LOW_POWER_ENTER };

    init_slave();
    handshake();
    send_at(ECSP_CMD_SET_LOW_POWER, enter, 1u, 0u);
    expect_frame(low_power_ok, sizeof low_power_ok);

    ecsp_slave_step(&g_slave, 1000u);
    expect_nothing();
    ecsp_slave_step(&g_slave, 2000u);
    expect_nothing();
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));
}

/* Handshake still leaves 低功耗 and still emits no result event for the
 * in-flight long-running command it interrupted. */
static void test_handshake_leaves_low_power_without_result_event(void)
{
    static const uint8_t enter[1] = { ECSP_LOW_POWER_ENTER };
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    uint32_t resumed = 1500u;

    init_slave();
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    send_at(ECSP_CMD_SET_LOW_POWER, enter, 1u, 0u);
    expect_frame(low_power_ok, sizeof low_power_ok);

    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));
    ecsp_slave_step(&g_slave, 1000u);
    expect_nothing();
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));

    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, resumed);
    expect_frame(handshake_resp, sizeof handshake_resp);
    CHECK_EQ(0u, ecsp_slave_long_run_cmd(&g_slave));
    CHECK_EQ(ECSP_LOW_POWER_EXIT, ecsp_slave_low_power_mode(&g_slave));
    ecsp_slave_step(&g_slave, resumed);
    expect_nothing();

    send_at(ECSP_CMD_GET_SELFTEST, NULL, 0u, resumed);
    expect_frame(get_selftest_none, sizeof get_selftest_none);

    ecsp_slave_step(&g_slave, resumed + 999u);
    expect_nothing();
    ecsp_slave_step(&g_slave, resumed + 1000u);
    expect_frame(heartbeat_frame, sizeof heartbeat_frame);
}

/* Bytes already in the transmit FIFO stay there when 0x0F turns upload off. */
static void test_queued_report_is_not_withdrawn_when_upload_turns_off(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };

    init_slave();
    handshake();
    finish_selftest_ok();
    send_at(ECSP_CMD_CONTROL_UPLOAD, off, 1u, 0u);
    expect_two(selftest_report_ok, sizeof selftest_report_ok,
               upload_off_frame, sizeof upload_off_frame);
    ecsp_slave_step(&g_slave, 0u);
    expect_nothing();
}

/* A result the FIFO could not take is sent on a later step, and still sent
 * when that later step is a handshake that restores power-up off. */
static void test_fifo_full_report_is_sent_later_including_after_handshake(void)
{
    static const uint8_t on[1] = { ECSP_CTRL_UPLOAD_ON };
    uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    size_t one_answer = ECSP_FRAME_MIN_LEN + 1u + ECSP_SERIAL_LEN;

    init_slave_cap(ECSP_UPLOAD_ON, one_answer);
    handshake();
    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    send_at(ECSP_CMD_GET_SERIAL, NULL, 0u, 0u);
    expect_frame(serial_get_resp, sizeof serial_get_resp);
    ecsp_slave_step(&g_slave, 0u);
    expect_frame(selftest_report_ok, sizeof selftest_report_ok);

    init_slave_cap(ECSP_UPLOAD_OFF, one_answer);
    handshake();
    send_at(ECSP_CMD_CONTROL_UPLOAD, on, 1u, 0u);
    expect_frame(upload_off_frame, sizeof upload_off_frame);
    send_at(ECSP_CMD_CONTROL_SELFTEST, start, 2u, 0u);
    drain();
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    send_at(ECSP_CMD_GET_SERIAL, NULL, 0u, 0u);
    expect_frame(serial_get_resp, sizeof serial_get_resp);

    send_at(ECSP_CMD_HANDSHAKE, NULL, 0u, 0u);
    expect_two(handshake_resp, sizeof handshake_resp,
               selftest_report_ok, sizeof selftest_report_ok);
}

/* After 0x0F, a long-running accept still waits T_exec_max before 0x09. */
static void test_host_still_waits_exec_max_after_upload_off(void)
{
    static const uint8_t off[1] = { ECSP_CTRL_UPLOAD_OFF };
    static const uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    uint32_t accepted = 40u;

    g_txn_calls = 0;
    reach_normal();
    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_CONTROL_UPLOAD,
                                      off, 1u, on_txn, NULL));
    host_step(30u);
    expect_host(upload_off_frame, sizeof upload_off_frame);
    ecsp_host_rx(&g_host, upload_off_frame, sizeof upload_off_frame);
    host_step(35u);
    CHECK_EQ(1, g_txn_calls);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, 0x02u));

    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_CONTROL_SELFTEST,
                                      start, 2u, on_txn, NULL));
    host_step(accepted);
    expect_host(selftest_req, sizeof selftest_req);
    ecsp_host_rx(&g_host, selftest_accept, sizeof selftest_accept);
    host_step(accepted + 10u);
    expect_host_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESULT, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(1, g_txn_calls);

    host_step(accepted + 10u + 50u);
    expect_host_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESULT, ecsp_host_txn(&g_host, 0x02u));

    host_step(accepted + 10u + 1999u);
    expect_host_nothing();
    CHECK_EQ(1, g_txn_calls);

    host_step(accepted + 10u + 2000u);
    expect_host(get_selftest_req, sizeof get_selftest_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
    CHECK_EQ(1, g_txn_calls);
}

int main(void)
{
    test_off_answers_ok();
    test_other_control_answers_bad_param();
    test_off_holds_selftest_report_but_query_returns_it();
    test_off_holds_fault_report_but_query_returns_it();
    test_power_up_off_holds_selftest_report();
    test_power_up_on_sends_selftest_report();
    test_on_and_repeat_keep_reports();
    test_repeat_off_stays_quiet();
    test_bad_control_leaves_mode();
    test_turn_on_sends_later_report();
    test_init_rejects_other_upload_mode();
    test_reinit_returns_to_configured_power_up();
    test_off_holds_upgrade_report_but_query_returns_it();
    test_heartbeat_and_waiting_report_while_off();
    test_upload_before_handshake_is_rejected();
    test_upload_while_busy_is_rejected();
    test_upload_does_not_commit_identity();
    test_upload_silence_ceiling_is_50ms();
    test_handshake_restores_power_up_on_without_replaying();
    test_handshake_restores_power_up_off();
    test_armed_one_shot_survives_low_power_exit_while_upload_off();
    test_exit_sends_armed_report_then_resumes_heartbeat();
    test_exit_sends_armed_fault_and_query_still_reads_it();
    test_turn_on_does_not_replay_missed_one_shots();
    test_leave_low_power_while_upload_stays_off();
    test_low_power_suppresses_heartbeats_while_upload_on();
    test_handshake_leaves_low_power_without_result_event();
    test_queued_report_is_not_withdrawn_when_upload_turns_off();
    test_fifo_full_report_is_sent_later_including_after_handshake();
    test_host_still_waits_exec_max_after_upload_off();
    return ecsp_test_report();
}
