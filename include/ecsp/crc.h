#ifndef ECSP_CRC_H
#define ECSP_CRC_H

#include <stddef.h>
#include <stdint.h>

/* CRC16-MODBUS over `data[0..len)`. For a frame this covers the bytes from
 * the first byte of the length field through the last byte of the payload
 * (spec 3.2). */
uint16_t ecsp_crc16_modbus(const uint8_t *data, size_t len);

#endif /* ECSP_CRC_H */
