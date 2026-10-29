#include "ecsp/frame.h"

#include "ecsp/bytes.h"
#include "ecsp/config.h"
#include "ecsp/crc.h"

ecsp_scan_t ecsp_frame_scan(const uint8_t *buf, size_t len,
                            ecsp_frame_t *out, size_t *consumed)
{
    size_t i = 0u;
    uint16_t frame_len;
    uint16_t crc;
    uint16_t calc;

    /* Find the next frame header. */
    while (i < len && buf[i] != ECSP_FRAME_HEADER) {
        i++;
    }
    if (i == len) {
        /* No header in sight: drop the scanned garbage. */
        *consumed = i;
        return ECSP_SCAN_NEED_MORE;
    }

    /* Need header + 2 length bytes before the frame can be sized. */
    if (len - i < 3u) {
        *consumed = i;
        return ECSP_SCAN_NEED_MORE;
    }

    frame_len = ecsp_rd_u16le(&buf[i + 1u]);
    if (frame_len < ECSP_FRAME_MIN_LEN || frame_len > ECSP_MAX_FRAME) {
        /* Bad length: skip this 0xFA and rescan after it. */
        *consumed = i + 1u;
        return ECSP_SCAN_SKIPPED;
    }

    if (len - i < (size_t)frame_len) {
        /* A complete frame is not here yet. */
        *consumed = i;
        return ECSP_SCAN_NEED_MORE;
    }

    if (buf[i + (size_t)frame_len - 1u] != ECSP_FRAME_TAIL) {
        /* Bad tail: skip this 0xFA and rescan after it. */
        *consumed = i + 1u;
        return ECSP_SCAN_SKIPPED;
    }

    crc = ecsp_rd_u16le(&buf[i + (size_t)frame_len - 3u]);
    calc = ecsp_crc16_modbus(&buf[i + 1u], (size_t)frame_len - 4u);
    if (crc != calc) {
        /* Bad CRC: discard the whole frame. */
        *consumed = i + (size_t)frame_len;
        return ECSP_SCAN_SKIPPED;
    }

    out->product_addr = buf[i + 3u];
    out->cmd = buf[i + 4u];
    out->board_addr = buf[i + 5u];
    out->payload = &buf[i + 6u];
    out->payload_len = (uint16_t)((size_t)frame_len - ECSP_FRAME_MIN_LEN);
    *consumed = i + (size_t)frame_len;
    return ECSP_SCAN_OK;
}

size_t ecsp_frame_encode(uint8_t *buf, size_t cap,
                         uint8_t product_addr, uint8_t board_addr,
                         uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    size_t frame_len = ECSP_FRAME_MIN_LEN + (size_t)payload_len;
    uint16_t crc;

    if (frame_len > ECSP_MAX_FRAME || frame_len > cap) {
        return 0u;
    }
    if (payload_len > 0u && payload == NULL) {
        return 0u;
    }

    buf[0] = ECSP_FRAME_HEADER;
    ecsp_wr_u16le(&buf[1], (uint16_t)frame_len);
    buf[3] = product_addr;
    buf[4] = cmd;
    buf[5] = board_addr;
    if (payload_len > 0u) {
        size_t i;
        for (i = 0u; i < payload_len; i++) {
            buf[6u + i] = payload[i];
        }
    }
    crc = ecsp_crc16_modbus(&buf[1], frame_len - 4u);
    ecsp_wr_u16le(&buf[6u + (size_t)payload_len], crc);
    buf[6u + (size_t)payload_len + 2u] = ECSP_FRAME_TAIL;
    return frame_len;
}
