/*
 * Board commands 0x30-0x33 on the virtual link (ticket #15, parent #13).
 *
 * Seam: one host context and one slave context, with a byte array between
 * them. The slave is given this board's command table, which is not part of
 * the protocol library. The chip id is a fixed stand-in. Assertions name
 * session state and transaction results (status, response code, payload).
 * They do not read context fields.
 */

#include "ecsp/ecsp.h"

#include "bringup.h"
#include "test_util.h"

#define BOARD 0x02u
#define LOG_MAX 8

static const uint8_t g_chip_id[F429_CHIP_ID_LEN] = {
    0x10u, 0x32u, 0x54u, 0x76u, 0x98u, 0xBAu, 0xDCu, 0xFEu,
    0x01u, 0x23u, 0x45u, 0x67u
};

static const uint8_t g_version[7] = {
    '1', '.', '0', '.', '0', '.', '0'
};

static const uint8_t g_serial[12] = {
    'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
};

typedef struct {
    uint8_t cmd;
    int status;
    uint8_t rs;
    uint8_t data[16];
    uint16_t len;
} txn_rec_t;

static uint8_t g_host_rx[4u * ECSP_MAX_FRAME];
static uint8_t g_host_tx[4u * ECSP_MAX_FRAME];
static uint8_t g_slave_rx[4u * ECSP_MAX_FRAME];
static uint8_t g_slave_tx[4u * ECSP_MAX_FRAME];
static ecsp_host_t g_host;
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;
static ecsp_slave_identity_t g_nvm;
static ecsp_host_slave_cfg_t g_slot;
static txn_rec_t g_log[LOG_MAX];
static int g_log_n;
static int g_commits;
static int g_keep_identity;
static uint32_t g_now;

static void on_txn(ecsp_host_t *h, uint8_t board_addr, uint8_t cmd,
                   int status, uint8_t response_code,
                   const uint8_t *payload, uint16_t len, void *user)
{
    txn_rec_t *r;
    uint16_t n;

    (void)h;
    (void)board_addr;
    (void)user;
    if (g_log_n >= LOG_MAX) {
        g_log_n++;
        return;
    }
    r = &g_log[g_log_n];
    g_log_n++;
    memset(r, 0, sizeof *r);
    r->cmd = cmd;
    r->status = status;
    r->rs = response_code;
    r->len = len;
    n = len;
    if (n > (uint16_t)sizeof r->data) {
        n = (uint16_t)sizeof r->data;
    }
    if (n > 0u && payload != NULL) {
        memcpy(r->data, payload, n);
    }
}

static txn_rec_t *nth_txn(uint8_t cmd, int nth)
{
    int i;
    int seen = 0;
    int n = g_log_n;

    if (n > LOG_MAX) {
        n = LOG_MAX;
    }
    for (i = 0; i < n; i++) {
        if (g_log[i].cmd == cmd) {
            if (seen == nth) {
                return &g_log[i];
            }
            seen++;
        }
    }
    return NULL;
}

static int commit_identity(ecsp_slave_t *s, const ecsp_slave_identity_t *next)
{
    (void)s;
    g_commits++;
    g_nvm = *next;
    return ECSP_OK;
}

static void init_slave(void)
{
    ecsp_slave_cfg_t cfg;
    const ecsp_cmd_table_t *cmds;

    f429_board_init(g_chip_id);
    if (g_keep_identity == 0) {
        memcpy(g_identity.hardware_version.b, g_version, sizeof g_version);
        memcpy(g_identity.serial_number.b, g_serial, sizeof g_serial);
    }

    cmds = f429_board_cmds();
    cfg.product_addr = 0x03u;
    cfg.board_addr = BOARD;
    memcpy(cfg.version.b, g_version, sizeof g_version);
    cfg.identity = &g_identity;
    cfg.fault_word = 0u;
    cfg.cmds = *cmds;
    cfg.rx_buf = g_slave_rx;
    cfg.rx_cap = sizeof g_slave_rx;
    cfg.tx_buf = g_slave_tx;
    cfg.tx_cap = sizeof g_slave_tx;
    CHECK_EQ(ECSP_OK, ecsp_slave_init(&g_slave, &cfg));
}

static void init_host(void)
{
    ecsp_host_cfg_t cfg;

    memcpy(g_slot.expect_version.b, g_version, sizeof g_version);
    g_slot.board_addr = BOARD;

    cfg.product_addr = 0x03u;
    cfg.slaves = &g_slot;
    cfg.slave_count = 1u;
    cfg.rx_buf = g_host_rx;
    cfg.rx_cap = sizeof g_host_rx;
    cfg.tx_buf = g_host_tx;
    cfg.tx_cap = sizeof g_host_tx;
    cfg.on_event = NULL;
    cfg.user = NULL;
    cfg.board_cmds = f429_board_cmds()->table;
    cfg.board_cmd_count = f429_board_cmds()->table_len;
    CHECK_EQ(ECSP_OK, ecsp_host_init(&g_host, &cfg));
}

static void setup(void)
{
    g_log_n = 0;
    g_commits = 0;
    g_keep_identity = 0;
    g_now = 0u;
    memset(&g_nvm, 0, sizeof g_nvm);
    init_slave();
    init_host();
}

static void deliver_host(void)
{
    uint8_t buf[4u * ECSP_MAX_FRAME];
    size_t n;
    size_t off = 0u;

    n = ecsp_host_tx_pending(&g_host);
    if (n > sizeof buf) {
        n = sizeof buf;
    }
    if (n == 0u) {
        return;
    }
    memcpy(buf, g_host_tx, n);
    ecsp_host_tx_drain(&g_host, n);

    while (off < n) {
        ecsp_frame_t frame;
        size_t consumed = 0u;
        ecsp_scan_t sc = ecsp_frame_scan(buf + off, n - off, &frame, &consumed);

        if (sc == ECSP_SCAN_NEED_MORE) {
            ecsp_slave_rx(&g_slave, buf + off, n - off);
            return;
        }
        if (sc != ECSP_SCAN_OK) {
            off += consumed;
            continue;
        }
        ecsp_slave_rx(&g_slave, buf + off, consumed);
        off += consumed;
    }
}

static void deliver_slave(void)
{
    uint8_t buf[4u * ECSP_MAX_FRAME];
    size_t n;
    size_t off = 0u;

    n = ecsp_slave_tx_pending(&g_slave);
    if (n > sizeof buf) {
        n = sizeof buf;
    }
    if (n == 0u) {
        return;
    }
    memcpy(buf, g_slave_tx, n);
    ecsp_slave_tx_drain(&g_slave, n);

    while (off < n) {
        ecsp_frame_t frame;
        size_t consumed = 0u;
        ecsp_scan_t sc = ecsp_frame_scan(buf + off, n - off, &frame, &consumed);

        if (sc == ECSP_SCAN_NEED_MORE) {
            ecsp_host_rx(&g_host, buf + off, n - off);
            return;
        }
        if (sc != ECSP_SCAN_OK) {
            off += consumed;
            continue;
        }
        ecsp_host_rx(&g_host, buf + off, consumed);
        off += consumed;
    }
}

static void exchange(uint32_t now)
{
    (void)ecsp_host_step(&g_host, now);
    ecsp_slave_step(&g_slave, now);
    deliver_host();
    deliver_slave();
    ecsp_slave_step(&g_slave, now);
    deliver_host();
    deliver_slave();
    (void)ecsp_host_step(&g_host, now);
}

static int bring_up(void)
{
    uint32_t t;

    for (t = 0u; t <= 200u; t += 10u) {
        exchange(t);
        if (ecsp_host_session(&g_host, BOARD) == ECSP_SESS_NORMAL) {
            g_now = t;
            return 1;
        }
    }
    return 0;
}

static int pump_until_rehandshake(uint32_t t0, uint32_t window)
{
    uint32_t dt;
    int saw_probe = 0;

    for (dt = 0u; dt <= window; dt += 10u) {
        ecsp_session_t sess;

        exchange(t0 + dt);
        sess = ecsp_host_session(&g_host, BOARD);
        if (sess == ECSP_SESS_DISCONNECTED ||
            sess == ECSP_SESS_HANDSHAKING ||
            sess == ECSP_SESS_HANDSHAKEN) {
            saw_probe = 1;
        }
        if (saw_probe != 0 && sess == ECSP_SESS_NORMAL) {
            g_now = t0 + dt;
            return 1;
        }
    }
    return 0;
}

static int submit(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    return ecsp_host_submit(&g_host, BOARD, cmd, payload, len, on_txn, NULL);
}

static void expect_answer(txn_rec_t *r, uint8_t rs,
                          const uint8_t *data, uint16_t len)
{
    CHECK(r != NULL);
    if (r == NULL) {
        return;
    }
    CHECK_EQ(ECSP_OK, r->status);
    CHECK_EQ((int)rs, (int)r->rs);
    if (len == 0u) {
        CHECK_EQ(0, (int)r->len);
    } else {
        CHECK_MEM(data, len, r->data, r->len);
    }
}

/* 0x30 is a getter: empty request, 12-byte stand-in, not the real chip. */
static void test_chip_id_is_the_stand_in(void)
{
    txn_rec_t *r;

    setup();
    CHECK(bring_up());
    CHECK_EQ(ECSP_OK, submit(0x30u, NULL, 0u));
    exchange(g_now);
    r = nth_txn(0x30u, 0);
    expect_answer(r, ECSP_RS_OK, g_chip_id, F429_CHIP_ID_LEN);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));
}

/* 0x31 is a getter. The test byte is 0x00 after power-up. */
static void test_test_byte_starts_at_zero(void)
{
    static const uint8_t zero = 0x00u;
    txn_rec_t *r;

    setup();
    CHECK(bring_up());
    CHECK_EQ(ECSP_OK, submit(0x31u, NULL, 0u));
    exchange(g_now);
    r = nth_txn(0x31u, 0);
    expect_answer(r, ECSP_RS_OK, &zero, 1u);
}

/* 0x32 is a setter. 0x31 reads back the byte that was written. */
static void test_set_test_byte_reads_back(void)
{
    static const uint8_t written = 0xA5u;
    txn_rec_t *set;
    txn_rec_t *got;

    setup();
    CHECK(bring_up());
    CHECK_EQ(ECSP_OK, submit(0x32u, &written, 1u));
    exchange(g_now);
    set = nth_txn(0x32u, 0);
    expect_answer(set, ECSP_RS_OK, NULL, 0u);

    CHECK_EQ(ECSP_OK, submit(0x31u, NULL, 0u));
    exchange(g_now);
    got = nth_txn(0x31u, 0);
    expect_answer(got, ECSP_RS_OK, &written, 1u);
}

/* Re-initialising the slave is a reset: the test byte returns to 0x00 and
 * the identity store is not written. */
static void test_reset_clears_test_byte_without_touching_identity(void)
{
    static const uint8_t written = 0x5Au;
    static const uint8_t zero = 0x00u;
    txn_rec_t *got;

    setup();
    CHECK(bring_up());
    g_nvm = g_identity;
    ecsp_slave_set_identity_commit(&g_slave, commit_identity);

    CHECK_EQ(ECSP_OK, submit(0x32u, &written, 1u));
    exchange(g_now);
    expect_answer(nth_txn(0x32u, 0), ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(ECSP_OK, submit(0x31u, NULL, 0u));
    exchange(g_now);
    expect_answer(nth_txn(0x31u, 0), ECSP_RS_OK, &written, 1u);
    CHECK_EQ(0, g_commits);

    g_identity = g_nvm;
    g_keep_identity = 1;
    init_slave();
    g_keep_identity = 0;
    ecsp_slave_set_identity_commit(&g_slave, commit_identity);
    CHECK(pump_until_rehandshake(g_now, 2000u));

    CHECK_EQ(ECSP_OK, submit(0x31u, NULL, 0u));
    exchange(g_now);
    got = nth_txn(0x31u, 1);
    expect_answer(got, ECSP_RS_OK, &zero, 1u);

    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_GET_HW_VERSION, NULL, 0u));
    exchange(g_now);
    got = nth_txn(ECSP_CMD_GET_HW_VERSION, 0);
    expect_answer(got, ECSP_RS_OK, g_version, 7u);

    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_GET_SERIAL, NULL, 0u));
    exchange(g_now);
    got = nth_txn(ECSP_CMD_GET_SERIAL, 0);
    expect_answer(got, ECSP_RS_OK, g_serial, 12u);
    CHECK_EQ(0, g_commits);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));
}

/* 0x33 is a non-idempotent action. A new transaction sequence adds one.
 * The answer is the new counter as a little-endian u32, starting from 0. */
static void test_new_sequence_returns_the_next_count(void)
{
    static const uint8_t first[4] = { 0x01u, 0x00u, 0x00u, 0x00u };
    static const uint8_t second[4] = { 0x02u, 0x00u, 0x00u, 0x00u };
    uint8_t seq_a = 0x01u;
    uint8_t seq_b = 0x02u;

    setup();
    CHECK(bring_up());
    CHECK_EQ(ECSP_OK, submit(0x33u, &seq_a, 1u));
    exchange(g_now);
    expect_answer(nth_txn(0x33u, 0), ECSP_RS_OK, first, 4u);

    CHECK_EQ(ECSP_OK, submit(0x33u, &seq_b, 1u));
    exchange(g_now);
    expect_answer(nth_txn(0x33u, 1), ECSP_RS_OK, second, 4u);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));
}

/* The same transaction sequence replays the original answer. The count
 * does not increase, so the next new sequence is still the following value. */
static void test_replay_keeps_the_count(void)
{
    static const uint8_t first[4] = { 0x01u, 0x00u, 0x00u, 0x00u };
    static const uint8_t second[4] = { 0x02u, 0x00u, 0x00u, 0x00u };
    uint8_t seq_a = 0x11u;
    uint8_t seq_b = 0x12u;

    setup();
    CHECK(bring_up());
    CHECK_EQ(ECSP_OK, submit(0x33u, &seq_a, 1u));
    exchange(g_now);
    expect_answer(nth_txn(0x33u, 0), ECSP_RS_OK, first, 4u);

    CHECK_EQ(ECSP_OK, submit(0x33u, &seq_a, 1u));
    exchange(g_now);
    expect_answer(nth_txn(0x33u, 1), ECSP_RS_OK, first, 4u);

    CHECK_EQ(ECSP_OK, submit(0x33u, &seq_b, 1u));
    exchange(g_now);
    expect_answer(nth_txn(0x33u, 2), ECSP_RS_OK, second, 4u);
}

static void drain_host(void)
{
    ecsp_host_tx_drain(&g_host, ecsp_host_tx_pending(&g_host));
}

static void step_host(uint32_t now)
{
    CHECK_EQ(ECSP_OK, ecsp_host_step(&g_host, now));
}

/* Silence, then the same request, then the spec's answer so the next
 * command can be submitted. The slave is not stepped. */
static void host_retries_at(uint8_t cmd, const uint8_t *payload, uint16_t plen,
                            const uint8_t *frame, size_t frame_len,
                            uint32_t ceiling, const uint8_t *answer,
                            size_t answer_len, uint32_t *now)
{
    uint32_t t = *now;

    g_log_n = 0;
    CHECK_EQ(ECSP_OK, submit(cmd, payload, plen));
    step_host(t);
    CHECK_MEM(frame, frame_len, g_host_tx, ecsp_host_tx_pending(&g_host));
    drain_host();

    step_host(t + ceiling - 1u);
    CHECK_EQ(0u, ecsp_host_tx_pending(&g_host));
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, BOARD));
    CHECK(nth_txn(cmd, 0) == NULL);

    step_host(t + ceiling);
    CHECK_MEM(frame, frame_len, g_host_tx, ecsp_host_tx_pending(&g_host));
    drain_host();

    ecsp_host_rx(&g_host, answer, answer_len);
    step_host(t + ceiling);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, BOARD));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));
    *now = t + ceiling;
}

/* F429's table states 100 ms on 0x32 and 0x33, and no millisecond on
 * 0x30 and 0x31. Frames are the board spec examples. */
static void test_f429_ceilings_are_per_command(void)
{
    static const uint8_t written[1] = { 0xA5u };
    static const uint8_t seq[1] = { 0x01u };
    static const uint8_t get_chip_req[] = {
        0xFA, 0x09, 0x00, 0x03, 0x30, 0x02, 0x9D, 0xC0, 0x0D
    };
    static const uint8_t get_byte_req[] = {
        0xFA, 0x09, 0x00, 0x03, 0x31, 0x02, 0x9C, 0x50, 0x0D
    };
    static const uint8_t set_byte_req[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x32, 0x02, 0xA5, 0x60, 0x21, 0x0D
    };
    static const uint8_t bump_req[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x33, 0x02, 0x01, 0x30, 0x5A, 0x0D
    };
    static const uint8_t set_byte_ok[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x32, 0x02, 0x00, 0xA0, 0x5A, 0x0D
    };
    static const uint8_t bump_ok[] = {
        0xFA, 0x0E, 0x00, 0x03, 0x33, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x52, 0xFA, 0x0D
    };
    static const uint8_t get_chip_ok[] = {
        0xFA, 0x16, 0x00, 0x03, 0x30, 0x02, 0x00,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67,
        0x75, 0xFB, 0x0D
    };
    static const uint8_t get_byte_ok[] = {
        0xFA, 0x0B, 0x00, 0x03, 0x31, 0x02, 0x00, 0x00, 0x4A, 0xFC, 0x0D
    };
    uint32_t now;

    setup();
    CHECK(bring_up());
    now = g_now;

    host_retries_at(0x32u, written, 1u, set_byte_req, sizeof set_byte_req,
                    100u, set_byte_ok, sizeof set_byte_ok, &now);
    host_retries_at(0x33u, seq, 1u, bump_req, sizeof bump_req,
                    100u, bump_ok, sizeof bump_ok, &now);
    host_retries_at(0x30u, NULL, 0u, get_chip_req, sizeof get_chip_req,
                    50u, get_chip_ok, sizeof get_chip_ok, &now);
    host_retries_at(0x31u, NULL, 0u, get_byte_req, sizeof get_byte_req,
                    50u, get_byte_ok, sizeof get_byte_ok, &now);
}

/* A board command other than 0x30-0x33 answers 0x04. */
static void test_other_board_command_is_unsupported(void)
{
    txn_rec_t *r;

    setup();
    CHECK(bring_up());
    CHECK_EQ(ECSP_OK, submit(0x34u, NULL, 0u));
    exchange(g_now);
    r = nth_txn(0x34u, 0);
    expect_answer(r, ECSP_RS_UNSUPPORTED, NULL, 0u);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, BOARD));

    CHECK_EQ(ECSP_OK, submit(0xEFu, NULL, 0u));
    exchange(g_now);
    r = nth_txn(0xEFu, 0);
    expect_answer(r, ECSP_RS_UNSUPPORTED, NULL, 0u);
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, BOARD));
}

int main(void)
{
    test_chip_id_is_the_stand_in();
    test_test_byte_starts_at_zero();
    test_set_test_byte_reads_back();
    test_reset_clears_test_byte_without_touching_identity();
    test_new_sequence_returns_the_next_count();
    test_replay_keeps_the_count();
    test_other_board_command_is_unsupported();
    test_f429_ceilings_are_per_command();
    return ecsp_test_report();
}
