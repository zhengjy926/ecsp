#include "txn_cache.h"

#if ECSP_ENABLE_SLAVE

#include <string.h>

#if ECSP_SLAVE_TXN_CACHE > 0

void ecsp_txn_cache_clear(ecsp_slave_t *s)
{
    s->txn_cache_head = 0u;
    s->txn_cache_count = 0u;
}

int ecsp_txn_cache_lookup(const ecsp_slave_t *s, uint8_t cmd, uint8_t seq,
                          const uint8_t **frame, size_t *frame_len)
{
    size_t i;

    for (i = 0u; i < s->txn_cache_count; i++) {
        const ecsp_slave_txn_entry_t *e =
            &s->txn_cache[(s->txn_cache_head + i) % ECSP_SLAVE_TXN_SLOTS];

        if (e->cmd == cmd && e->seq == seq) {
            *frame = e->frame;
            *frame_len = (size_t)e->frame_len;
            return 1;
        }
    }
    return 0;
}

int ecsp_txn_cache_store(ecsp_slave_t *s, uint8_t cmd, uint8_t seq,
                         const uint8_t *frame, size_t frame_len)
{
    ecsp_slave_txn_entry_t *e;

    if (frame_len > ECSP_MAX_FRAME) {
        /* The answer is delivered by the caller; only the cache copy is
         * skipped. The frame encoder already refuses frames this long, so this
         * guard only protects the slot from a future encoder change. */
        return ECSP_E_CAP;
    }

    if (s->txn_cache_count < ECSP_SLAVE_TXN_SLOTS) {
        e = &s->txn_cache[(s->txn_cache_head + s->txn_cache_count) %
                          ECSP_SLAVE_TXN_SLOTS];
        s->txn_cache_count++;
    } else {
        /* Full: overwrite the oldest entry (FIFO), not the least recently used. */
        e = &s->txn_cache[s->txn_cache_head];
        s->txn_cache_head = (s->txn_cache_head + 1u) % ECSP_SLAVE_TXN_SLOTS;
    }

    e->cmd = cmd;
    e->seq = seq;
    e->frame_len = (uint16_t)frame_len;
    memcpy(e->frame, frame, frame_len);
    return ECSP_OK;
}

#else /* ECSP_SLAVE_TXN_CACHE == 0: deduplication is off */

void ecsp_txn_cache_clear(ecsp_slave_t *s)
{
    (void)s;
}

int ecsp_txn_cache_lookup(const ecsp_slave_t *s, uint8_t cmd, uint8_t seq,
                          const uint8_t **frame, size_t *frame_len)
{
    (void)s;
    (void)cmd;
    (void)seq;
    (void)frame;
    (void)frame_len;
    return 0;
}

int ecsp_txn_cache_store(ecsp_slave_t *s, uint8_t cmd, uint8_t seq,
                         const uint8_t *frame, size_t frame_len)
{
    (void)s;
    (void)cmd;
    (void)seq;
    (void)frame;
    (void)frame_len;
    return ECSP_OK;
}

#endif /* ECSP_SLAVE_TXN_CACHE */

#endif /* ECSP_ENABLE_SLAVE */
