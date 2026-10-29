/*
 * L1 frame scanner: incremental "bytes in, frame out" behavior (ticket #3).
 *
 * These pin the scanner contract the slave engine leans on: a partial frame
 * yields NEED_MORE and is retried when more bytes arrive; two glued frames are
 * consumed one at a time; a frame whose payload or CRC contains 0xFA / 0x0D is
 * sized from its length field, never by re-searching for a header/tail byte.
 *
 * The scanner itself landed in ticket #2; this file is the behavioral
 * regression for its incremental path (ticket #3 acceptance criteria 3, 4, 5).
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

/* spec 5.2.1 handshake request, P=0x03 B=0x02 (identical to vector handshake_req). */
static const uint8_t handshake_req[] = {
    0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
};

static void test_scan_partial_then_complete(void)
{
    ecsp_frame_t f;
    size_t consumed = 0u;

    /* A 4-byte prefix of a 9-byte frame: sized but incomplete -> NEED_MORE,
     * nothing to drop. */
    CHECK_EQ(ECSP_SCAN_NEED_MORE, ecsp_frame_scan(handshake_req, 4u, &f, &consumed));
    CHECK_EQ(0u, consumed);

    /* The whole frame parses in one piece. */
    CHECK_EQ(ECSP_SCAN_OK, ecsp_frame_scan(handshake_req, sizeof handshake_req, &f, &consumed));
    CHECK_EQ(sizeof handshake_req, consumed);
    CHECK_EQ(0x03u, f.product_addr);
    CHECK_EQ(0x01u, f.cmd);
    CHECK_EQ(0x02u, f.board_addr);
    CHECK_EQ(0u, f.payload_len);
}

static void test_scan_leading_garbage(void)
{
    static const uint8_t buf[] = {
        0x11, 0x22,                                  /* line noise */
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
    };
    ecsp_frame_t f;
    size_t consumed = 0u;

    CHECK_EQ(ECSP_SCAN_OK, ecsp_frame_scan(buf, sizeof buf, &f, &consumed));
    CHECK_EQ(sizeof buf, consumed);   /* 2 garbage bytes + 9-byte frame */
    CHECK_EQ(0x01u, f.cmd);

    /* Garbage with no header in sight is dropped and reported as NEED_MORE. */
    CHECK_EQ(ECSP_SCAN_NEED_MORE, ecsp_frame_scan(buf, 2u, &f, &consumed));
    CHECK_EQ(2u, consumed);
}

static void test_scan_two_glued_frames(void)
{
    static const uint8_t buf[] = {
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D,
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0D
    };
    ecsp_frame_t f;
    size_t consumed = 0u;
    const uint8_t *p = buf;
    size_t left = sizeof buf;

    /* Two complete frames, one scan call each, without polluting each other. */
    CHECK_EQ(ECSP_SCAN_OK, ecsp_frame_scan(p, left, &f, &consumed));
    CHECK_EQ(9u, consumed);
    CHECK_EQ(0x01u, f.cmd);
    CHECK_EQ(0u, f.payload_len);

    p += consumed;
    left -= consumed;
    CHECK_EQ(ECSP_SCAN_OK, ecsp_frame_scan(p, left, &f, &consumed));
    CHECK_EQ(9u, consumed);
    CHECK_EQ(0x01u, f.cmd);
    CHECK_EQ(0u, f.payload_len);

    p += consumed;
    left -= consumed;
    CHECK_EQ(ECSP_SCAN_NEED_MORE, ecsp_frame_scan(p, left, &f, &consumed));
    CHECK_EQ(0u, consumed);
}

static void test_scan_bad_crc_skips_whole_frame(void)
{
    /* handshake_req with the CRC low byte corrupted (0x88 -> 0x89). */
    static const uint8_t buf[] = {
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x89, 0x50, 0x0D
    };
    ecsp_frame_t f;
    size_t consumed = 0u;

    CHECK_EQ(ECSP_SCAN_SKIPPED, ecsp_frame_scan(buf, sizeof buf, &f, &consumed));
    CHECK_EQ(9u, consumed);   /* the whole frame is discarded, not just the header */
}

static void test_scan_bad_tail_skips_header(void)
{
    /* handshake_req with the tail corrupted (0x0D -> 0x0E). */
    static const uint8_t buf[] = {
        0xFA, 0x09, 0x00, 0x03, 0x01, 0x02, 0x88, 0x50, 0x0E
    };
    ecsp_frame_t f;
    size_t consumed = 0u;

    CHECK_EQ(ECSP_SCAN_SKIPPED, ecsp_frame_scan(buf, sizeof buf, &f, &consumed));
    CHECK_EQ(1u, consumed);   /* only the 0xFA is skipped; the scanner rescans after it */
}

static void test_scan_reserved_bytes_in_payload_and_crc(void)
{
    /* 14-byte frame whose payload carries both 0x0D and 0xFA. The scanner must
     * size the frame from its length field (0x0E) and read the tail at the
     * computed offset, never stopping at the 0x0D / 0xFA inside the payload. */
    static const uint8_t buf[] = {
        0xFA, 0x0E, 0x00, 0x03, 0x30, 0x02, 0xDE, 0x0D, 0xAD, 0xFA, 0xBE,
        0x99, 0x88, 0x0D
    };
    ecsp_frame_t f;
    size_t consumed = 0u;

    CHECK_EQ(ECSP_SCAN_OK, ecsp_frame_scan(buf, sizeof buf, &f, &consumed));
    CHECK_EQ(sizeof buf, consumed);   /* 14 bytes, in one piece */
    CHECK_EQ(0x30u, f.cmd);
    CHECK_EQ(5u, f.payload_len);
    CHECK_EQ(0xDEu, f.payload[0]);
    CHECK_EQ(0x0Du, f.payload[1]);
    CHECK_EQ(0xADu, f.payload[2]);
    CHECK_EQ(0xFAu, f.payload[3]);
    CHECK_EQ(0xBEu, f.payload[4]);
}

int main(void)
{
    test_scan_partial_then_complete();
    test_scan_leading_garbage();
    test_scan_two_glued_frames();
    test_scan_bad_crc_skips_whole_frame();
    test_scan_bad_tail_skips_header();
    test_scan_reserved_bytes_in_payload_and_crc();

    return ecsp_test_report();
}
