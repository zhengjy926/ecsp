#ifndef ECSP_BYTES_H
#define ECSP_BYTES_H

#include <stdint.h>

/* Little-endian multi-byte helpers (ADR-0006: byte-wise codec; never map a
 * struct over a receive buffer). */

uint16_t ecsp_rd_u16le(const uint8_t *p);
uint32_t ecsp_rd_u32le(const uint8_t *p);
void ecsp_wr_u16le(uint8_t *p, uint16_t v);
void ecsp_wr_u32le(uint8_t *p, uint32_t v);

#endif /* ECSP_BYTES_H */
