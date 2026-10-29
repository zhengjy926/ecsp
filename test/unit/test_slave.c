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

static ecsp_version_t make_version(const uint8_t b[7])
{
    ecsp_version_t v;
    memcpy(v.b, b, 7u);
    return v;
}

static void init_slave_version(const uint8_t version[7])
{
    ecsp_slave_cfg_t cfg;
    cfg.product_addr = 0x03u;
    cfg.board_addr   = 0x02u;
    cfg.version      = make_version(version);
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    CHECK_EQ(0, ecsp_slave_init(&g_slave, &cfg));
}

static void init_slave(void)
{
    init_slave_version(version_1_0_0_0);
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

    return ecsp_test_report();
}
