/*
 * L3 slave engine: handshake + version + pre-handshake gate (tickets #3, #4).
 *
 * Seam under test: bytes fed into ecsp_slave_rx, state advanced with
 * ecsp_slave_step, outbound bytes observed through ecsp_slave_tx_pending and
 * the caller-owned transmit buffer (ecsp_slave_tx_drain to consume them).
 *
 * A handshake request (0x01) whose product AND board addresses both match this
 * node is answered 0x00 and moves the session to "ready". Before that
 * handshake, any non-0x01 command is rejected 0x10. Once ready, 0x02 (get
 * software version) is answered 0x00 plus the injected 7-byte version; a 0x01
 * or 0x02 request carrying a payload is answered 0x02 (length error). Frames
 * whose address does not match, or whose CRC/tail is bad, stay silent.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

/* spec 5.2.1, P=0x03 B=0x02 (identical to the handshake_req/resp vectors). */
static const uint8_t handshake_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
};
static const uint8_t handshake_resp[] = {
    0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0x00, 0x50, 0x55, 0x0D
};

/* spec 5.2.2 get-version request / response ("1.0.0.0", P=0x03 B=0x02). */
static const uint8_t version_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x02, 0x02, 0x88, 0xA0, 0x0D
};
static const uint8_t version_resp[] = {
    0xFA, 0x11, 0x00, 0x03, 0x02, 0x02, 0x00,
    0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x30,
    0xFA, 0x0E, 0x0D
};

static const uint8_t version_1_0_0_0[7] = { '1', '.', '0', '.', '0', '.', '0' };
static const uint8_t version_1_0_0_9[7] = { '1', '.', '0', '.', '0', '.', '9' };

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;
static ecsp_slave_identity_t g_nvm;
static int g_commits;
static int g_commit_rc;

static ecsp_version_t make_version(const uint8_t b[7])
{
    ecsp_version_t v;
    memcpy(v.b, b, 7u);
    return v;
}

static void load_default_identity(void)
{
    static const uint8_t default_hw[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t default_sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_identity.hardware_version = make_version(default_hw);
    memcpy(g_identity.serial_number.b, default_sn, sizeof default_sn);
}

/* `g_identity` is the caller's RAM image. A re-init that models power-up
 * keeps whatever the test loaded into it (the durable image), and does not
 * put the factory defaults back. */
static void bind_slave(const uint8_t version[7], uint8_t fault_word)
{
    ecsp_slave_cfg_t cfg;

    cfg.product_addr = 0x03u;
    cfg.board_addr   = 0x02u;
    cfg.version      = make_version(version);
    cfg.identity     = &g_identity;
    cfg.fault_word   = fault_word;
    cfg.upload_mode  = ECSP_UPLOAD_ON;
    cfg.cmds.table   = NULL;
    cfg.cmds.table_len = 0u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    CHECK_EQ(0, ecsp_slave_init(&g_slave, &cfg));
}

static void init_slave_cfg(const uint8_t version[7], uint8_t fault_word)
{
    load_default_identity();
    bind_slave(version, fault_word);
}

static void init_slave_version(const uint8_t version[7])
{
    init_slave_cfg(version, 0x00u);
}

static void init_slave(void)
{
    init_slave_version(version_1_0_0_0);
}

static void init_slave_fault(uint8_t fault_word)
{
    init_slave_cfg(version_1_0_0_0, fault_word);
}

static void drain_all(void)
{
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));
}

/* Feed one frame for this node (P=0x03/B=0x02) and advance the engine. */
static void send_cmd(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n = ecsp_frame_encode(frame, sizeof frame, 0x03u, 0x02u, cmd,
                                 payload, len);

    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, 0u);
}

/* Assert the whole transmit FIFO is exactly one response frame carrying
 * [rs] + data, then drain it. */
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
    drain_all();
}

static void enter_ready(void)
{
    send_cmd(ECSP_CMD_HANDSHAKE, NULL, 0u);
    drain_all();
}

static void test_handshake_responds(void)
{
    init_slave();
    ecsp_slave_rx(&g_slave, handshake_req, sizeof handshake_req);
    ecsp_slave_step(&g_slave, 0u);

    CHECK_EQ(sizeof handshake_resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(handshake_resp, sizeof handshake_resp,
              g_tx, ecsp_slave_tx_pending(&g_slave));

    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_handshake_echoes_node_addresses(void)
{
    /* Same behavior with a different node identity: the response must carry the
     * node's own addresses, not the spec's P=0x03/B=0x02 example values. */
    static const uint8_t req[] = {
        0xFA, 0x09, 0x00, 0x10, 0x01, 0x20, 0xF9, 0x8C, 0x0D
    };
    static const uint8_t resp[] = {
        0xFA, 0x0A, 0x00, 0x10, 0x01, 0x20, 0x00, 0x4C, 0x71, 0x0D
    };
    ecsp_slave_cfg_t cfg;
    ecsp_slave_t s;

    cfg.product_addr = 0x10u;
    cfg.board_addr   = 0x20u;
    cfg.version      = make_version(version_1_0_0_0);
    cfg.identity     = &g_identity;
    cfg.fault_word   = 0x00u;
    cfg.upload_mode  = ECSP_UPLOAD_ON;
    cfg.cmds.table   = NULL;
    cfg.cmds.table_len = 0u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    CHECK_EQ(0, ecsp_slave_init(&s, &cfg));

    ecsp_slave_rx(&s, req, sizeof req);
    ecsp_slave_step(&s, 0u);
    CHECK_EQ(sizeof resp, ecsp_slave_tx_pending(&s));
    CHECK_MEM(resp, sizeof resp, g_tx, ecsp_slave_tx_pending(&s));
}

static void test_product_addr_mismatch_silent(void)
{
    /* Valid frame, product address 0x04 != this node's 0x03. */
    static const uint8_t req[] = {
        0xFA, 0x09, 0x00, 0x04, 0x01, 0x02, 0x39, 0x91, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_board_addr_mismatch_silent(void)
{
    /* Valid frame, board address 0x05 != this node's 0x02. */
    static const uint8_t req[] = {
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x05, 0xC9, 0x92, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_crc_error_silent(void)
{
    /* handshake_req with the CRC low byte corrupted. */
    static const uint8_t req[] = {
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x89, 0x50, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_tail_error_silent(void)
{
    /* handshake_req with the tail corrupted. */
    static const uint8_t req[] = {
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0E
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_version_before_handshake_rejected(void)
{
    /* spec 4.1.2: before the handshake a 0x02 is answered 0x10 with no data. */
    static const uint8_t resp[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x02, 0x02, 0x10, 0xA1, 0x99, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, version_req, sizeof version_req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(resp, sizeof resp, g_tx, ecsp_slave_tx_pending(&g_slave));
}

static void test_other_command_before_handshake_rejected(void)
{
    /* 0x03 (set hardware version) is "any non-0x01 command" -> 0x10. */
    static const uint8_t req[] = {
        0xFA, 0x09, 0x00, 0x03, 0x03, 0x02, 0x89, 0x30, 0x0D
    };
    static const uint8_t resp[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x03, 0x02, 0x10, 0xF0, 0x59, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(resp, sizeof resp, g_tx, ecsp_slave_tx_pending(&g_slave));
}

static void test_version_with_payload_before_handshake_rejected(void)
{
    /* ADR-0002: the session gate precedes the length gate, so a payload-bearing
     * 0x02 before the handshake is 0x10, not 0x02. */
    static const uint8_t req[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x02, 0x02, 0xAA, 0x20, 0x2A, 0x0D
    };
    static const uint8_t resp[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x02, 0x02, 0x10, 0xA1, 0x99, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(resp, sizeof resp, g_tx, ecsp_slave_tx_pending(&g_slave));
}

static void test_handshake_with_payload_length_error(void)
{
    /* spec 5.2.1: a handshake request must carry an empty payload, so a
     * payload-bearing handshake is a length error (0x02), not a handshake. */
    static const uint8_t req[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0xAA, 0xD0, 0x2A, 0x0D
    };
    static const uint8_t resp[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0x02, 0xD1, 0x94, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(resp, sizeof resp, g_tx, ecsp_slave_tx_pending(&g_slave));
}

static void test_half_frame_then_rest(void)
{
    init_slave();

    /* First 4 bytes of the handshake request: no answer yet. */
    ecsp_slave_rx(&g_slave, handshake_req, 4u);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));

    /* The remaining 5 bytes complete the frame across the call boundary. */
    ecsp_slave_rx(&g_slave, handshake_req + 4u, sizeof handshake_req - 4u);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof handshake_resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(handshake_resp, sizeof handshake_resp,
              g_tx, ecsp_slave_tx_pending(&g_slave));
}

static void test_two_glued_frames(void)
{
    static const uint8_t glued[] = {
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D,
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, glued, sizeof glued);
    ecsp_slave_step(&g_slave, 0u);

    /* Both handshakes are answered, in order, without polluting each other. */
    CHECK_EQ(2u * sizeof handshake_resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(handshake_resp, sizeof handshake_resp, g_tx, sizeof handshake_resp);
    CHECK_MEM(handshake_resp, sizeof handshake_resp,
              g_tx + sizeof handshake_resp, sizeof handshake_resp);
}

static void test_tx_drain_partial(void)
{
    init_slave();
    ecsp_slave_rx(&g_slave, handshake_req, sizeof handshake_req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof handshake_resp, ecsp_slave_tx_pending(&g_slave));

    /* Draining 4 bytes shifts the remaining 6 to the front of the FIFO. */
    ecsp_slave_tx_drain(&g_slave, 4u);
    CHECK_EQ(sizeof handshake_resp - 4u, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(handshake_resp + 4u, sizeof handshake_resp - 4u,
              g_tx, sizeof handshake_resp - 4u);
}

static void test_version_after_handshake(void)
{
    init_slave();

    ecsp_slave_rx(&g_slave, handshake_req, sizeof handshake_req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof handshake_resp, ecsp_slave_tx_pending(&g_slave));
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));

    ecsp_slave_rx(&g_slave, version_req, sizeof version_req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof version_resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(version_resp, sizeof version_resp,
              g_tx, ecsp_slave_tx_pending(&g_slave));
}

static void test_version_reports_injected_bytes_verbatim(void)
{
    /* A version whose 7th byte is '9' (0x39) is reported verbatim: the engine
     * treats it as 7 opaque bytes, never a NUL-terminated C string. */
    static const uint8_t resp[] = {
        0xFA, 0x11, 0x00, 0x03, 0x02, 0x02, 0x00,
        0x31, 0x2E, 0x30, 0x2E, 0x30, 0x2E, 0x39,
        0x3A, 0x08, 0x0D
    };

    init_slave_version(version_1_0_0_9);

    ecsp_slave_rx(&g_slave, handshake_req, sizeof handshake_req);
    ecsp_slave_step(&g_slave, 0u);
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));

    ecsp_slave_rx(&g_slave, version_req, sizeof version_req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(resp, sizeof resp, g_tx, ecsp_slave_tx_pending(&g_slave));
}

static void test_version_with_payload_length_error(void)
{
    /* After the handshake, a get-version request carrying a payload is a length
     * error (0x02) with no version data (spec 5.2.2). */
    static const uint8_t req[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x02, 0x02, 0xAA, 0x20, 0x2A, 0x0D
    };
    static const uint8_t resp[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x02, 0x02, 0x02, 0x21, 0x94, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, handshake_req, sizeof handshake_req);
    ecsp_slave_step(&g_slave, 0u);
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));

    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(sizeof resp, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(resp, sizeof resp, g_tx, ecsp_slave_tx_pending(&g_slave));
}

/* ---- ticket #5: passthrough commands ---- */

static ecsp_slave_identity_t g_seen[2];

static int commit_to_nvm(ecsp_slave_t *s, const ecsp_slave_identity_t *next)
{
    (void)s;
    if (g_commits < 2) {
        g_seen[g_commits] = *next;
    }
    g_commits++;
    if (g_commit_rc != ECSP_OK) {
        return g_commit_rc;
    }
    g_nvm = *next;
    return ECSP_OK;
}

/* spec 5.2.3 / 5.2.4: 0x00 is answered only after the identity commit
 * succeeds. A later init that loads the committed image still returns it,
 * and the serial number that this command did not write stays put. */
static void test_committed_hardware_version_survives_reinit(void)
{
    static const uint8_t hw[7] = { '1', '.', '0', '.', '0', '.', '9' };
    static const uint8_t sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_commits = 0;
    g_commit_rc = ECSP_OK;
    init_slave();
    g_nvm = g_identity;
    ecsp_slave_set_identity_commit(&g_slave, commit_to_nvm);
    enter_ready();

    send_cmd(ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_commits);
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, hw, sizeof hw));
    CHECK_EQ(0, memcmp(g_identity.serial_number.b, sn, sizeof sn));
    CHECK_EQ(0, memcmp(g_nvm.hardware_version.b, hw, sizeof hw));
    CHECK_EQ(0, memcmp(g_nvm.serial_number.b, sn, sizeof sn));

    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, hw, sizeof hw);

    g_identity = g_nvm;
    bind_slave(version_1_0_0_0, 0x00u);
    enter_ready();
    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, hw, sizeof hw);
    send_cmd(ECSP_CMD_GET_SERIAL, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SERIAL, ECSP_RS_OK, sn, sizeof sn);
}

/* spec 5.2.3: a failed commit answers 0x01 and the saved identity is the
 * one from before the write. */
static void test_failed_hardware_commit_keeps_saved_identity(void)
{
    static const uint8_t hw[7] = { '2', '.', '0', '.', '0', '.', '0' };
    static const uint8_t stored[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_commits = 0;
    g_commit_rc = ECSP_E_VAL;
    init_slave();
    g_nvm = g_identity;
    ecsp_slave_set_identity_commit(&g_slave, commit_to_nvm);
    enter_ready();

    send_cmd(ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, 0x01u, NULL, 0u);
    CHECK_EQ(1, g_commits);
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, stored, sizeof stored));
    CHECK_EQ(0, memcmp(g_identity.serial_number.b, sn, sizeof sn));
    CHECK_EQ(0, memcmp(g_nvm.hardware_version.b, stored, sizeof stored));

    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, stored, sizeof stored);
}

static void test_set_and_get_hardware_version(void)
{
    /* spec 5.2.3 / 5.2.4: a successful write is visible verbatim through the
     * get command, and lands in the caller-owned identity storage. */
    static const uint8_t hw[7] = { '1', '.', '0', '.', '0', '.', '9' };

    init_slave();
    enter_ready();

    send_cmd(ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, hw, sizeof hw));

    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, hw, sizeof hw);
}

static void test_bad_length_hardware_version_keeps_stored_value(void)
{
    /* spec 5.2.3 / 6.2: a length error is 0x02 and must not change the saved
     * value. */
    static const uint8_t bad[6] = { '1', '.', '0', '.', '0', '.' };
    static const uint8_t stored[7] = { '1', '.', '0', '.', '0', '.', '0' };

    init_slave();
    enter_ready();

    send_cmd(ECSP_CMD_SET_HW_VERSION, bad, sizeof bad);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_BAD_LEN, NULL, 0u);

    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, stored, sizeof stored);
}

static void test_bad_parameter_hardware_version_keeps_stored_value(void)
{
    /* Right length, wrong "X.Y.Z.N" shape: 0x03, never 0x02, and unchanged. */
    static const uint8_t bad[7] = { '1', '.', '0', '.', '0', '.', 'X' };
    static const uint8_t stored[7] = { '1', '.', '0', '.', '0', '.', '0' };

    init_slave();
    enter_ready();

    send_cmd(ECSP_CMD_SET_HW_VERSION, bad, sizeof bad);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_BAD_PARAM, NULL, 0u);

    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, stored, sizeof stored);
}

/* spec 5.2.5 / 5.2.6: same commit rule as hardware version. Setting the
 * serial number does not replace the hardware version. */
static void test_committed_serial_survives_reinit(void)
{
    static const uint8_t hw[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t sn[12] = {
        'S', 'N', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'
    };

    g_commits = 0;
    g_commit_rc = ECSP_OK;
    init_slave();
    g_nvm = g_identity;
    ecsp_slave_set_identity_commit(&g_slave, commit_to_nvm);
    enter_ready();

    send_cmd(ECSP_CMD_SET_SERIAL, sn, sizeof sn);
    expect_resp(ECSP_CMD_SET_SERIAL, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, g_commits);
    CHECK_EQ(0, memcmp(g_identity.serial_number.b, sn, sizeof sn));
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, hw, sizeof hw));
    CHECK_EQ(0, memcmp(g_nvm.serial_number.b, sn, sizeof sn));
    CHECK_EQ(0, memcmp(g_nvm.hardware_version.b, hw, sizeof hw));

    send_cmd(ECSP_CMD_GET_SERIAL, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SERIAL, ECSP_RS_OK, sn, sizeof sn);

    g_identity = g_nvm;
    bind_slave(version_1_0_0_0, 0x00u);
    enter_ready();
    send_cmd(ECSP_CMD_GET_SERIAL, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SERIAL, ECSP_RS_OK, sn, sizeof sn);
    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, hw, sizeof hw);
}

static void test_set_and_get_serial_number(void)
{
    /* spec 5.2.5 / 5.2.6. */
    static const uint8_t sn[12] = {
        'S', 'N', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'
    };

    init_slave();
    enter_ready();

    send_cmd(ECSP_CMD_SET_SERIAL, sn, sizeof sn);
    expect_resp(ECSP_CMD_SET_SERIAL, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(0, memcmp(g_identity.serial_number.b, sn, sizeof sn));

    send_cmd(ECSP_CMD_GET_SERIAL, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SERIAL, ECSP_RS_OK, sn, sizeof sn);
}

static void test_bad_length_serial_keeps_stored_value(void)
{
    static const uint8_t bad[11] = {
        'S', 'N', '1', '2', '3', '4', '5', '6', '7', '8', '9'
    };
    static const uint8_t stored[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    init_slave();
    enter_ready();

    send_cmd(ECSP_CMD_SET_SERIAL, bad, sizeof bad);
    expect_resp(ECSP_CMD_SET_SERIAL, ECSP_RS_BAD_LEN, NULL, 0u);

    send_cmd(ECSP_CMD_GET_SERIAL, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SERIAL, ECSP_RS_OK, stored, sizeof stored);
}

static void test_bad_parameter_serial_keeps_stored_value(void)
{
    static const uint8_t bad[12] = {
        'S', 'N', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'X'
    };
    static const uint8_t stored[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    init_slave();
    enter_ready();

    send_cmd(ECSP_CMD_SET_SERIAL, bad, sizeof bad);
    expect_resp(ECSP_CMD_SET_SERIAL, ECSP_RS_BAD_PARAM, NULL, 0u);

    send_cmd(ECSP_CMD_GET_SERIAL, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SERIAL, ECSP_RS_OK, stored, sizeof stored);
}

static void test_low_power_mode(void)
{
    /* spec 5.2.11: modes 0x00 and 0x01 are accepted; anything else is 0x03;
     * a missing mode byte is a length error 0x02. */
    static const uint8_t exit_mode[1] = { ECSP_LOW_POWER_EXIT };
    static const uint8_t enter_mode[1] = { ECSP_LOW_POWER_ENTER };
    static const uint8_t bad_mode[1] = { 0x02u };

    init_slave();
    enter_ready();

    CHECK_EQ(ECSP_LOW_POWER_EXIT, ecsp_slave_low_power_mode(&g_slave));

    send_cmd(ECSP_CMD_SET_LOW_POWER, exit_mode, sizeof exit_mode);
    expect_resp(ECSP_CMD_SET_LOW_POWER, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(ECSP_LOW_POWER_EXIT, ecsp_slave_low_power_mode(&g_slave));

    send_cmd(ECSP_CMD_SET_LOW_POWER, enter_mode, sizeof enter_mode);
    expect_resp(ECSP_CMD_SET_LOW_POWER, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));

    send_cmd(ECSP_CMD_SET_LOW_POWER, bad_mode, sizeof bad_mode);
    expect_resp(ECSP_CMD_SET_LOW_POWER, ECSP_RS_BAD_PARAM, NULL, 0u);
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));

    send_cmd(ECSP_CMD_SET_LOW_POWER, NULL, 0u);
    expect_resp(ECSP_CMD_SET_LOW_POWER, ECSP_RS_BAD_LEN, NULL, 0u);
    CHECK_EQ(ECSP_LOW_POWER_ENTER, ecsp_slave_low_power_mode(&g_slave));
}

static void test_get_fault_returns_injected_word(void)
{
    /* spec 5.2.18: the injected fault word is returned verbatim after 0x00. */
    static const uint8_t fault[1] = { 0x05u };

    init_slave_fault(fault[0]);
    enter_ready();

    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_OK, fault, sizeof fault);
}

static void test_get_fault_before_handshake_is_rejected(void)
{
    init_slave_fault(0x05u);

    send_cmd(ECSP_CMD_GET_FAULT, NULL, 0u);
    expect_resp(ECSP_CMD_GET_FAULT, ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
}

static void test_system_reset_answers_and_requests_reset(void)
{
    /* spec 5.2.7: the answer exists before the reset takes effect. The library
     * only raises the reset request; the product performs the actual MCU reset
     * after draining, and that reset re-initialises this context - which is
     * where the session returns to waiting for a handshake and the transaction
     * cache is emptied. Until then the session is left untouched, so a
     * retransmitted 0x07 can replay its answer instead of being refused 0x10
     * (ticket #6, spec 4.2). */
    static const uint8_t seq[1] = { 0x42u };

    init_slave();
    enter_ready();
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));

    send_cmd(ECSP_CMD_SYSTEM_RESET, seq, sizeof seq);
    expect_resp(ECSP_CMD_SYSTEM_RESET, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));

    /* Re-initialising models the reset the product performs after draining. */
    init_slave();
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));

    send_cmd(ECSP_CMD_GET_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_VERSION, ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
}

static void test_system_reset_with_bad_length_does_not_reset(void)
{
    /* A length error (0x02) is decided before the handler, so it must not
     * reset the session. */
    init_slave();
    enter_ready();

    send_cmd(ECSP_CMD_SYSTEM_RESET, NULL, 0u);
    expect_resp(ECSP_CMD_SYSTEM_RESET, ECSP_RS_BAD_LEN, NULL, 0u);
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));

    send_cmd(ECSP_CMD_GET_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_VERSION, ECSP_RS_OK, version_1_0_0_0, 7u);
}

/* spec 5.2.5: a failed serial commit answers 0x01 and keeps the old serial. */
static void test_failed_serial_commit_keeps_saved_identity(void)
{
    static const uint8_t sn[12] = {
        'S', 'N', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'
    };
    static const uint8_t stored[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_commits = 0;
    g_commit_rc = ECSP_E_VAL;
    init_slave();
    g_nvm = g_identity;
    ecsp_slave_set_identity_commit(&g_slave, commit_to_nvm);
    enter_ready();

    send_cmd(ECSP_CMD_SET_SERIAL, sn, sizeof sn);
    expect_resp(ECSP_CMD_SET_SERIAL, 0x01u, NULL, 0u);
    CHECK_EQ(1, g_commits);
    CHECK_EQ(0, memcmp(g_identity.serial_number.b, stored, sizeof stored));
    CHECK_EQ(0, memcmp(g_nvm.serial_number.b, stored, sizeof stored));

    send_cmd(ECSP_CMD_GET_SERIAL, NULL, 0u);
    expect_resp(ECSP_CMD_GET_SERIAL, ECSP_RS_OK, stored, sizeof stored);
}

/* No callback registered: the command still answers 0x00 and changes RAM.
 * Reloading the untouched durable image brings the old identity back. */
static void test_unregistered_commit_changes_ram_only(void)
{
    static const uint8_t hw[7] = { '1', '.', '0', '.', '0', '.', '9' };
    static const uint8_t old_hw[7] = { '1', '.', '0', '.', '0', '.', '0' };

    g_commits = 0;
    init_slave();
    g_nvm = g_identity;
    enter_ready();

    send_cmd(ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_OK, NULL, 0u);
    CHECK_EQ(0, g_commits);
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, hw, sizeof hw));
    CHECK_EQ(0, memcmp(g_nvm.hardware_version.b, old_hw, sizeof old_hw));

    g_identity = g_nvm;
    bind_slave(version_1_0_0_0, 0x00u);
    enter_ready();
    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, old_hw, sizeof old_hw);
}

/* A retransmission is not deduplicated: commit runs again, and both
 * publishes carry the same identity. */
static void test_repeated_identity_write_commits_the_same_value(void)
{
    static const uint8_t hw[7] = { '3', '.', '1', '.', '0', '.', '0' };
    static const uint8_t sn[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_commits = 0;
    g_commit_rc = ECSP_OK;
    init_slave();
    g_nvm = g_identity;
    ecsp_slave_set_identity_commit(&g_slave, commit_to_nvm);
    enter_ready();

    send_cmd(ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_OK, NULL, 0u);
    send_cmd(ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_OK, NULL, 0u);

    CHECK_EQ(2, g_commits);
    CHECK_EQ(0, memcmp(g_seen[0].hardware_version.b, hw, sizeof hw));
    CHECK_EQ(0, memcmp(g_seen[1].hardware_version.b, hw, sizeof hw));
    CHECK_EQ(0, memcmp(g_seen[0].serial_number.b, sn, sizeof sn));
    CHECK_EQ(0, memcmp(g_seen[1].serial_number.b, sn, sizeof sn));
    CHECK_EQ(0, memcmp(g_nvm.hardware_version.b, hw, sizeof hw));
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, hw, sizeof hw));

    send_cmd(ECSP_CMD_GET_HW_VERSION, NULL, 0u);
    expect_resp(ECSP_CMD_GET_HW_VERSION, ECSP_RS_OK, hw, sizeof hw);
}

static void arm_commit(void)
{
    g_commits = 0;
    g_commit_rc = ECSP_OK;
    g_nvm = g_identity;
    ecsp_slave_set_identity_commit(&g_slave, commit_to_nvm);
}

/* spec 5.2.3: format, length, session and busy rejections do not commit. */
static void test_rejected_identity_write_does_not_commit(void)
{
    static const uint8_t bad_shape[7] = { '1', '.', '0', '.', '0', '.', 'X' };
    static const uint8_t short_hw[6] = { '1', '.', '0', '.', '0', '.' };
    static const uint8_t good_hw[7] = { '1', '.', '0', '.', '0', '.', '9' };
    static const uint8_t stored[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t start[2] = { 0x01u, ECSP_CTRL_START };
    static const uint8_t tmax[2] = { 0xD0u, 0x07u };

    init_slave();
    arm_commit();
    send_cmd(ECSP_CMD_SET_HW_VERSION, good_hw, sizeof good_hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
    CHECK_EQ(0, g_commits);

    enter_ready();
    send_cmd(ECSP_CMD_SET_HW_VERSION, short_hw, sizeof short_hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_BAD_LEN, NULL, 0u);
    CHECK_EQ(0, g_commits);

    send_cmd(ECSP_CMD_SET_HW_VERSION, bad_shape, sizeof bad_shape);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_BAD_PARAM, NULL, 0u);
    CHECK_EQ(0, g_commits);

    send_cmd(ECSP_CMD_CONTROL_SELFTEST, start, sizeof start);
    expect_resp(ECSP_CMD_CONTROL_SELFTEST, ECSP_RS_OK, tmax, sizeof tmax);
    send_cmd(ECSP_CMD_SET_HW_VERSION, good_hw, sizeof good_hw);
    expect_resp(ECSP_CMD_SET_HW_VERSION, ECSP_RS_BUSY, NULL, 0u);
    CHECK_EQ(0, g_commits);
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, stored, sizeof stored));
}

/* spec 6.2: a mismatched address or a bad CRC is silent, and does not commit. */
static void test_discarded_identity_frame_does_not_commit(void)
{
    static const uint8_t hw[7] = { '1', '.', '0', '.', '0', '.', '9' };
    static const uint8_t stored[7] = { '1', '.', '0', '.', '0', '.', '0' };
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n;

    init_slave();
    enter_ready();
    arm_commit();

    n = ecsp_frame_encode(frame, sizeof frame, 0x04u, 0x02u,
                          ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
    CHECK_EQ(0, g_commits);

    n = ecsp_frame_encode(frame, sizeof frame, 0x03u, 0x02u,
                          ECSP_CMD_SET_HW_VERSION, hw, sizeof hw);
    CHECK(n > 3u);
    frame[n - 3u] ^= 0xFFu;
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
    CHECK_EQ(0, g_commits);
    CHECK_EQ(0, memcmp(g_identity.hardware_version.b, stored, sizeof stored));
}

static void test_init_rejects_incomplete_configuration(void)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t sw_version[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const ecsp_cmd_entry_t big_response[] = {
        { 0x30u, ECSP_CMD_NONE, 0u, 40u, NULL, NULL }
    };
    static const ecsp_cmd_entry_t oversized_response[] = {
        { 0x30u, ECSP_CMD_NONE, 0u, ECSP_MAX_FRAME, NULL, NULL }
    };

    cfg.product_addr = 0x03u;
    cfg.board_addr   = 0x02u;
    cfg.version      = make_version(sw_version);
    cfg.identity     = &g_identity;
    cfg.fault_word   = 0u;
    cfg.upload_mode  = ECSP_UPLOAD_ON;
    cfg.cmds.table   = NULL;
    cfg.cmds.table_len = 0u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;

    /* The identity storage is mandatory: 0x03/0x05 have nowhere to write
     * without it. */
    cfg.identity = NULL;
    CHECK_EQ(ECSP_E_ARG, ecsp_slave_init(&g_slave, &cfg));

    /* The transmit FIFO must hold the largest common answer (get serial:
     * 9-byte frame + response code + 12-byte serial). */
    cfg.identity = &g_identity;
    cfg.tx_cap = ECSP_FRAME_MIN_LEN + 1u + ECSP_SERIAL_LEN - 1u;
    CHECK_EQ(ECSP_E_ARG, ecsp_slave_init(&g_slave, &cfg));

    /* A non-empty table cannot have a NULL pointer. */
    cfg.tx_cap = sizeof g_tx;
    cfg.cmds.table_len = 1u;
    CHECK_EQ(ECSP_E_ARG, ecsp_slave_init(&g_slave, &cfg));

    /* The FIFO must also fit the largest response declared by the table, and
     * a declaration larger than a frame is invalid. */
    cfg.cmds.table = big_response;
    cfg.tx_cap = ECSP_FRAME_MIN_LEN + 1u + big_response[0].resp_max - 1u;
    CHECK_EQ(ECSP_E_ARG, ecsp_slave_init(&g_slave, &cfg));
    cfg.tx_cap = ECSP_FRAME_MIN_LEN + 1u + big_response[0].resp_max;
    CHECK_EQ(ECSP_OK, ecsp_slave_init(&g_slave, &cfg));

    cfg.cmds.table = oversized_response;
    cfg.tx_cap = sizeof g_tx;
    CHECK_EQ(ECSP_E_ARG, ecsp_slave_init(&g_slave, &cfg));
}

int main(void)
{
    test_handshake_responds();
    test_handshake_echoes_node_addresses();
    test_product_addr_mismatch_silent();
    test_board_addr_mismatch_silent();
    test_crc_error_silent();
    test_tail_error_silent();
    test_version_before_handshake_rejected();
    test_other_command_before_handshake_rejected();
    test_version_with_payload_before_handshake_rejected();
    test_handshake_with_payload_length_error();
    test_half_frame_then_rest();
    test_two_glued_frames();
    test_tx_drain_partial();
    test_version_after_handshake();
    test_version_reports_injected_bytes_verbatim();
    test_version_with_payload_length_error();
    test_committed_hardware_version_survives_reinit();
    test_failed_hardware_commit_keeps_saved_identity();
    test_set_and_get_hardware_version();
    test_bad_length_hardware_version_keeps_stored_value();
    test_bad_parameter_hardware_version_keeps_stored_value();
    test_committed_serial_survives_reinit();
    test_failed_serial_commit_keeps_saved_identity();
    test_unregistered_commit_changes_ram_only();
    test_repeated_identity_write_commits_the_same_value();
    test_rejected_identity_write_does_not_commit();
    test_discarded_identity_frame_does_not_commit();
    test_set_and_get_serial_number();
    test_bad_length_serial_keeps_stored_value();
    test_bad_parameter_serial_keeps_stored_value();
    test_low_power_mode();
    test_get_fault_returns_injected_word();
    test_get_fault_before_handshake_is_rejected();
    test_system_reset_answers_and_requests_reset();
    test_system_reset_with_bad_length_does_not_reset();
    test_init_rejects_incomplete_configuration();

    return ecsp_test_report();
}
