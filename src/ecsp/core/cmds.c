#include "ecsp/cmds.h"

#include <string.h>

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
