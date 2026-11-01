/*
 * 0x07's silence ceiling is its own compile-time value (spec 7.1, table 7-4).
 *
 * This binary is built with that ceiling loosened to 200 ms. The shared
 * 50 ms default is not. The seam is the host transmit FIFO and the
 * caller-supplied clock. Expected frames are the golden vectors.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

#define DEFAULT_MS 50u
#define RESET_MS   200u

/* spec 5.2.1 */
static const uint8_t handshake_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
};
static const uint8_t handshake_resp[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0x00, 0x50, 0x55, 0x0D
};
/* spec 5.2.2 "1.0.0.0" */
static const uint8_t version_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x02, 0x02, 0x88, 0xA0, 0x0D
};
static const uint8_t version_ok[] = {
    0xFA, 0x11, 0x00, 0x03, 0x02, 0x02, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30,
    0xFA, 0x0E, 0x0D
};
/* spec 5.2.18 */
static const uint8_t fault_query_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x12, 0x02, 0x85, 0x60, 0x0D
};
/* spec 5.2.7, transaction sequence 0x01 */
static const uint8_t reset_req[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x07, 0x02, 0x01, 0x71, 0x94, 0x0D
};
static const uint8_t reset_seq[1] = { 0x01u };
/* spec 5.2.8 start, seq 0x01 control 0x00 */
static const uint8_t selftest_start[2] = { 0x01u, 0x00u };
static const uint8_t selftest_req[] = {
    0xFA, 0x0B, 0x00, 0x03, 0x08, 0x02, 0x01, 0x00, 0x47, 0xF0, 0x0D
};
/* spec 5.2.11 enter */
static const uint8_t low_power_mode[1] = { 0x01u };
static const uint8_t low_power_req[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x0B, 0x02, 0x01, 0xB1, 0x97, 0x0D
};

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static ecsp_host_t g_host;
static ecsp_host_slave_cfg_t g_slave;

static void step_at(uint32_t now_ms)
{
    CHECK_EQ(ECSP_OK, ecsp_host_step(&g_host, now_ms));
}

static void expect_nothing(void)
{
    CHECK_EQ(0u, ecsp_host_tx_pending(&g_host));
}

static void expect_bytes(const uint8_t *want, size_t want_len)
{
    size_t got = ecsp_host_tx_pending(&g_host);

    CHECK_MEM(want, want_len, g_tx, got);
    ecsp_host_tx_drain(&g_host, got);
}

static void init_host(void)
{
    ecsp_host_cfg_t cfg;
    static const uint8_t expect[7] = { '1', '.', '0', '.', '0', '.', '0' };

    memcpy(g_slave.expect_version.b, expect, 7u);
    g_slave.board_addr = 0x02u;
    cfg.product_addr = 0x03u;
    cfg.slaves = &g_slave;
    cfg.slave_count = 1u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    cfg.on_event = NULL;
    cfg.user = NULL;
    CHECK_EQ(ECSP_OK, ecsp_host_init(&g_host, &cfg));
}

static void reach_normal(void)
{
    init_host();
    step_at(0u);
    ecsp_host_tx_drain(&g_host, ecsp_host_tx_pending(&g_host));
    ecsp_host_rx(&g_host, handshake_resp, sizeof handshake_resp);
    step_at(10u);
    expect_bytes(version_req, sizeof version_req);
    ecsp_host_rx(&g_host, version_ok, sizeof version_ok);
    step_at(20u);
    expect_nothing();
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, 0x02u));
}

/* Silence shorter than the ceiling leaves the FIFO empty. The ceiling
 * step sends the golden request again. */
static void expect_retry(uint8_t cmd, const uint8_t *payload, uint16_t len,
                         const uint8_t *frame, size_t frame_len, uint32_t ceiling)
{
    uint32_t sent = 30u;

    reach_normal();
    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, cmd, payload, len,
                                      NULL, NULL));
    step_at(sent);
    expect_bytes(frame, frame_len);

    step_at(sent + ceiling - 1u);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    step_at(sent + ceiling);
    expect_bytes(frame, frame_len);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
}

static void test_handshake_still_retries_at_50ms(void)
{
    init_host();
    step_at(0u);
    expect_bytes(handshake_req, sizeof handshake_req);

    step_at(DEFAULT_MS - 1u);
    expect_nothing();

    step_at(DEFAULT_MS);
    expect_bytes(handshake_req, sizeof handshake_req);
    CHECK_EQ(ECSP_SESS_HANDSHAKING, ecsp_host_session(&g_host, 0x02u));
}

static void test_query_set_and_action_still_use_50ms(void)
{
    expect_retry(ECSP_CMD_GET_FAULT, NULL, 0u,
                 fault_query_req, sizeof fault_query_req, DEFAULT_MS);
    expect_retry(ECSP_CMD_SET_LOW_POWER, low_power_mode, 1u,
                 low_power_req, sizeof low_power_req, DEFAULT_MS);
    expect_retry(ECSP_CMD_CONTROL_SELFTEST, selftest_start, 2u,
                 selftest_req, sizeof selftest_req, DEFAULT_MS);
}

/* 200 ms of silence is what retries 0x07. 50 ms is not. */
static void test_reset_waits_its_own_200ms(void)
{
    uint32_t sent = 30u;

    reach_normal();
    CHECK_EQ(ECSP_OK, ecsp_host_submit(&g_host, 0x02u, ECSP_CMD_SYSTEM_RESET,
                                      reset_seq, 1u, NULL, NULL));
    step_at(sent);
    expect_bytes(reset_req, sizeof reset_req);

    step_at(sent + DEFAULT_MS);
    expect_nothing();
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));

    step_at(sent + RESET_MS - 1u);
    expect_nothing();

    step_at(sent + RESET_MS);
    expect_bytes(reset_req, sizeof reset_req);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, 0x02u));
}

int main(void)
{
    test_handshake_still_retries_at_50ms();
    test_query_set_and_action_still_use_50ms();
    test_reset_waits_its_own_200ms();
    return ecsp_test_report();
}
