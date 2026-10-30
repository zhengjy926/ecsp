/*
 * Two contexts, one virtual link (ticket #12).
 *
 * Seam, the one parent ticket #1 already fixed for cross-module behavior:
 * a host context and a slave context, with a byte array between them.
 * The link can drop a frame, hold one, split one, or flip a CRC byte.
 * Assertions name a spec section or a golden vector. They do not read
 * context fields.
 *
 * What this seam catches: retransmission still means one execution, a bad
 * CRC dies quietly, a restarted slave does not hand back the previous
 * answer, a lost result event is closed by get-result, and a reset during
 * a long-running command does not invent that command's outcome.
 * What it misses: two rejection reasons on one frame (that stays on the
 * slave-pipeline seam) and a CRC byte that is 0xFA (that stays on the
 * vector seam).
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

#define ECSP_VECTOR(name, hex, ref) static const char name##_hex[] = hex;
#include "vectors.h"
#undef ECSP_VECTOR

#define BOARD      0x02u
#define BOARD_ACT  0x30u
#define QUERY_MS   50u
#define ACTION_MS  100u
#define DROPOUT_MS 3000u
#define REPORT_MS  1000u
#define LOG_MAX    8

static const uint8_t version_1_0_0_0[7] = {
    '1', '.', '0', '.', '0', '.', '0'
};

typedef struct {
    uint8_t cmd;
    int status;
    uint8_t rs;
    uint8_t data[4];
    uint16_t len;
} txn_rec_t;

static uint8_t g_host_rx[4u * ECSP_MAX_FRAME];
static uint8_t g_host_tx[4u * ECSP_MAX_FRAME];
static uint8_t g_slave_rx[4u * ECSP_MAX_FRAME];
static uint8_t g_slave_tx[4u * ECSP_MAX_FRAME];
static ecsp_host_t g_host;
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;
static ecsp_host_slave_cfg_t g_slot;
static txn_rec_t g_log[LOG_MAX];
static int g_log_n;
static int g_act_calls;
static int g_abort_calls;
static uint8_t g_abort_cmd;
static int g_resets;
static uint32_t g_now;

static int g_drop_half;
static unsigned g_host_seq;
static unsigned g_slave_seq;
static int g_corrupt;
static int g_drop_events;
static int g_drop_slave_once;
static int g_mute_slave;
static int g_cut_on;
static size_t g_cut_bytes;
static int g_auto_reset;

static int g_slave_frames;
static int g_result_events;
static int g_selftest_reqs;
static int g_get_selftest_reqs;
static uint8_t g_result_frame[ECSP_MAX_FRAME];
static size_t g_result_len;
static uint8_t g_by_cmd[0x20][ECSP_MAX_FRAME];
static size_t g_by_cmd_len[0x20];
static uint8_t g_host_by_cmd[0x20][ECSP_MAX_FRAME];
static size_t g_host_by_cmd_len[0x20];

static int on_act(ecsp_slave_t *s, const ecsp_frame_t *req,
                  uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)cap;
    g_act_calls++;
    resp[0] = 0x5Au;
    *resp_len = 1u;
    return ECSP_RS_OK;
}

static const ecsp_cmd_entry_t g_board[] = {
    { BOARD_ACT, ECSP_CMD_TXN_SEQ, 1u, 1u, NULL, on_act }
};

static int abort_ok(ecsp_slave_t *s, uint8_t cmd)
{
    (void)s;
    g_abort_calls++;
    g_abort_cmd = cmd;
    return ECSP_OK;
}

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

static void init_slave(void)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t hw[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    memcpy(g_identity.hardware_version.b, hw, sizeof hw);
    memcpy(g_identity.serial_number.b, sn, sizeof sn);

    cfg.product_addr = 0x03u;
    cfg.board_addr = BOARD;
    memcpy(cfg.version.b, version_1_0_0_0, sizeof version_1_0_0_0);
    cfg.identity = &g_identity;
    cfg.fault_word = 0x01u;
    cfg.cmds.table = g_board;
    cfg.cmds.table_len = sizeof g_board / sizeof g_board[0];
    cfg.rx_buf = g_slave_rx;
    cfg.rx_cap = sizeof g_slave_rx;
    cfg.tx_buf = g_slave_tx;
    cfg.tx_cap = sizeof g_slave_tx;
    CHECK_EQ(ECSP_OK, ecsp_slave_init(&g_slave, &cfg));
}

static void init_host(void)
{
    ecsp_host_cfg_t cfg;

    memcpy(g_slot.expect_version.b, version_1_0_0_0, sizeof version_1_0_0_0);
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
    CHECK_EQ(ECSP_OK, ecsp_host_init(&g_host, &cfg));
}

static void setup(void)
{
    g_log_n = 0;
    g_act_calls = 0;
    g_abort_calls = 0;
    g_abort_cmd = 0u;
    g_resets = 0;
    g_now = 0u;
    g_drop_half = 0;
    g_host_seq = 0u;
    g_slave_seq = 0u;
    g_corrupt = 0;
    g_drop_events = 0;
    g_drop_slave_once = 0;
    g_mute_slave = 0;
    g_cut_on = 0;
    g_cut_bytes = 0u;
    g_auto_reset = 0;
    g_slave_frames = 0;
    g_result_events = 0;
    g_selftest_reqs = 0;
    g_get_selftest_reqs = 0;
    g_result_len = 0u;
    memset(g_by_cmd_len, 0, sizeof g_by_cmd_len);
    memset(g_host_by_cmd_len, 0, sizeof g_host_by_cmd_len);
    init_slave();
    init_host();
}

static int half_drop(unsigned *seq)
{
    unsigned i;

    if (g_drop_half == 0) {
        return 0;
    }
    i = *seq;
    *seq = i + 1u;
    return ((i & 1u) != 0u) ? 1 : 0;
}

static void remember(uint8_t cmd, const uint8_t *frame, size_t n, int from_host)
{
    if (cmd >= 0x20u || n > ECSP_MAX_FRAME) {
        return;
    }
    if (from_host) {
        memcpy(g_host_by_cmd[cmd], frame, n);
        g_host_by_cmd_len[cmd] = n;
    } else {
        memcpy(g_by_cmd[cmd], frame, n);
        g_by_cmd_len[cmd] = n;
    }
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
    if (g_cut_on != 0 && n > g_cut_bytes) {
        n = g_cut_bytes;
        g_cut_on = 0;
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
        uint8_t tmp[ECSP_MAX_FRAME];

        if (sc == ECSP_SCAN_NEED_MORE) {
            ecsp_slave_rx(&g_slave, buf + off, n - off);
            return;
        }
        if (sc != ECSP_SCAN_OK) {
            off += consumed;
            continue;
        }
        remember(frame.cmd, buf + off, consumed, 1);
        if (frame.cmd == (uint8_t)ECSP_CMD_CONTROL_SELFTEST) {
            g_selftest_reqs++;
        }
        if (frame.cmd == (uint8_t)ECSP_CMD_GET_SELFTEST) {
            g_get_selftest_reqs++;
        }
        if (half_drop(&g_host_seq)) {
            off += consumed;
            continue;
        }
        if (g_corrupt != 0 && consumed >= 3u && consumed <= sizeof tmp) {
            memcpy(tmp, buf + off, consumed);
            tmp[consumed - 3u] ^= 0xFFu;
            g_corrupt = 0;
            ecsp_slave_rx(&g_slave, tmp, consumed);
        } else {
            ecsp_slave_rx(&g_slave, buf + off, consumed);
        }
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
    if (g_mute_slave != 0) {
        return;
    }

    while (off < n) {
        ecsp_frame_t frame;
        size_t consumed = 0u;
        ecsp_scan_t sc = ecsp_frame_scan(buf + off, n - off, &frame, &consumed);
        int drop;

        if (sc == ECSP_SCAN_NEED_MORE) {
            ecsp_host_rx(&g_host, buf + off, n - off);
            return;
        }
        if (sc != ECSP_SCAN_OK) {
            off += consumed;
            continue;
        }
        g_slave_frames++;
        remember(frame.cmd, buf + off, consumed, 0);
        drop = 0;
        if (frame.cmd == (uint8_t)ECSP_CMD_SELFTEST_REPORT) {
            g_result_events++;
            if (consumed <= sizeof g_result_frame) {
                memcpy(g_result_frame, buf + off, consumed);
                g_result_len = consumed;
            }
            if (g_drop_events != 0) {
                drop = 1;
            }
        }
        if (g_drop_slave_once != 0) {
            g_drop_slave_once = 0;
            drop = 1;
        }
        if (half_drop(&g_slave_seq)) {
            drop = 1;
        }
        if (drop == 0) {
            ecsp_host_rx(&g_host, buf + off, consumed);
        }
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
    if (g_auto_reset != 0 && ecsp_slave_reset_requested(&g_slave) != 0) {
        init_slave();
        g_resets++;
    }
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

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static size_t decode_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0u;

    while (*s != '\0') {
        int hi;
        int lo;

        while (*s == ' ') {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        hi = hex_nibble((unsigned char)*s);
        s++;
        lo = hex_nibble((unsigned char)*s);
        s++;
        if (hi < 0 || lo < 0 || n >= cap) {
            return 0u;
        }
        out[n] = (uint8_t)((hi << 4) | lo);
        n++;
    }
    return n;
}

static void expect_vector_named(const char *file, int line, const char *name,
                                const char *hex, const uint8_t *got,
                                size_t got_len)
{
    uint8_t want[ECSP_MAX_FRAME];
    size_t n = decode_hex(hex, want, sizeof want);
    char msg[96];

    ecsp_test_checks++;
    if (n == 0u || n != got_len || memcmp(want, got, n) != 0) {
        (void)snprintf(msg, sizeof msg, "vector %s", name);
        ecsp_test_fail(file, line, msg);
    }
}

#define EXPECT_VECTOR(sym, got, got_len) \
    expect_vector_named(__FILE__, __LINE__, #sym, sym##_hex, (got), (got_len))

#define CHECK_SPEC(spec, cond) \
    do { \
        ecsp_test_checks++; \
        if (!(cond)) { \
            ecsp_test_fail(__FILE__, __LINE__, spec " | " #cond); \
        } \
    } while (0)

static int submit(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    return ecsp_host_submit(&g_host, BOARD, cmd, payload, len, on_txn, NULL);
}

static void pump_until_normal(uint32_t t0, uint32_t window)
{
    uint32_t dt;

    for (dt = 0u; dt <= window; dt += 10u) {
        exchange(t0 + dt);
        if (ecsp_host_session(&g_host, BOARD) == ECSP_SESS_NORMAL) {
            g_now = t0 + dt;
            return;
        }
    }
}

/* After a reset the host is still in normal communication until the slave's
 * handshake report arrives (spec 4.1.4). Returning on the session that was
 * already normal would skip that probe. */
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

/* spec 4.3: the result event closes the long-running command. No get-result
 * is sent when the event arrives. */
static void test_result_event_closes_the_long_run(void)
{
    uint8_t start[2];
    txn_rec_t *r;

    setup();
    CHECK(bring_up());
    EXPECT_VECTOR(version_resp, g_by_cmd[ECSP_CMD_GET_VERSION],
                  g_by_cmd_len[ECSP_CMD_GET_VERSION]);

    start[0] = 0x01u;
    start[1] = (uint8_t)ECSP_CTRL_START;
    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_CONTROL_SELFTEST, start, 2u));
    exchange(g_now);
    CHECK_SPEC("spec table 4-2",
               ecsp_host_txn(&g_host, BOARD) == ECSP_TXN_WAIT_RESULT);
    CHECK(nth_txn(ECSP_CMD_CONTROL_SELFTEST, 0) == NULL);

    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    exchange(g_now);
    r = nth_txn(ECSP_CMD_CONTROL_SELFTEST, 0);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK_SPEC("spec 4.3", r->status == ECSP_OK);
        CHECK_EQ(1, (int)r->len);
        CHECK_SPEC("spec 5.2.10", r->data[0] == 0x00u);
    }
    EXPECT_VECTOR(selftest_report, g_result_frame, g_result_len);
    CHECK_SPEC("spec 4.3", g_get_selftest_reqs == 0);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, BOARD));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));
}

/* spec 4.1.1 → 9.2 → 5.2.18 → 4.3 (lost event) → 7.2.1 → 4.1 again. */
static void test_lifecycle_loss_dropout_and_reconnect(void)
{
    uint8_t start[2];
    txn_rec_t *fault;
    txn_rec_t *done;
    uint32_t t;
    uint32_t dt;
    int closed = 0;

    setup();
    CHECK(bring_up());
    CHECK_SPEC("spec 9.2", ecsp_host_session(&g_host, BOARD) == ECSP_SESS_NORMAL);
    EXPECT_VECTOR(version_resp, g_by_cmd[ECSP_CMD_GET_VERSION],
                  g_by_cmd_len[ECSP_CMD_GET_VERSION]);

    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_GET_FAULT, NULL, 0u));
    exchange(g_now);
    fault = nth_txn(ECSP_CMD_GET_FAULT, 0);
    CHECK(fault != NULL);
    if (fault != NULL) {
        CHECK_SPEC("spec 5.2.18", fault->status == ECSP_OK);
        CHECK_EQ((int)ECSP_RS_OK, (int)fault->rs);
        CHECK_EQ(1, (int)fault->len);
        CHECK_EQ(0x01, (int)fault->data[0]);
    }
    EXPECT_VECTOR(fault_query_resp, g_by_cmd[ECSP_CMD_GET_FAULT],
                  g_by_cmd_len[ECSP_CMD_GET_FAULT]);

    start[0] = 0x01u;
    start[1] = (uint8_t)ECSP_CTRL_START;
    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_CONTROL_SELFTEST, start, 2u));
    exchange(g_now);
    CHECK_SPEC("spec table 4-2",
               ecsp_host_txn(&g_host, BOARD) == ECSP_TXN_WAIT_RESULT);

    g_drop_events = 1;
    CHECK_EQ(ECSP_OK, ecsp_slave_long_run_complete(&g_slave, ECSP_RS_OK));
    exchange(g_now);
    EXPECT_VECTOR(selftest_report, g_result_frame, g_result_len);
    CHECK(nth_txn(ECSP_CMD_CONTROL_SELFTEST, 0) == NULL);

    t = g_now;
    for (dt = 10u; dt <= 2100u; dt += 10u) {
        exchange(t + dt);
        if (nth_txn(ECSP_CMD_CONTROL_SELFTEST, 0) != NULL) {
            g_now = t + dt;
            closed = 1;
            break;
        }
    }
    CHECK_SPEC("spec 4.3", closed);
    done = nth_txn(ECSP_CMD_CONTROL_SELFTEST, 0);
    if (done != NULL) {
        CHECK_SPEC("spec 4.3", done->status == ECSP_OK);
        CHECK_SPEC("spec 4.3", done->rs == (uint8_t)ECSP_RS_OK);
        CHECK_EQ(1, (int)done->len);
        CHECK_EQ(0x00, (int)done->data[0]);
    }
    CHECK_SPEC("spec 4.3", g_get_selftest_reqs == 1);
    EXPECT_VECTOR(selftest_query_req, g_host_by_cmd[ECSP_CMD_GET_SELFTEST],
                  g_host_by_cmd_len[ECSP_CMD_GET_SELFTEST]);
    CHECK_EQ(ECSP_TXN_IDLE, ecsp_host_txn(&g_host, BOARD));
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));

    g_mute_slave = 1;
    exchange(g_now + DROPOUT_MS - 10u);
    CHECK_SPEC("spec 7.2.1",
               ecsp_host_session(&g_host, BOARD) == ECSP_SESS_NORMAL);
    exchange(g_now + DROPOUT_MS);
    CHECK_SPEC("spec 7.2.1",
               ecsp_host_session(&g_host, BOARD) == ECSP_SESS_OFFLINE);
    CHECK_SPEC("spec table 4-1",
               submit(ECSP_CMD_GET_FAULT, NULL, 0u) == ECSP_E_STATE);

    g_mute_slave = 0;
    t = g_now + DROPOUT_MS;
    init_slave();
    pump_until_normal(t, 2000u);
    CHECK_SPEC("spec 4.1.2",
               ecsp_host_session(&g_host, BOARD) == ECSP_SESS_NORMAL);

    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_GET_FAULT, NULL, 0u));
    exchange(g_now);
    fault = nth_txn(ECSP_CMD_GET_FAULT, 0);
    CHECK(fault != NULL);
    if (fault != NULL) {
        CHECK_SPEC("spec 4.1", fault->status == ECSP_OK);
        CHECK_EQ(0x01, (int)fault->data[0]);
    }
    EXPECT_VECTOR(fault_query_resp, g_by_cmd[ECSP_CMD_GET_FAULT],
                  g_by_cmd_len[ECSP_CMD_GET_FAULT]);
}

/* Every other frame, each direction. The first request is delivered and its
 * answer is not; the retransmission is not delivered; the one after that is,
 * and so is its answer. spec 4.2: the handler runs once. */
static void test_half_loss_runs_the_action_once(void)
{
    uint8_t seq[1];
    txn_rec_t *r;
    uint32_t t;
    uint32_t dt;
    int closed = 0;

    setup();
    CHECK(bring_up());
    g_drop_half = 1;
    g_host_seq = 0u;
    g_slave_seq = 1u;

    seq[0] = 0x11u;
    t = g_now;
    CHECK_EQ(ECSP_OK, submit(BOARD_ACT, seq, 1u));
    for (dt = 0u; dt <= 400u; dt += ACTION_MS) {
        exchange(t + dt);
        r = nth_txn(BOARD_ACT, 0);
        if (r != NULL) {
            closed = 1;
            break;
        }
    }
    r = nth_txn(BOARD_ACT, 0);
    CHECK_SPEC("spec 4.2", closed);
    CHECK_SPEC("spec 4.2", g_act_calls == 1);
    if (r != NULL) {
        CHECK_SPEC("spec 4.2", r->status == ECSP_OK);
        CHECK_EQ((int)ECSP_RS_OK, (int)r->rs);
        CHECK_EQ(1, (int)r->len);
        CHECK_EQ(0x5A, (int)r->data[0]);
    }
    CHECK_EQ(ECSP_SESS_NORMAL, ecsp_host_session(&g_host, BOARD));
}

/* spec table 6-2: a bad CRC drops the whole frame and produces no answer.
 * The retry is the same request, and the command then completes. */
static void test_bad_crc_is_discarded_and_the_retry_continues(void)
{
    txn_rec_t *r;
    uint32_t t;

    setup();
    CHECK(bring_up());
    t = g_now;
    g_slave_frames = 0;
    g_corrupt = 1;
    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_GET_FAULT, NULL, 0u));
    exchange(t);
    CHECK_SPEC("spec table 6-2", g_slave_frames == 0);
    CHECK(nth_txn(ECSP_CMD_GET_FAULT, 0) == NULL);
    CHECK_EQ(ECSP_TXN_WAIT_RESP, ecsp_host_txn(&g_host, BOARD));

    exchange(t + QUERY_MS);
    r = nth_txn(ECSP_CMD_GET_FAULT, 0);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK_SPEC("spec 6.2", r->status == ECSP_OK);
        CHECK_EQ((int)ECSP_RS_OK, (int)r->rs);
        CHECK_EQ(0x01, (int)r->data[0]);
    }
    EXPECT_VECTOR(fault_query_resp, g_by_cmd[ECSP_CMD_GET_FAULT],
                  g_by_cmd_len[ECSP_CMD_GET_FAULT]);
}

/* spec 3.1: fewer bytes than the length field is not a frame, so the slave
 * sends nothing. spec 7.1: the finished answer is then held 20 ms, still
 * inside the 50 ms query window, and the command completes. */
static void test_split_frame_is_reassembled(void)
{
    txn_rec_t *r;
    uint32_t t;

    setup();
    CHECK(bring_up());
    t = g_now;
    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_GET_FAULT, NULL, 0u));
    (void)ecsp_host_step(&g_host, t);

    g_cut_bytes = 4u;
    g_cut_on = 1;
    deliver_host();
    ecsp_slave_step(&g_slave, t);
    CHECK_SPEC("spec 3.1", ecsp_slave_tx_pending(&g_slave) == 0u);

    deliver_host();
    ecsp_slave_step(&g_slave, t);
    CHECK(ecsp_slave_tx_pending(&g_slave) > 0u);
    (void)ecsp_host_step(&g_host, t + 20u);
    CHECK_SPEC("spec 7.1", ecsp_host_txn(&g_host, BOARD) == ECSP_TXN_WAIT_RESP);
    deliver_slave();
    (void)ecsp_host_step(&g_host, t + 20u);

    r = nth_txn(ECSP_CMD_GET_FAULT, 0);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK_SPEC("spec 7.1", r->status == ECSP_OK);
        CHECK_EQ(0x01, (int)r->data[0]);
    }
    EXPECT_VECTOR(fault_query_resp, g_by_cmd[ECSP_CMD_GET_FAULT],
                  g_by_cmd_len[ECSP_CMD_GET_FAULT]);
}

/* spec 4.2: re-initialising the slave is the soft reset. The host's retry
 * of the same sequence must not come back as the answer from the previous
 * run. spec 4.1.2: the fresh slave answers 0x10. */
static void test_restarted_slave_does_not_return_the_previous_answer(void)
{
    uint8_t seq[1];
    txn_rec_t *r;
    uint32_t t;

    setup();
    CHECK(bring_up());
    t = g_now;
    seq[0] = 0x11u;
    g_drop_slave_once = 1;
    CHECK_EQ(ECSP_OK, submit(BOARD_ACT, seq, 1u));
    exchange(t);
    CHECK_SPEC("spec 4.2", g_act_calls == 1);
    CHECK(nth_txn(BOARD_ACT, 0) == NULL);

    init_slave();
    exchange(t + ACTION_MS);
    r = nth_txn(BOARD_ACT, 0);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK_SPEC("spec 4.1.2", r->status == ECSP_OK);
        CHECK_SPEC("spec 4.2", r->rs == (uint8_t)ECSP_RS_NOT_HANDSHAKEN);
        CHECK_SPEC("spec 4.2", r->len == 0u);
    }
    CHECK_SPEC("spec 4.2", g_act_calls == 1);
}

/* spec 4.4: 0x07 while busy enters the safe state, answers 0x00, resets,
 * and does not emit the result event. After the new handshake the outcome
 * is whatever get-result says (spec 5.2.9: 0x13), not an assumed success.
 * The in-flight start is not sent again. */
static void test_reset_during_long_run_does_not_assume_the_outcome(void)
{
    uint8_t start[2];
    uint8_t seq[1];
    txn_rec_t *abandoned;
    txn_rec_t *reset;
    txn_rec_t *query;
    int starts;
    uint32_t t;

    setup();
    CHECK(bring_up());
    ecsp_slave_set_safe_abort(&g_slave, abort_ok);

    start[0] = 0x01u;
    start[1] = (uint8_t)ECSP_CTRL_START;
    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_CONTROL_SELFTEST, start, 2u));
    exchange(g_now);
    CHECK_EQ(ECSP_TXN_WAIT_RESULT, ecsp_host_txn(&g_host, BOARD));

    g_auto_reset = 1;
    seq[0] = 0x02u;
    starts = g_selftest_reqs;
    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_SYSTEM_RESET, seq, 1u));
    CHECK(nth_txn(ECSP_CMD_CONTROL_SELFTEST, 0) == NULL);

    t = g_now;
    exchange(t);
    g_auto_reset = 0;
    abandoned = nth_txn(ECSP_CMD_CONTROL_SELFTEST, 0);
    CHECK(abandoned != NULL);
    if (abandoned != NULL) {
        CHECK_SPEC("spec 4.4", abandoned->status == ECSP_E_RESET);
    }
    CHECK_SPEC("spec 4.4", g_abort_calls == 1);
    CHECK_SPEC("spec 4.4", g_abort_cmd == (uint8_t)ECSP_CMD_CONTROL_SELFTEST);
    CHECK_SPEC("spec 4.4", g_result_events == 0);
    CHECK_SPEC("spec 4.2", g_resets == 1);
    reset = nth_txn(ECSP_CMD_SYSTEM_RESET, 0);
    CHECK(reset != NULL);
    if (reset != NULL) {
        CHECK_SPEC("spec 5.2.7", reset->status == ECSP_OK);
        CHECK_SPEC("spec 5.2.7", reset->rs == (uint8_t)ECSP_RS_OK);
    }

    CHECK_SPEC("spec 4.1.4", pump_until_rehandshake(t, 2000u) == 1);
    CHECK_SPEC("spec 4.4", ecsp_host_session(&g_host, BOARD) == ECSP_SESS_NORMAL);
    CHECK_SPEC("spec 4.4", g_selftest_reqs == starts);
    CHECK_SPEC("spec 4.4", g_result_events == 0);

    CHECK_EQ(ECSP_OK, submit(ECSP_CMD_GET_SELFTEST, NULL, 0u));
    exchange(g_now);
    query = nth_txn(ECSP_CMD_GET_SELFTEST, 0);
    CHECK(query != NULL);
    if (query != NULL) {
        CHECK_SPEC("spec 5.2.9", query->status == ECSP_OK);
        CHECK_SPEC("spec 4.4", query->rs == (uint8_t)ECSP_RS_INVALID);
        CHECK_SPEC("spec 5.2.9", query->len == 0u);
    }
}

int main(void)
{
    test_result_event_closes_the_long_run();
    test_lifecycle_loss_dropout_and_reconnect();
    test_half_loss_runs_the_action_once();
    test_bad_crc_is_discarded_and_the_retry_continues();
    test_split_frame_is_reassembled();
    test_restarted_slave_does_not_return_the_previous_answer();
    test_reset_during_long_run_does_not_assume_the_outcome();
    return ecsp_test_report();
}
