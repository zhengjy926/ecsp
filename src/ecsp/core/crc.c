#include "ecsp/crc.h"

uint16_t ecsp_crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;

    for (i = 0u; i < len; i++) {
        int b;

        crc = (uint16_t)(crc ^ (uint16_t)data[i]);
        for (b = 0; b < 8; b++) {
            if ((crc & 1u) != 0u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}
