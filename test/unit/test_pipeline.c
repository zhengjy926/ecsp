/*
 * L3 slave pipeline priority (ticket #5, ADR-0002).
 *
 * The engine walks a frame through one fixed decision order. When one frame
 * matches several rejection conditions, the answer is the code of the earliest
 * step:
 *
 *   1 framing (silent) -> 2 address (silent) -> 3 command (0x04) ->
 *   4 session (0x10) -> 5 length (0x02) -> ... -> 8 parameter (0x03) ->
 *   9 handler (its return code)
 *
 * These tests are the executable form of that order. They also pin down the
 * handler contract: a handler is only reached after every gate, and the
 * engine returns the handler's code untouched. The fake handler deliberately
 * repeats some of the pipeline's own checks; those checks must be unreachable
 * because the pipeline already rejected (or accepted) the frame.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

#define FAKE_CMD           0x30u
#define FAKE_REQ_LEN       2u
#define FAKE_ANY_CMD       0x31u
#define FAKE_VALIDATED_CMD 0x32u
#define CACHED_CMD         0x33u
#define PREEMPT_CMD        0x34u
#define CONCURRENT_CMD     0x35u

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;

static int g_fake_calls;
static int g_fake_rs;
static int g_any_calls;
static int g_validated_calls;
static int g_cached_calls;
static int g_preempt_calls;
static int g_concurrent_calls;

static int fake_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                        uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)resp;
    (void)cap;

    g_fake_calls++;

    /* Deliberate duplicate of the pipeline's length gate (ADR-0002 forbids
     * this in real handlers): the engine must have rejected a wrong-length
     * frame before ever reaching this line. */
    if (req->payload_len != FAKE_REQ_LEN) {
        return (int)ECSP_RS_BAD_LEN;
    }

    *resp_len = 0u;
    return g_fake_rs;
}

/* A board command that declares a variable-length payload: the pipeline must
 * skip the length gate and let the handler decide. */
static int any_length_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                              uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)resp;
    (void)cap;
    g_any_calls++;
    *resp_len = 0u;
    return ECSP_RS_OK;
}

/* A board command whose variable-length payload is gated by a validator
 * before the handler: 0x02 for a bad length, 0x03 for a bad value. */
static int validated_validator(const ecsp_frame_t *req)
{
    if (req->payload_len != 3u) {
        return ECSP_E_LEN;
    }
    if (req->payload[0] != 0xAAu) {
        return ECSP_E_VAL;
    }
    return ECSP_OK;
}

static int validated_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                             uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)resp;
    (void)cap;
    g_validated_calls++;
    *resp_len = 0u;
    return ECSP_RS_OK;
}

static int cached_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                          uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)resp;
    (void)cap;
    g_cached_calls++;
    *resp_len = 0u;
    return ECSP_RS_OK;
}

static int preempt_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                           uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)resp;
    (void)cap;
    g_preempt_calls++;
    *resp_len = 0u;
    return ECSP_RS_OK;
}

static int concurrent_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                              uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)resp;
    (void)cap;
    g_concurrent_calls++;
    *resp_len = 0u;
    return ECSP_RS_OK;
}

static const ecsp_cmd_entry_t g_board_table[] = {
    { FAKE_CMD, ECSP_CMD_NONE, FAKE_REQ_LEN, 0u, NULL, fake_handler },
    { FAKE_ANY_CMD, ECSP_CMD_NONE, ECSP_CMD_LEN_ANY, 0u, NULL,
      any_length_handler },
    { FAKE_VALIDATED_CMD, ECSP_CMD_NONE, ECSP_CMD_LEN_ANY, 0u,
      validated_validator, validated_handler },
    { CACHED_CMD, ECSP_CMD_TXN_SEQ, 1u, 0u, NULL, cached_handler },
    { PREEMPT_CMD, ECSP_CMD_PREEMPTIVE, 1u, 0u, NULL, preempt_handler },
    { CONCURRENT_CMD, ECSP_CMD_CONCURRENT, 1u, 0u, NULL, concurrent_handler }
};

static void init_slave(void)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t sw_version[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t hw_version[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t serial[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_fake_calls = 0;
    g_fake_rs = 0x55;
    g_any_calls = 0;
    g_validated_calls = 0;
    g_cached_calls = 0;
    g_preempt_calls = 0;
    g_concurrent_calls = 0;

    memcpy(g_identity.hardware_version.b, hw_version, sizeof hw_version);
    memcpy(g_identity.serial_number.b, serial, sizeof serial);

    cfg.product_addr = 0x03u;
    cfg.board_addr   = 0x02u;
    memcpy(cfg.version.b, sw_version, sizeof sw_version);
    cfg.identity       = &g_identity;
    cfg.fault_word     = 0x00u;
    cfg.cmds.table     = g_board_table;
    cfg.cmds.table_len = sizeof g_board_table / sizeof g_board_table[0];
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    CHECK_EQ(0, ecsp_slave_init(&g_slave, &cfg));
}

static size_t build_frame(uint8_t *buf, uint8_t product, uint8_t board,
                          uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    return ecsp_frame_encode(buf, ECSP_MAX_FRAME, product, board, cmd,
                             payload, len);
}

/* Feed one frame from this node (P=0x03/B=0x02) and advance the engine. */
static void send(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n = build_frame(frame, 0x03u, 0x02u, cmd, payload, len);

    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, 0u);
}

/* Feed one frame from an arbitrary address. */
static void send_from(uint8_t product, uint8_t board, uint8_t cmd,
                      const uint8_t *payload, uint16_t len)
{
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n = build_frame(frame, product, board, cmd, payload, len);

    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, 0u);
}

static void drain(void)
{
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));
}

static void handshake(void)
{
    send(ECSP_CMD_HANDSHAKE, NULL, 0u);
    CHECK_EQ(0, g_fake_calls);
    drain();
}

/* Assert the whole transmit FIFO is exactly one response frame carrying
 * [rs] + data. */
static void expect_answer(uint8_t cmd, uint8_t rs,
                          const uint8_t *data, uint16_t data_len)
{
    uint8_t payload[ECSP_MAX_FRAME];
    uint8_t want[ECSP_MAX_FRAME];
    size_t want_len;

    payload[0] = rs;
    if (data_len > 0u) {
        memcpy(&payload[1], data, data_len);
    }
    want_len = build_frame(want, 0x03u, 0x02u, cmd, payload,
                           (uint16_t)(1u + data_len));

    CHECK_EQ(want_len, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(want, want_len, g_tx, ecsp_slave_tx_pending(&g_slave));
    drain();
}

static void test_address_mismatch_is_silent_before_unknown_command(void)
{
    /* ADR-0002 step 2 before step 3: the frame is not even examined for its
     * command code when the address does not match. */
    init_slave();
    send_from(0x04u, 0x02u, 0x7Fu, NULL, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    send_from(0x03u, 0x05u, 0x7Fu, NULL, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_session_gate_precedes_length_gate(void)
{
    /* A 0x03 carrying the wrong length is still answered 0x10 before the
     * handshake: session (step 4) is earlier than length (step 5). */
    static const uint8_t one_byte[1] = { 0x00u };

    init_slave();
    send(ECSP_CMD_SET_HW_VERSION, one_byte, 1u);
    expect_answer(ECSP_CMD_SET_HW_VERSION, ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
}

static void test_command_gate_precedes_session_gate(void)
{
    /* ADR-0002 step 3 before step 4: an undefined command is 0x04 even in the
     * waiting-handshake state. */
    init_slave();
    send(0x7Fu, NULL, 0u);
    expect_answer(0x7Fu, ECSP_RS_UNSUPPORTED, NULL, 0u);
}

static void test_unknown_command_after_handshake(void)
{
    init_slave();
    handshake();

    send(0x7Fu, NULL, 0u);
    expect_answer(0x7Fu, ECSP_RS_UNSUPPORTED, NULL, 0u);
}

/* 0x0F is not a request yet. A host frame that uses it is still undefined. */
static void test_upload_control_is_still_undefined(void)
{
    static const uint8_t off[1] = { 0x00u };

    init_slave();
    handshake();

    send(ECSP_CMD_CONTROL_UPLOAD, off, sizeof off);
    expect_answer(ECSP_CMD_CONTROL_UPLOAD, ECSP_RS_UNSUPPORTED, NULL, 0u);
}

static void test_command_gate_precedes_length_gate(void)
{
    /* An unknown command carrying a payload is 0x04, not 0x02. */
    static const uint8_t one_byte[1] = { 0xAAu };

    init_slave();
    handshake();

    send(0x7Fu, one_byte, 1u);
    expect_answer(0x7Fu, ECSP_RS_UNSUPPORTED, NULL, 0u);
}

static void test_length_gate_precedes_parameter_gate(void)
{
    /* 0x05 wants 12 bytes and validates the serial format. A 3-byte payload
     * fails both; the earlier gate (length) decides: 0x02, never 0x03. */
    static const uint8_t garbage[3] = { 0x00u, 0x11u, 0x22u };

    init_slave();
    handshake();

    send(ECSP_CMD_SET_SERIAL, garbage, sizeof garbage);
    expect_answer(ECSP_CMD_SET_SERIAL, ECSP_RS_BAD_LEN, NULL, 0u);
}

static void test_parameter_gate_after_length_gate(void)
{
    /* Right length, wrong serial format: the only possible code is 0x03. */
    static const uint8_t bad_serial[12] = {
        'X', 'X', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    init_slave();
    handshake();

    send(ECSP_CMD_SET_SERIAL, bad_serial, sizeof bad_serial);
    expect_answer(ECSP_CMD_SET_SERIAL, ECSP_RS_BAD_PARAM, NULL, 0u);
}

static void test_handler_not_called_when_session_gate_rejects(void)
{
    /* The fake handler would answer 0x55 (or its own 0x02 for a bad length),
     * but the session gate must win and the handler must never run. */
    static const uint8_t payload[FAKE_REQ_LEN] = { 0x00u, 0x01u };

    init_slave();
    send(FAKE_CMD, payload, sizeof payload);
    expect_answer(FAKE_CMD, ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
    CHECK_EQ(0, g_fake_calls);
}

static void test_handler_not_called_when_length_gate_rejects(void)
{
    static const uint8_t short_payload[1] = { 0x00u };

    init_slave();
    handshake();

    send(FAKE_CMD, short_payload, sizeof short_payload);
    expect_answer(FAKE_CMD, ECSP_RS_BAD_LEN, NULL, 0u);
    CHECK_EQ(0, g_fake_calls);
}

static void test_handler_not_called_for_wrong_address(void)
{
    static const uint8_t payload[FAKE_REQ_LEN] = { 0x00u, 0x01u };

    init_slave();
    handshake();

    send_from(0x04u, 0x02u, FAKE_CMD, payload, sizeof payload);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
    CHECK_EQ(0, g_fake_calls);
}

static void test_handler_return_code_is_not_overridden(void)
{
    /* Once every gate passes, the engine executes the handler and uses its
     * return value verbatim as the response code (0x55 is not a code the
     * engine itself emits; it must not be replaced with 0x00). */
    static const uint8_t payload[FAKE_REQ_LEN] = { 0x00u, 0x01u };

    init_slave();
    handshake();

    send(FAKE_CMD, payload, sizeof payload);
    CHECK_EQ(1, g_fake_calls);
    expect_answer(FAKE_CMD, 0x55u, NULL, 0u);
}

static void test_board_command_with_variable_length(void)
{
    /* ECSP_CMD_LEN_ANY: no fixed length gate. This command has no validator,
     * so the handler receives whatever length arrives. */
    static const uint8_t payload[3] = { 0x01u, 0x02u, 0x03u };

    init_slave();
    handshake();

    send(FAKE_ANY_CMD, payload, sizeof payload);
    CHECK_EQ(1, g_any_calls);
    expect_answer(FAKE_ANY_CMD, ECSP_RS_OK, NULL, 0u);
}

static void test_board_validator_maps_length_and_value_errors(void)
{
    /* A board command with a validator: E_LEN -> 0x02, E_VAL -> 0x03, both
     * decided before the handler, which therefore never runs on rejection. */
    static const uint8_t two_bytes[2] = { 0xAAu, 0x00u };
    static const uint8_t bad_value[3] = { 0xBBu, 0x00u, 0x00u };
    static const uint8_t accepted[3] = { 0xAAu, 0x00u, 0x00u };

    init_slave();
    handshake();

    send(FAKE_VALIDATED_CMD, two_bytes, sizeof two_bytes);
    expect_answer(FAKE_VALIDATED_CMD, ECSP_RS_BAD_LEN, NULL, 0u);
    CHECK_EQ(0, g_validated_calls);

    send(FAKE_VALIDATED_CMD, bad_value, sizeof bad_value);
    expect_answer(FAKE_VALIDATED_CMD, ECSP_RS_BAD_PARAM, NULL, 0u);
    CHECK_EQ(0, g_validated_calls);

    send(FAKE_VALIDATED_CMD, accepted, sizeof accepted);
    CHECK_EQ(1, g_validated_calls);
    expect_answer(FAKE_VALIDATED_CMD, ECSP_RS_OK, NULL, 0u);
}

/* spec 5.2.8 ceiling, little-endian. Accepting 0x08 is what makes the slave busy. */
static void start_selftest(void)
{
    static const uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    static const uint8_t tmax[2] = { 0xD0u, 0x07u };

    send(ECSP_CMD_CONTROL_SELFTEST, start, sizeof start);
    expect_answer(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_OK, tmax, sizeof tmax);
}

/* Event codes are not requests. 0x0A stays 0x04, including while a
 * long-running self-test is in flight. A query issued in that same window
 * is still answered. */
static void test_result_event_code_is_not_a_request(void)
{
    static const uint8_t result[1] = { 0x00u };
    static const uint8_t version[7] = {
        '1', '.', '0', '.', '0', '.', '0'
    };

    init_slave();
    handshake();
    start_selftest();

    send(ECSP_CMD_GET_VERSION, NULL, 0u);
    expect_answer(ECSP_CMD_GET_VERSION, ECSP_RS_OK, version, sizeof version);

    send(ECSP_CMD_SELFTEST_REPORT, result, sizeof result);
    expect_answer(ECSP_CMD_SELFTEST_REPORT, ECSP_RS_UNSUPPORTED, NULL, 0u);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));
}

static void test_length_gate_precedes_busy_gate(void)
{
    /* ADR-0002 step 5 before step 7: a short 0x08 while busy is 0x02, not 0x11. */
    static const uint8_t seq_only[1] = { 0x02u };

    init_slave();
    handshake();
    start_selftest();

    send(ECSP_CMD_CONTROL_SELFTEST, seq_only, sizeof seq_only);
    expect_answer(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_BAD_LEN, NULL, 0u);
}

static void test_cache_hit_precedes_busy_gate(void)
{
    /* ADR-0002 step 6 before step 7: retransmitting an accepted non-preemptive
     * action while busy replays the cached answer. It must not become 0x11
     * and must not run the handler again. */
    static const uint8_t seq[1] = { 0x01u };

    init_slave();
    handshake();

    send(CACHED_CMD, seq, sizeof seq);
    expect_answer(CACHED_CMD, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_cached_calls);

    start_selftest();

    send(CACHED_CMD, seq, sizeof seq);
    expect_answer(CACHED_CMD, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_cached_calls);
}

static void test_busy_gate_precedes_parameter_gate(void)
{
    /* ADR-0002 step 7 before step 8: an illegal low-power mode while busy is
     * 0x11, not 0x03. A new non-preemptive action is 0x11 and its handler does
     * not run. A preemptive command is accepted. An illegal control on the
     * preemptive self-test command itself still reaches step 8 and is 0x03. */
    static const uint8_t bad_mode[1] = { 0x02u };
    static const uint8_t new_seq[1] = { 0x02u };
    static const uint8_t preempt[1] = { 0x00u };
    static const uint8_t concurrent[1] = { 0x00u };
    static const uint8_t bad_ctrl[2] = { 0x09u, 0x05u };

    init_slave();
    handshake();
    start_selftest();

    send(ECSP_CMD_SET_LOW_POWER, bad_mode, sizeof bad_mode);
    expect_answer(ECSP_CMD_SET_LOW_POWER, ECSP_RS_BUSY, NULL, 0u);

    send(CACHED_CMD, new_seq, sizeof new_seq);
    expect_answer(CACHED_CMD, ECSP_RS_BUSY, NULL, 0u);
    CHECK_EQ(0, g_cached_calls);

    send(PREEMPT_CMD, preempt, sizeof preempt);
    expect_answer(PREEMPT_CMD, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_preempt_calls);

    /* Spec 4.3.1 orthogonal action: accepted, and the self-test stays in flight. */
    send(CONCURRENT_CMD, concurrent, sizeof concurrent);
    expect_answer(CONCURRENT_CMD, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_concurrent_calls);
    CHECK_EQ(ECSP_CMD_CONTROL_SELFTEST, ecsp_slave_long_run_cmd(&g_slave));

    send(ECSP_CMD_CONTROL_SELFTEST, bad_ctrl, sizeof bad_ctrl);
    expect_answer(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_BAD_PARAM, NULL, 0u);
}

int main(void)
{
    test_address_mismatch_is_silent_before_unknown_command();
    test_session_gate_precedes_length_gate();
    test_command_gate_precedes_session_gate();
    test_unknown_command_after_handshake();
    test_upload_control_is_still_undefined();
    test_result_event_code_is_not_a_request();
    test_command_gate_precedes_length_gate();
    test_length_gate_precedes_parameter_gate();
    test_parameter_gate_after_length_gate();
    test_handler_not_called_when_session_gate_rejects();
    test_handler_not_called_when_length_gate_rejects();
    test_handler_not_called_for_wrong_address();
    test_handler_return_code_is_not_overridden();
    test_board_command_with_variable_length();
    test_board_validator_maps_length_and_value_errors();
    test_length_gate_precedes_busy_gate();
    test_cache_hit_precedes_busy_gate();
    test_busy_gate_precedes_parameter_gate();

    return ecsp_test_report();
}
