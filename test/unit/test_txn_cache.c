/*
 * L3 slave transaction-sequence cache (ticket #6, spec 4.2, ADR-0004/0008).
 *
 * Seam: a request frame goes in through ecsp_slave_rx / ecsp_slave_step, the
 * answer comes out through ecsp_slave_tx_pending and the caller-owned transmit
 * buffer. Nothing internal is inspected: "the handler ran again" is observed
 * through the handler's own call counter, which it echoes into the answer
 * data, so a replayed answer and a re-executed one cannot be confused.
 *
 * Rules under test:
 *   - the same (command code, transaction sequence) executes once and then
 *     replays the first answer byte for byte (spec 4.2);
 *   - a different sequence is a different transaction and executes again;
 *   - a handshake request (0x01) empties the cache, so a host that restarts
 *     and starts its counters over at 0 cannot hit an old entry (ADR-0004);
 *   - a full cache evicts its OLDEST entry (FIFO), not the least recently used;
 *   - commands that carry no transaction sequence are never deduplicated;
 *   - an answer that fills a whole frame is delivered and replayed unchanged:
 *     the cache never shortens a reply to make room for itself.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

#define TXN_CMD     0x30u /* non-idempotent action: payload[0] is the sequence */
#define TXN_ANY_CMD 0x31u /* sequence-carrying action with a variable length */
#define PLAIN_CMD   0x32u /* setter-like command: no sequence, no dedupe */
#define BIG_CMD     0x33u /* sequence-carrying action with a whole-frame answer */
#define CHECKED_CMD 0x34u /* sequence-carrying action with a validator */

/* Largest response DATA length that still fits one frame. */
#define BIG_DATA (ECSP_MAX_FRAME - ECSP_FRAME_MIN_LEN - 1u)

static uint8_t g_rx[ECSP_MAX_FRAME];
static uint8_t g_tx[2u * ECSP_MAX_FRAME];
static ecsp_slave_t g_slave;
static ecsp_slave_identity_t g_identity;

static int g_txn_calls;
static int g_any_calls;
static int g_plain_calls;
static int g_big_calls;

/* Echo the sequence and the call number: a second execution necessarily
 * produces different bytes from the first. */
static int txn_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                       uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)cap;
    g_txn_calls++;
    resp[0] = req->payload[0];
    resp[1] = (uint8_t)g_txn_calls;
    *resp_len = 2u;
    return ECSP_RS_OK;
}

/* A sequence-carrying command whose payload length is the handler's business;
 * with a zero-length payload there is no sequence byte to key on. */
static int any_txn_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
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

static int plain_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                         uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)cap;
    g_plain_calls++;
    resp[0] = (uint8_t)g_plain_calls;
    *resp_len = 1u;
    return ECSP_RS_OK;
}

static int big_handler(ecsp_slave_t *s, const ecsp_frame_t *req,
                       uint8_t *resp, size_t cap, size_t *resp_len)
{
    size_t i;

    (void)s;
    (void)cap;
    g_big_calls++;
    for (i = 0u; i < BIG_DATA; i++) {
        resp[i] = (uint8_t)(req->payload[0] + (uint8_t)i);
    }
    *resp_len = BIG_DATA;
    return ECSP_RS_OK;
}

static int checked_validator(const ecsp_frame_t *req)
{
    /* Length is already gated by req_len; only the value is left. */
    return (req->payload[1] == 0xAAu) ? ECSP_OK : ECSP_E_VAL;
}

static const ecsp_cmd_entry_t g_board_table[] = {
    { TXN_CMD, ECSP_CMD_TXN_SEQ, 1u, 2u, NULL, txn_handler },
    { TXN_ANY_CMD, ECSP_CMD_TXN_SEQ, ECSP_CMD_LEN_ANY, 0u, NULL,
      any_txn_handler },
    { PLAIN_CMD, ECSP_CMD_NONE, 1u, 1u, NULL, plain_handler },
    { BIG_CMD, ECSP_CMD_TXN_SEQ, 1u, (uint16_t)BIG_DATA, NULL, big_handler },
    { CHECKED_CMD, ECSP_CMD_TXN_SEQ, 2u, 2u, checked_validator, txn_handler }
};

/* A table without the whole-frame answer, so a context can be configured with
 * a transmit FIFO that holds only one small answer. */
static const ecsp_cmd_entry_t g_small_table[] = {
    { TXN_CMD, ECSP_CMD_TXN_SEQ, 1u, 2u, NULL, txn_handler }
};

static void init_slave_cfg(const ecsp_cmd_entry_t *table, size_t table_len,
                           size_t tx_cap)
{
    ecsp_slave_cfg_t cfg;
    static const uint8_t sw_version[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t hw_version[7] = { '1', '.', '0', '.', '0', '.', '0' };
    static const uint8_t serial[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };

    g_txn_calls = 0;
    g_any_calls = 0;
    g_plain_calls = 0;
    g_big_calls = 0;

    memcpy(g_identity.hardware_version.b, hw_version, sizeof hw_version);
    memcpy(g_identity.serial_number.b, serial, sizeof serial);

    cfg.product_addr = 0x03u;
    cfg.board_addr   = 0x02u;
    memcpy(cfg.version.b, sw_version, sizeof sw_version);
    cfg.identity       = &g_identity;
    cfg.fault_word     = 0x00u;
    cfg.upload_mode    = ECSP_UPLOAD_ON;
    cfg.cmds.table     = table;
    cfg.cmds.table_len = table_len;
    cfg.rx_buf = g_rx;
    cfg.rx_cap = sizeof g_rx;
    cfg.tx_buf = g_tx;
    cfg.tx_cap = tx_cap;
    CHECK_EQ(0, ecsp_slave_init(&g_slave, &cfg));
}

static void init_slave(void)
{
    init_slave_cfg(g_board_table,
                   sizeof g_board_table / sizeof g_board_table[0],
                   sizeof g_tx);
}

/* Feed one frame from this node (P=0x03/B=0x02) and advance the engine. */
static void send(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[ECSP_MAX_FRAME];
    size_t n = ecsp_frame_encode(frame, sizeof frame, 0x03u, 0x02u, cmd,
                                 payload, len);

    CHECK(n > 0u);
    ecsp_slave_rx(&g_slave, frame, n);
    ecsp_slave_step(&g_slave, 0u);
}

static void drain(void)
{
    ecsp_slave_tx_drain(&g_slave, ecsp_slave_tx_pending(&g_slave));
}

/* Assert the whole transmit FIFO is exactly one answer frame carrying [rs] +
 * data, then drain it. */
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
    want_len = ecsp_frame_encode(want, sizeof want, 0x03u, 0x02u, cmd,
                                 payload, (uint16_t)(1u + data_len));

    CHECK_EQ(want_len, ecsp_slave_tx_pending(&g_slave));
    CHECK_MEM(want, want_len, g_tx, ecsp_slave_tx_pending(&g_slave));
    drain();
}

static void handshake(void)
{
    send(ECSP_CMD_HANDSHAKE, NULL, 0u);
    expect_answer(ECSP_CMD_HANDSHAKE, ECSP_RS_OK, NULL, 0u);
}

static void test_same_sequence_replays_without_running_the_handler(void)
{
    static const uint8_t seq[1] = { 0x11u };
    /* The first answer: sequence 0x11 from the first handler call. */
    static const uint8_t first[2] = { 0x11u, 0x01u };

    init_slave();
    handshake();

    send(TXN_CMD, seq, sizeof seq);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, first, sizeof first);

    /* Two retransmissions must replay the whole answer frame byte for byte and
     * must not execute the action a second time. */
    send(TXN_CMD, seq, sizeof seq);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, first, sizeof first);

    send(TXN_CMD, seq, sizeof seq);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, first, sizeof first);
}

static void test_different_sequences_execute_separately(void)
{
    static const uint8_t seq_a[1] = { 0x01u };
    static const uint8_t seq_b[1] = { 0x02u };
    static const uint8_t answer_a[2] = { 0x01u, 0x01u };
    static const uint8_t answer_b[2] = { 0x02u, 0x02u };

    init_slave();
    handshake();

    send(TXN_CMD, seq_a, sizeof seq_a);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, answer_a, sizeof answer_a);

    send(TXN_CMD, seq_b, sizeof seq_b);
    CHECK_EQ(2, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, answer_b, sizeof answer_b);

    /* Retransmitting either transaction still costs no execution. */
    send(TXN_CMD, seq_a, sizeof seq_a);
    CHECK_EQ(2, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, answer_a, sizeof answer_a);

    send(TXN_CMD, seq_b, sizeof seq_b);
    CHECK_EQ(2, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, answer_b, sizeof answer_b);
}

static void test_handshake_clears_the_cache(void)
{
    static const uint8_t seq[1] = { 0x21u };
    static const uint8_t before_restart[2] = { 0x21u, 0x01u };
    static const uint8_t after_restart[2] = { 0x21u, 0x02u };

    init_slave();
    handshake();

    send(TXN_CMD, seq, sizeof seq);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, before_restart, sizeof before_restart);

    /* A retransmission still replays. */
    send(TXN_CMD, seq, sizeof seq);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, before_restart, sizeof before_restart);

    /* The host restarted: it re-handshakes and its transaction counter starts
     * over at 0. The old entry must be gone, or the first action of the new
     * host run would replay an answer it cannot have asked for (ADR-0004). */
    send(ECSP_CMD_HANDSHAKE, NULL, 0u);
    expect_answer(ECSP_CMD_HANDSHAKE, ECSP_RS_OK, NULL, 0u);

    send(TXN_CMD, seq, sizeof seq);
    CHECK_EQ(2, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, after_restart, sizeof after_restart);
}

static void test_cache_evicts_the_oldest_entry(void)
{
    int i;
    int slots = (int)ECSP_SLAVE_TXN_CACHE;

    init_slave();
    handshake();

    /* Fill every slot with a distinct sequence. */
    for (i = 1; i <= slots; i++) {
        uint8_t seq[1];
        uint8_t answer[2];

        seq[0] = (uint8_t)i;
        answer[0] = (uint8_t)i;
        answer[1] = (uint8_t)i; /* call number i */
        send(TXN_CMD, seq, 1u);
        expect_answer(TXN_CMD, ECSP_RS_OK, answer, sizeof answer);
    }
    CHECK_EQ(slots, g_txn_calls);

    /* Touching the oldest entry still replays it; a hit does not reorder the
     * FIFO (this is not an LRU cache). */
    {
        static const uint8_t first[1] = { 0x01u };
        static const uint8_t first_answer[2] = { 0x01u, 0x01u };

        send(TXN_CMD, first, sizeof first);
        CHECK_EQ(slots, g_txn_calls);
        expect_answer(TXN_CMD, ECSP_RS_OK, first_answer, sizeof first_answer);
    }

    /* One new transaction fills the cache again and must evict entry 1 (the
     * oldest), not entry 1's "recently used" neighbour. */
    {
        uint8_t seq[1];
        uint8_t answer[2];

        seq[0] = (uint8_t)(slots + 1);
        answer[0] = (uint8_t)(slots + 1);
        answer[1] = (uint8_t)(slots + 1);
        send(TXN_CMD, seq, 1u);
        CHECK_EQ(slots + 1, g_txn_calls);
        expect_answer(TXN_CMD, ECSP_RS_OK, answer, sizeof answer);
    }

    /* Entry 2 survived (FIFO evicted only the oldest entry). */
    {
        static const uint8_t second[1] = { 0x02u };
        static const uint8_t second_answer[2] = { 0x02u, 0x02u };

        send(TXN_CMD, second, sizeof second);
        CHECK_EQ(slots + 1, g_txn_calls);
        expect_answer(TXN_CMD, ECSP_RS_OK, second_answer, sizeof second_answer);
    }

    /* Entry 1 is gone: the transaction executes again, and is cached again. */
    {
        static const uint8_t first[1] = { 0x01u };
        uint8_t answer[2];

        answer[0] = 0x01u;
        answer[1] = (uint8_t)(slots + 2);
        send(TXN_CMD, first, sizeof first);
        CHECK_EQ(slots + 2, g_txn_calls);
        expect_answer(TXN_CMD, ECSP_RS_OK, answer, sizeof answer);

        send(TXN_CMD, first, sizeof first);
        CHECK_EQ(slots + 2, g_txn_calls);
        expect_answer(TXN_CMD, ECSP_RS_OK, answer, sizeof answer);
    }
}

static void test_command_without_a_sequence_is_never_deduplicated(void)
{
    static const uint8_t payload[1] = { 0x33u };
    static const uint8_t first_answer[1] = { 0x01u };
    static const uint8_t second_answer[1] = { 0x02u };

    /* spec 4.2 exemption: getters, setters and idempotent actions carry no
     * transaction sequence, so the same bytes are simply two requests. */
    init_slave();
    handshake();

    send(PLAIN_CMD, payload, sizeof payload);
    CHECK_EQ(1, g_plain_calls);
    expect_answer(PLAIN_CMD, ECSP_RS_OK, first_answer, sizeof first_answer);

    send(PLAIN_CMD, payload, sizeof payload);
    CHECK_EQ(2, g_plain_calls);
    expect_answer(PLAIN_CMD, ECSP_RS_OK, second_answer, sizeof second_answer);
}

static void test_sequence_command_without_a_sequence_byte(void)
{
    /* A sequence-carrying command declares its own variable length; with an
     * empty payload there is no sequence byte to key on, so nothing may be
     * read past the payload and nothing is deduplicated. */
    init_slave();
    handshake();

    send(TXN_ANY_CMD, NULL, 0u);
    CHECK_EQ(1, g_any_calls);
    expect_answer(TXN_ANY_CMD, ECSP_RS_OK, NULL, 0u);

    send(TXN_ANY_CMD, NULL, 0u);
    CHECK_EQ(2, g_any_calls);
    expect_answer(TXN_ANY_CMD, ECSP_RS_OK, NULL, 0u);
}

static void test_system_reset_retransmits_replay_the_first_answer(void)
{
    static const uint8_t seq[1] = { 0x42u };
    static const uint8_t other_seq[1] = { 0x43u };

    init_slave();
    handshake();

    /* 0x07 answers first and only raises the reset request; the product
     * performs the actual MCU reset after draining (spec 5.2.7). */
    send(ECSP_CMD_SYSTEM_RESET, seq, sizeof seq);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));
    expect_answer(ECSP_CMD_SYSTEM_RESET, ECSP_RS_OK, NULL, 0u);

    /* Back-to-back retransmissions of the same transaction: the same 0x00
     * bytes, never 0x10. This also proves the handler did not run again: a
     * re-executed 0x07 would now answer 0x11 (a reset sequence is already in
     * progress), so byte-exact 0x00 answers can only come from the cache. */
    send(ECSP_CMD_SYSTEM_RESET, seq, sizeof seq);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));
    expect_answer(ECSP_CMD_SYSTEM_RESET, ECSP_RS_OK, NULL, 0u);

    send(ECSP_CMD_SYSTEM_RESET, seq, sizeof seq);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));
    expect_answer(ECSP_CMD_SYSTEM_RESET, ECSP_RS_OK, NULL, 0u);

    /* A *new* transaction during the reset sequence is refused 0x11: a second
     * concurrent reset must not start before the product has reset (spec
     * 5.2.7, 4.4). Criterion "a different sequence executes separately" still
     * holds for every ordinary action command (see the TXN_CMD tests). */
    send(ECSP_CMD_SYSTEM_RESET, other_seq, sizeof other_seq);
    CHECK_EQ(1, ecsp_slave_reset_requested(&g_slave));
    expect_answer(ECSP_CMD_SYSTEM_RESET, ECSP_RS_BUSY, NULL, 0u);

    /* Re-initialising the context models the reset the product just performed:
     * the session is waiting for a handshake again and the cache is empty, so
     * the same request is now refused (spec 4.1.2, ADR-0004). */
    init_slave();
    CHECK_EQ(0, ecsp_slave_reset_requested(&g_slave));
    send(ECSP_CMD_SYSTEM_RESET, seq, sizeof seq);
    expect_answer(ECSP_CMD_SYSTEM_RESET, ECSP_RS_NOT_HANDSHAKEN, NULL, 0u);
}

static void test_maximum_frame_answer_is_cached_and_replayed(void)
{
    static const uint8_t seq[1] = { 0x51u };
    uint8_t answer[BIG_DATA];
    size_t i;

    init_slave();
    handshake();

    for (i = 0u; i < BIG_DATA; i++) {
        answer[i] = (uint8_t)(seq[0] + (uint8_t)i);
    }

    /* The answer is a whole frame (ECSP_FRAME_MIN_LEN + 1 + BIG_DATA =
     * ECSP_MAX_FRAME bytes): it is delivered and replayed byte for byte, so a
     * cache slot covers the largest answer the encoder can produce. An answer
     * longer than that cannot be encoded at all, which is why the "too long to
     * cache, still send" branch takes a guard rather than a reachable test. */
    send(BIG_CMD, seq, sizeof seq);
    CHECK_EQ(1, g_big_calls);
    expect_answer(BIG_CMD, ECSP_RS_OK, answer, BIG_DATA);

    send(BIG_CMD, seq, sizeof seq);
    CHECK_EQ(1, g_big_calls);
    expect_answer(BIG_CMD, ECSP_RS_OK, answer, BIG_DATA);
}

static void test_rejected_action_is_not_cached(void)
{
    /* Only an answer that actually came from the handler may be cached. A
     * parameter error is decided at step 8, before the handler, so it is a
     * rejection rather than a completed transaction: retrying the same
     * sequence must re-run the gate, not replay the error from the cache. */
    static const uint8_t bad[2] = { 0x71u, 0x00u };
    static const uint8_t good[2] = { 0x71u, 0xAAu };
    static const uint8_t answer[2] = { 0x71u, 0x01u };

    init_slave();
    handshake();

    send(CHECKED_CMD, bad, sizeof bad);
    CHECK_EQ(0, g_txn_calls);
    expect_answer(CHECKED_CMD, ECSP_RS_BAD_PARAM, NULL, 0u);

    send(CHECKED_CMD, bad, sizeof bad);
    CHECK_EQ(0, g_txn_calls);
    expect_answer(CHECKED_CMD, ECSP_RS_BAD_PARAM, NULL, 0u);

    /* The accepted request runs once, then replays. */
    send(CHECKED_CMD, good, sizeof good);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(CHECKED_CMD, ECSP_RS_OK, answer, sizeof answer);

    send(CHECKED_CMD, good, sizeof good);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(CHECKED_CMD, ECSP_RS_OK, answer, sizeof answer);
}

static void test_replay_waits_for_transmit_room(void)
{
    /* Two retransmissions of one transaction arrive in a single receive burst
     * while the transmit FIFO can hold only one answer. The first answer is
     * queued and the second frame stays in the receive buffer, so the caller
     * can drain and step again: the replay is then delivered, and the handler
     * still ran exactly once. */
    static const uint8_t seq[1] = { 0x61u };
    static const uint8_t answer[2] = { 0x61u, 0x01u };
    uint8_t burst[2u * ECSP_MAX_FRAME];
    size_t one;
    size_t two;

    init_slave_cfg(g_small_table, sizeof g_small_table / sizeof g_small_table[0],
                   ECSP_FRAME_MIN_LEN + 1u + ECSP_SERIAL_LEN);
    handshake();

    one = ecsp_frame_encode(burst, sizeof burst, 0x03u, 0x02u, TXN_CMD,
                            seq, ECSP_TXN_SEQ_LEN);
    two = ecsp_frame_encode(&burst[one], sizeof burst - one, 0x03u, 0x02u,
                            TXN_CMD, seq, ECSP_TXN_SEQ_LEN);
    CHECK(one > 0u);
    CHECK(two > 0u);

    ecsp_slave_rx(&g_slave, burst, one + two);
    ecsp_slave_step(&g_slave, 0u);

    /* Exactly one answer fits; the retransmission is not consumed yet. */
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, answer, sizeof answer);

    ecsp_slave_step(&g_slave, 0u);
    CHECK_EQ(1, g_txn_calls);
    expect_answer(TXN_CMD, ECSP_RS_OK, answer, sizeof answer);
}

int main(void)
{
    test_same_sequence_replays_without_running_the_handler();
    test_different_sequences_execute_separately();
    test_handshake_clears_the_cache();
    test_cache_evicts_the_oldest_entry();
    test_command_without_a_sequence_is_never_deduplicated();
    test_sequence_command_without_a_sequence_byte();
    test_system_reset_retransmits_replay_the_first_answer();
    test_maximum_frame_answer_is_cached_and_replayed();
    test_rejected_action_is_not_cached();
    test_replay_waits_for_transmit_room();

    return ecsp_test_report();
}
