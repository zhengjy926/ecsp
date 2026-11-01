#include "ecsp/cmds.h"

#include <string.h>

/* kind: a request the slave executes, or an event the slave must not treat
 * as a request. 0x0F is absent. */
enum {
    CATALOG_REQUEST = 1,
    CATALOG_EVENT = 2
};

typedef struct {
    uint8_t cmd;
    uint8_t kind;
    uint8_t flags;
    uint16_t req_len;
    uint16_t resp_max;
    uint32_t resp_ms;
    uint8_t event_cmd;
    uint8_t query_cmd;
} catalog_entry_t;

/* Silence ceilings. The three classes and the handshake share one macro.
 * 0x07 names its own macro even while both numbers are 50, so loosening
 * that macro does not loosen the row. The host reads resp_ms and does
 * not look at flags. */
static const catalog_entry_t g_catalog[] = {
    { ECSP_CMD_HANDSHAKE, CATALOG_REQUEST, ECSP_CMD_PREEMPTIVE,
      0u, 0u, ECSP_RESP_DEFAULT_MS, 0u, 0u },
    { ECSP_CMD_GET_VERSION, CATALOG_REQUEST, ECSP_CMD_QUERY,
      0u, ECSP_VERSION_LEN, ECSP_RESP_DEFAULT_MS, 0u, 0u },
    { ECSP_CMD_SET_HW_VERSION, CATALOG_REQUEST, ECSP_CMD_NONE,
      ECSP_VERSION_LEN, 0u, ECSP_RESP_IDENTITY_MS, 0u, 0u },
    { ECSP_CMD_GET_HW_VERSION, CATALOG_REQUEST, ECSP_CMD_QUERY,
      0u, ECSP_VERSION_LEN, ECSP_RESP_DEFAULT_MS, 0u, 0u },
    { ECSP_CMD_SET_SERIAL, CATALOG_REQUEST, ECSP_CMD_NONE,
      ECSP_SERIAL_LEN, 0u, ECSP_RESP_IDENTITY_MS, 0u, 0u },
    { ECSP_CMD_GET_SERIAL, CATALOG_REQUEST, ECSP_CMD_QUERY,
      0u, ECSP_SERIAL_LEN, ECSP_RESP_DEFAULT_MS, 0u, 0u },
    { ECSP_CMD_SYSTEM_RESET, CATALOG_REQUEST,
      (uint8_t)(ECSP_CMD_PREEMPTIVE | ECSP_CMD_TXN_SEQ),
      ECSP_TXN_SEQ_LEN, 0u, ECSP_RESP_RESET_MS, 0u, 0u },
    { ECSP_CMD_CONTROL_SELFTEST, CATALOG_REQUEST,
      (uint8_t)(ECSP_CMD_PREEMPTIVE | ECSP_CMD_TXN_SEQ),
      (uint16_t)(ECSP_TXN_SEQ_LEN + ECSP_CTRL_LEN), 2u, ECSP_RESP_DEFAULT_MS,
      ECSP_CMD_SELFTEST_REPORT, ECSP_CMD_GET_SELFTEST },
    { ECSP_CMD_GET_SELFTEST, CATALOG_REQUEST, ECSP_CMD_QUERY,
      0u, 1u, ECSP_RESP_DEFAULT_MS, 0u, 0u },
    { ECSP_CMD_SELFTEST_REPORT, CATALOG_EVENT, ECSP_CMD_NONE,
      0u, 0u, 0u, 0u, 0u },
    { ECSP_CMD_SET_LOW_POWER, CATALOG_REQUEST, ECSP_CMD_NONE,
      1u, 0u, ECSP_RESP_DEFAULT_MS, 0u, 0u },
    { ECSP_CMD_START_UPGRADE, CATALOG_REQUEST,
      (uint8_t)(ECSP_CMD_PREEMPTIVE | ECSP_CMD_TXN_SEQ),
      (uint16_t)(ECSP_TXN_SEQ_LEN + ECSP_CTRL_LEN), 2u, ECSP_RESP_UPGRADE_MS,
      ECSP_CMD_UPGRADE_REPORT, ECSP_CMD_GET_UPGRADE },
    { ECSP_CMD_GET_UPGRADE, CATALOG_REQUEST, ECSP_CMD_QUERY,
      0u, 1u, ECSP_RESP_DEFAULT_MS, 0u, 0u },
    { ECSP_CMD_UPGRADE_REPORT, CATALOG_EVENT, ECSP_CMD_NONE,
      0u, 0u, 0u, 0u, 0u },
    { ECSP_CMD_HEARTBEAT, CATALOG_EVENT, ECSP_CMD_NONE,
      0u, 0u, 0u, 0u, 0u },
    { ECSP_CMD_FAULT_REPORT, CATALOG_EVENT, ECSP_CMD_NONE,
      0u, 0u, 0u, 0u, 0u },
    { ECSP_CMD_GET_FAULT, CATALOG_REQUEST, ECSP_CMD_QUERY,
      0u, 1u, ECSP_RESP_DEFAULT_MS, 0u, 0u },
};

#define ECSP_CATALOG_COUNT (sizeof g_catalog / sizeof g_catalog[0])

static const catalog_entry_t *find_catalog(uint8_t cmd)
{
    size_t i;

    for (i = 0u; i < ECSP_CATALOG_COUNT; i++) {
        if (g_catalog[i].cmd == cmd) {
            return &g_catalog[i];
        }
    }
    return NULL;
}

int ecsp_catalog_request(uint8_t cmd, uint8_t *flags, uint16_t *req_len,
                         uint16_t *resp_max)
{
    const catalog_entry_t *e = find_catalog(cmd);

    if (e == NULL || e->kind != CATALOG_REQUEST ||
        flags == NULL || req_len == NULL || resp_max == NULL) {
        return 0;
    }
    *flags = e->flags;
    *req_len = e->req_len;
    *resp_max = e->resp_max;
    return 1;
}

int ecsp_catalog_resp_ms(uint8_t cmd, uint32_t *resp_ms)
{
    const catalog_entry_t *e = find_catalog(cmd);

    if (e == NULL || e->kind != CATALOG_REQUEST || resp_ms == NULL) {
        return 0;
    }
    *resp_ms = e->resp_ms;
    return 1;
}

int ecsp_catalog_long_run(uint8_t action, uint8_t *event_cmd,
                          uint8_t *query_cmd)
{
    const catalog_entry_t *e = find_catalog(action);

    if (e == NULL || e->query_cmd == 0u ||
        event_cmd == NULL || query_cmd == NULL) {
        return 0;
    }
    *event_cmd = e->event_cmd;
    *query_cmd = e->query_cmd;
    return 1;
}

uint16_t ecsp_catalog_max_resp_data(void)
{
    size_t i;
    uint16_t max = 0u;

    for (i = 0u; i < ECSP_CATALOG_COUNT; i++) {
        if (g_catalog[i].kind == CATALOG_REQUEST &&
            g_catalog[i].resp_max > max) {
            max = g_catalog[i].resp_max;
        }
    }
    return max;
}

int ecsp_version_equals(const ecsp_version_t *a, const ecsp_version_t *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (memcmp(a->b, b->b, ECSP_VERSION_LEN) == 0) ? 1 : 0;
}

static int is_digit(uint8_t c)
{
    return (c >= (uint8_t)'0' && c <= (uint8_t)'9') ? 1 : 0;
}

int ecsp_version_parse(const uint8_t *p, size_t n, ecsp_version_t *v)
{
    if (v == NULL || p == NULL) {
        return ECSP_E_ARG;
    }
    if (n != ECSP_VERSION_LEN) {
        return ECSP_E_LEN;
    }
    /* "X.Y.Z.N": dots in slots 1, 3, 5; ASCII digits elsewhere. */
    if (p[1] != (uint8_t)'.' || p[3] != (uint8_t)'.' || p[5] != (uint8_t)'.' ||
        !is_digit(p[0]) || !is_digit(p[2]) || !is_digit(p[4]) ||
        !is_digit(p[6])) {
        return ECSP_E_VAL;
    }
    memcpy(v->b, p, ECSP_VERSION_LEN);
    return ECSP_OK;
}

int ecsp_serial_parse(const uint8_t *p, size_t n, ecsp_serial_t *sn)
{
    size_t i;

    if (sn == NULL || p == NULL) {
        return ECSP_E_ARG;
    }
    if (n != ECSP_SERIAL_LEN) {
        return ECSP_E_LEN;
    }
    if (p[0] != (uint8_t)'S' || p[1] != (uint8_t)'N') {
        return ECSP_E_VAL;
    }
    for (i = 2u; i < ECSP_SERIAL_LEN; i++) {
        if (!is_digit(p[i])) {
            return ECSP_E_VAL;
        }
    }
    memcpy(sn->b, p, ECSP_SERIAL_LEN);
    return ECSP_OK;
}

int ecsp_low_power_mode_valid(uint8_t mode)
{
    return (mode == ECSP_LOW_POWER_EXIT || mode == ECSP_LOW_POWER_ENTER) ? 1 : 0;
}
