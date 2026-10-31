#ifndef FLASH_IDENTITY_H
#define FLASH_IDENTITY_H

#include "ecsp/slave.h"

/*
 * Sector-11 identity store (parent #13). 32-byte records, 32-bit program
 * width. Append while space remains; erase the sector only when full.
 * Does not touch option bytes (single-bank / DB1M clear).
 */

void flash_identity_load(ecsp_slave_identity_t *out,
                         const ecsp_slave_identity_t *blank);

int flash_identity_commit(const ecsp_slave_identity_t *next);

#endif /* FLASH_IDENTITY_H */
