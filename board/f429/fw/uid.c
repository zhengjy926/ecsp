#include "uid.h"

#include <string.h>

#include "stm32f4xx_hal.h"

void uid_read(uint8_t out[UID_LEN])
{
    /* Chip byte order as laid out at UID_BASE; no numeric reinterpretation. */
    memcpy(out, (const void *)UID_BASE, UID_LEN);
}
