#ifndef ECSP_FRAME_H
#define ECSP_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define ECSP_FRAME_HEADER 0xFAu
#define ECSP_FRAME_TAIL   0x0Du

/* Smallest legal frame: header + length(2) + product + cmd + board + crc(2)
 * + tail, with an empty payload. */
#define ECSP_FRAME_MIN_LEN 9u

typedef struct {
    uint8_t        product_addr;
    uint8_t        board_addr;
    uint8_t        cmd;
    const uint8_t *payload;   /* points into the caller's buffer (zero-copy) */
    uint16_t       payload_len;
} ecsp_frame_t;

typedef enum {
    ECSP_SCAN_NEED_MORE = 0,  /* buffer has no complete frame yet */
    ECSP_SCAN_OK,             /* a frame was parsed; *consumed is its length */
    ECSP_SCAN_SKIPPED         /* *consumed bytes are garbage and were dropped */
} ecsp_scan_t;

/* Scan `buf[0..len)` for one frame.
 *
 * On ECSP_SCAN_OK, *out is filled (payload points into `buf`) and *consumed
 * is the full frame length. On ECSP_SCAN_SKIPPED, *consumed bytes should be
 * removed before the next call. On ECSP_SCAN_NEED_MORE, *consumed is either 0
 * (a partial frame was kept) or the count of leading non-header bytes dropped.
 *
 * The scanner never re-searches for 0xFA inside a frame whose length, tail and
 * CRC are consistent, so a 0xFA that appears in the payload or CRC bytes does
 * not break framing (spec 3.2, issue #2). */
ecsp_scan_t ecsp_frame_scan(const uint8_t *buf, size_t len,
                            ecsp_frame_t *out, size_t *consumed);

/* Encode a frame into `buf` (capacity `cap`). Returns the frame length, or 0
 * if `cap` is too small, `payload_len` exceeds the frame limit, or `payload`
 * is NULL while `payload_len` is non-zero. */
size_t ecsp_frame_encode(uint8_t *buf, size_t cap,
                         uint8_t product_addr, uint8_t board_addr,
                         uint8_t cmd, const uint8_t *payload, uint16_t payload_len);

#endif /* ECSP_FRAME_H */
