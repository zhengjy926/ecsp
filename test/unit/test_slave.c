/*
 * L3 slave engine: handshake response (ticket #3).
 *
 * Seam under test: bytes fed into ecsp_slave_rx, state advanced with
 * ecsp_slave_step, outbound bytes observed through ecsp_slave_tx_pending and
 * the caller-owned transmit buffer (ecsp_slave_tx_drain to consume them).
 *
 * This ticket establishes the first "bytes in, bytes out" seam: only a
 * handshake request (0x01) whose product AND board addresses both match this
 * node is answered, with a single 0x00 response frame. Everything else is
 * silent. Rejection codes (0x02/0x04/0x10/...) land with the pipeline in #5.
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

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static ecsp_slave_t g_slave;

static void init_slave(void)
{
    ecsp_slave_cfg_t cfg;
    cfg.product_addr = 0x03u;
    cfg.board_addr   = 0x02u;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = sizeof g_tx;
    CHECK_EQ(0, ecsp_slave_init(&g_slave, &cfg));
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

static void test_non_handshake_silent(void)
{
    /* A matching-address command other than handshake (0x02 get version) is not
     * answered by this ticket; the rejection pipeline arrives in #5. */
    static const uint8_t req[] = {
        0xFA, 0x09, 0x00, 0x03, 0x02, 0x02, 0x88, 0xA0, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
}

static void test_handshake_with_payload_silent(void)
{
    /* A handshake request that carries a payload is not a handshake request
     * (spec 5.2.1: "请求帧数据载荷：无"). Its 0x02 rejection lands in #5; this
     * ticket's "其余情况一个字节都不发" means it stays silent. */
    static const uint8_t req[] = {
        0xFA, 0x0A, 0x00, 0x03, 0x01, 0x02, 0xAA, 0xD0, 0x2A, 0x0D
    };

    init_slave();
    ecsp_slave_rx(&g_slave, req, sizeof req);
    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(0u, ecsp_slave_tx_pending(&g_slave));
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

int main(void)
{
    test_handshake_responds();
    test_handshake_echoes_node_addresses();
    test_product_addr_mismatch_silent();
    test_board_addr_mismatch_silent();
    test_crc_error_silent();
    test_tail_error_silent();
    test_non_handshake_silent();
    test_handshake_with_payload_silent();
    test_half_frame_then_rest();
    test_two_glued_frames();
    test_tx_drain_partial();

    return ecsp_test_report();
}
