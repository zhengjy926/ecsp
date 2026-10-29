#include "ecsp/ecsp.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *hex;
    const char *spec_ref;
} vector_t;

static const vector_t g_vectors[] = {
#define ECSP_VECTOR(name, hex, ref) { #name, hex, ref },
#include "vectors.h"
#undef ECSP_VECTOR
};

#define VECTOR_COUNT (sizeof(g_vectors) / sizeof(g_vectors[0]))

static int g_checks = 0;
static int g_failures = 0;
static const char *g_ctx = NULL;

static void fail(const char *file, int line, const char *msg)
{
    g_failures++;
    printf("FAIL %s:%d", file, line);
    if (g_ctx != NULL) {
        printf(" [%s]", g_ctx);
    }
    printf(": %s\n", msg);
}

#define CHECK(cond) \
    do { \
        g_checks++; \
        if (!(cond)) { \
            fail(__FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(want, got) \
    do { \
        g_checks++; \
        if ((long)(want) != (long)(got)) { \
            char buf_[96]; \
            (void)snprintf(buf_, sizeof buf_, "%s == %s (want %ld, got %ld)", \
                           #got, #want, (long)(want), (long)(got)); \
            fail(__FILE__, __LINE__, buf_); \
        } \
    } while (0)

static unsigned hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (unsigned)(c - 'A' + 10);
    }
    return 0x100u;
}

static size_t hex_decode(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0u;
    while (*s != '\0') {
        unsigned hi;
        unsigned lo;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        hi = hexval((unsigned char)*s);
        s++;
        lo = hexval((unsigned char)*s);
        s++;
        if (hi > 15u || lo > 15u) {
            return 0u;
        }
        if (n < cap) {
            out[n] = (uint8_t)((hi << 4) | lo);
        }
        n++;
    }
    return n;
}

static void test_crc_known_value(void)
{
    /* CRC16-MODBUS check string "123456789" -> 0x4B37. */
    static const uint8_t check_string[] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9'
    };

    g_ctx = "crc16-modbus check value";
    CHECK_EQ(0x4B37u, ecsp_crc16_modbus(check_string, sizeof check_string));
    g_ctx = NULL;
}

static void test_one_vector(const vector_t *v)
{
    uint8_t bytes[ECSP_MAX_FRAME];
    size_t n = hex_decode(v->hex, bytes, sizeof bytes);

    CHECK_EQ(1u, n >= 9u);
    if (n < 9u) {
        return;
    }

    CHECK_EQ(ECSP_FRAME_HEADER, bytes[0]);
    CHECK_EQ(ECSP_FRAME_TAIL, bytes[n - 1u]);

    {
        uint16_t frame_len = ecsp_rd_u16le(&bytes[1]);
        uint16_t embedded = ecsp_rd_u16le(&bytes[n - 3u]);
        uint16_t calc = ecsp_crc16_modbus(&bytes[1], n - 4u);
        ecsp_frame_t frame;
        size_t consumed = 0u;
        ecsp_scan_t sc = ecsp_frame_scan(bytes, n, &frame, &consumed);

        CHECK_EQ(frame_len, n);
        CHECK_EQ(embedded, calc);
        CHECK_EQ(ECSP_SCAN_OK, sc);
        CHECK_EQ(n, consumed);
        CHECK_EQ(bytes[3], frame.product_addr);
        CHECK_EQ(bytes[4], frame.cmd);
        CHECK_EQ(bytes[5], frame.board_addr);
        CHECK_EQ(frame_len - 9u, frame.payload_len);
    }
}

static void test_vectors(void)
{
    size_t vi;
    for (vi = 0u; vi < VECTOR_COUNT; vi++) {
        g_ctx = g_vectors[vi].name;
        test_one_vector(&g_vectors[vi]);
        g_ctx = NULL;
    }
}

static void test_version_resp_crc_contains_header_byte(void)
{
    size_t vi;
    const vector_t *v = NULL;
    uint8_t bytes[ECSP_MAX_FRAME];
    size_t n;
    uint16_t embedded;
    ecsp_frame_t frame;
    size_t consumed = 0u;

    for (vi = 0u; vi < VECTOR_COUNT; vi++) {
        if (strcmp(g_vectors[vi].name, "version_resp") == 0) {
            v = &g_vectors[vi];
            break;
        }
    }

    g_ctx = "version_resp 0xFA-in-CRC";
    CHECK(v != NULL);
    if (v == NULL) {
        g_ctx = NULL;
        return;
    }

    n = hex_decode(v->hex, bytes, sizeof bytes);
    CHECK_EQ(1u, n >= 9u);
    if (n < 9u) {
        g_ctx = NULL;
        return;
    }

    embedded = ecsp_rd_u16le(&bytes[n - 3u]);
    /* The CRC low byte is 0xFA, the frame-header byte (spec 5.2.2). */
    CHECK_EQ(0x0EFAu, embedded);
    CHECK_EQ(ECSP_FRAME_HEADER, embedded & 0xFFu);

    /* The scanner sizes the frame from its length field and never re-searches
     * for 0xFA inside the CRC, so this frame must parse in one piece. */
    CHECK_EQ(ECSP_SCAN_OK, ecsp_frame_scan(bytes, n, &frame, &consumed));
    CHECK_EQ(n, consumed);
    CHECK_EQ(8u, frame.payload_len);
    CHECK_EQ(0x00u, frame.payload[0]);
    CHECK_EQ(0x31u, frame.payload[1]); /* '1' */
    CHECK_EQ(0x2Eu, frame.payload[2]); /* '.' */
    g_ctx = NULL;
}

static void test_encode_roundtrip(void)
{
    size_t vi;
    for (vi = 0u; vi < VECTOR_COUNT; vi++) {
        uint8_t bytes[ECSP_MAX_FRAME];
        uint8_t enc[ECSP_MAX_FRAME];
        size_t n = hex_decode(g_vectors[vi].hex, bytes, sizeof bytes);
        const uint8_t *payload;
        size_t out_len;

        g_ctx = g_vectors[vi].name;
        CHECK_EQ(1u, n >= 9u);
        if (n < 9u) {
            g_ctx = NULL;
            continue;
        }

        payload = (n > 9u) ? &bytes[6] : NULL;
        out_len = ecsp_frame_encode(enc, sizeof enc, bytes[3], bytes[5],
                                    bytes[4], payload, (uint16_t)(n - 9u));
        CHECK_EQ(n, out_len);
        if (out_len == n) {
            CHECK(memcmp(enc, bytes, n) == 0);
        }
        g_ctx = NULL;
    }
}

static void test_encode_capacity(void)
{
    uint8_t buf[ECSP_MAX_FRAME];

    g_ctx = "encode capacity";
    /* Capacity below the minimum frame length -> rejected. */
    CHECK_EQ((size_t)0u, ecsp_frame_encode(buf, 8u, 0x03u, 0x02u, 0x01u, NULL, 0u));
    /* Exactly the minimum frame length -> accepted. */
    CHECK_EQ((size_t)9u, ecsp_frame_encode(buf, 9u, 0x03u, 0x02u, 0x01u, NULL, 0u));
    g_ctx = NULL;
}

int main(void)
{
    test_crc_known_value();
    test_vectors();
    test_version_resp_crc_contains_header_byte();
    test_encode_roundtrip();
    test_encode_capacity();

    if (g_failures != 0) {
        printf("%d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("OK: %d checks passed\n", g_checks);
    return 0;
}
