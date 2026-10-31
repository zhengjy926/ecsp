#include "bringup.h"

#include <string.h>

#include "ecsp/bytes.h"

/* One board on this link. The handler signature has no user pointer, so the
 * RAM for the test byte and the counter lives here. f429_board_init is how a
 * reset clears it. The protocol library stays free of this state. */
typedef struct {
    uint8_t chip_id[F429_CHIP_ID_LEN];
    uint8_t test_byte;
    uint32_t counter;
} f429_ram_t;

static f429_ram_t g_ram;

void f429_board_init(const uint8_t chip_id[F429_CHIP_ID_LEN])
{
    memset(&g_ram, 0, sizeof g_ram);
    if (chip_id != NULL) {
        memcpy(g_ram.chip_id, chip_id, F429_CHIP_ID_LEN);
    }
}

static int on_get_chip_id(ecsp_slave_t *s, const ecsp_frame_t *req,
                          uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)cap;
    memcpy(resp, g_ram.chip_id, F429_CHIP_ID_LEN);
    *resp_len = F429_CHIP_ID_LEN;
    return ECSP_RS_OK;
}

static int on_get_test_byte(ecsp_slave_t *s, const ecsp_frame_t *req,
                            uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)cap;
    resp[0] = g_ram.test_byte;
    *resp_len = 1u;
    return ECSP_RS_OK;
}

static int on_set_test_byte(ecsp_slave_t *s, const ecsp_frame_t *req,
                            uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)resp;
    (void)cap;
    g_ram.test_byte = req->payload[0];
    *resp_len = 0u;
    return ECSP_RS_OK;
}

static int on_bump_counter(ecsp_slave_t *s, const ecsp_frame_t *req,
                           uint8_t *resp, size_t cap, size_t *resp_len)
{
    (void)s;
    (void)req;
    (void)cap;
    g_ram.counter++;
    ecsp_wr_u32le(resp, g_ram.counter);
    *resp_len = 4u;
    return ECSP_RS_OK;
}

static const ecsp_cmd_entry_t g_table[] = {
    { F429_CMD_GET_CHIP_ID, ECSP_CMD_QUERY, 0u, F429_CHIP_ID_LEN,
      NULL, on_get_chip_id },
    { F429_CMD_GET_TEST_BYTE, ECSP_CMD_QUERY, 0u, 1u,
      NULL, on_get_test_byte },
    { F429_CMD_SET_TEST_BYTE, 0u, 1u, 0u,
      NULL, on_set_test_byte },
    { F429_CMD_BUMP_COUNTER, ECSP_CMD_TXN_SEQ, 1u, 4u,
      NULL, on_bump_counter }
};

static const ecsp_cmd_table_t g_cmds = {
    g_table,
    sizeof g_table / sizeof g_table[0]
};

const ecsp_cmd_table_t *f429_board_cmds(void)
{
    return &g_cmds;
}
