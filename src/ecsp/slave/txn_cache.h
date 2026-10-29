#ifndef ECSP_SLAVE_TXN_CACHE_H
#define ECSP_SLAVE_TXN_CACHE_H

/*
 * L3 transaction-sequence cache (spec 4.2, ADR-0004). Internal to the slave
 * engine: these functions are not part of the public API.
 *
 * A non-idempotent action command puts a one-byte transaction sequence in its
 * first payload byte. The engine caches the answer frame under
 * (command code, sequence) and replays it byte for byte when the host
 * retransmits the same transaction after a lost answer. The cache is a FIFO:
 * when it is full the oldest entry is overwritten.
 *
 * The cache lives inside ecsp_slave_t, so it needs no allocation and no
 * caller-owned storage. ECSP_SLAVE_TXN_CACHE sets the number of entries;
 * 0 turns deduplication off.
 */

#include <stddef.h>
#include <stdint.h>

#include "ecsp/slave.h"

/* 1 when a cached answer for (cmd, seq) exists. On a hit *frame and *frame_len
 * describe the complete answer frame to copy into the transmit FIFO; the
 * pointer stays valid until the next cache mutation. */
int ecsp_txn_cache_lookup(const ecsp_slave_t *s, uint8_t cmd, uint8_t seq,
                          const uint8_t **frame, size_t *frame_len);

/* Cache `frame` (a complete answer frame, `frame_len` bytes) under
 * (cmd, seq). Returns ECSP_OK, or ECSP_E_CAP when the frame is longer than one
 * cache slot (ECSP_MAX_FRAME): such an answer is not cacheable, but the caller
 * has already queued it and must still deliver it to the host. */
int ecsp_txn_cache_store(ecsp_slave_t *s, uint8_t cmd, uint8_t seq,
                         const uint8_t *frame, size_t frame_len);

/* Drop every entry. Called on initialisation (which models the slave's soft
 * reset) and whenever an accepted handshake request arrives (ADR-0004). */
void ecsp_txn_cache_clear(ecsp_slave_t *s);

#endif /* ECSP_SLAVE_TXN_CACHE_H */
