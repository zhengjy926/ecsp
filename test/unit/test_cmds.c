/*
 * L2 common-command codecs: software version (ticket #4) and the fixed-layout
 * payloads of the passthrough commands (ticket #5).
 *
 * The version is a fixed 7-byte string (spec 5.2.2); comparison is a byte-wise
 * equality over all 7 bytes (spec 9.2). It must not be parsed numerically and
 * must not be treated as a NUL-terminated C string, so versions that differ
 * only after an embedded 0x00 byte must compare unequal.
 *
 * Hardware version (5.2.3) uses the same "X.Y.Z.N" layout; serial number
 * (5.2.5) is "SN" followed by 10 ASCII digits. Length errors and value errors
 * stay distinct here (ECSP_E_LEN vs ECSP_E_VAL): the pipeline maps them to
 * 0x02 and 0x03 respectively.
 */

#include "ecsp/ecsp.h"

#include "test_util.h"

static void test_equal_versions(void)
{
    ecsp_version_t a = { { '1', '.', '0', '.', '0', '.', '0' } };
    ecsp_version_t b = { { '1', '.', '0', '.', '0', '.', '0' } };

    CHECK_EQ(1, ecsp_version_equals(&a, &b));
    CHECK_EQ(1, ecsp_version_equals(&b, &a));
}

static void test_differs_in_last_byte(void)
{
    ecsp_version_t a = { { '1', '.', '0', '.', '0', '.', '0' } };
    ecsp_version_t b = { { '1', '.', '0', '.', '0', '.', '9' } };

    CHECK_EQ(0, ecsp_version_equals(&a, &b));
}

static void test_no_nul_termination_assumption(void)
{
    /* A version whose 7th byte is 0x00 is still a full 7-byte value: a version
     * differing only in that last byte must compare unequal. */
    ecsp_version_t a = { { '1', '.', '0', '.', '0', '.', 0x00 } };
    ecsp_version_t b = { { '1', '.', '0', '.', '0', '.', 0x01 } };
    ecsp_version_t c = { { '1', '.', '0', '.', '0', '.', 0x00 } };

    CHECK_EQ(0, ecsp_version_equals(&a, &b));
    CHECK_EQ(1, ecsp_version_equals(&a, &c));
}

static void test_embedded_nul_is_significant(void)
{
    /* A 0x00 inside the 7 bytes is significant: a strcmp-style comparison would
     * stop there and wrongly report "equal". */
    ecsp_version_t a = { { '1', 0x00, '0', '.', '0', '.', '0' } };
    ecsp_version_t b = { { '1', 0x00, '0', '.', '0', '.', '0' } };
    ecsp_version_t c = { { '1', 0x01, '0', '.', '0', '.', '0' } };

    CHECK_EQ(1, ecsp_version_equals(&a, &b));
    CHECK_EQ(0, ecsp_version_equals(&a, &c));
}

/* spec 5.2.3: hardware version carries the same 7-byte "X.Y.Z.N" layout as
 * the software version. Parse validates length and shape, then copies. */
static void test_version_parse_valid(void)
{
    static const uint8_t raw[7] = { '1', '.', '0', '.', '0', '.', '9' };
    ecsp_version_t v;

    CHECK_EQ(ECSP_OK, ecsp_version_parse(raw, sizeof raw, &v));
    CHECK_EQ(0, memcmp(v.b, raw, sizeof raw));
}

static void test_version_parse_length_error(void)
{
    static const uint8_t raw[6] = { '1', '.', '0', '.', '0', '9' };
    ecsp_version_t v = { { 0 } };

    CHECK_EQ(ECSP_E_LEN, ecsp_version_parse(raw, sizeof raw, &v));
    /* A failed parse must not modify the output. */
    CHECK_EQ(0, v.b[0]);
}

static void test_version_parse_value_error(void)
{
    /* Wrong dot positions and non-digit characters are value errors, not
     * length errors (spec 5.2.3, 5.2.5: 0x03 vs 0x02). */
    static const uint8_t no_dots[7] = { '1', '0', '0', '0', '0', '0', '0' };
    static const uint8_t bad_digit[7] = { '1', '.', '0', '.', '0', '.', 'X' };
    static const uint8_t bad_dot[7] = { '1', '.', '0', '.', '0', ',', '0' };
    ecsp_version_t v;

    CHECK_EQ(ECSP_E_VAL, ecsp_version_parse(no_dots, sizeof no_dots, &v));
    CHECK_EQ(ECSP_E_VAL, ecsp_version_parse(bad_digit, sizeof bad_digit, &v));
    CHECK_EQ(ECSP_E_VAL, ecsp_version_parse(bad_dot, sizeof bad_dot, &v));
}

/* spec 5.2.5: serial number is "SN" + 10 ASCII digits, fixed 12 bytes. */
static void test_serial_parse_valid(void)
{
    static const uint8_t raw[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };
    ecsp_serial_t sn;

    CHECK_EQ(ECSP_OK, ecsp_serial_parse(raw, sizeof raw, &sn));
    CHECK_EQ(0, memcmp(sn.b, raw, sizeof raw));
}

static void test_serial_parse_length_error(void)
{
    static const uint8_t raw[11] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };
    ecsp_serial_t sn = { { 0 } };

    CHECK_EQ(ECSP_E_LEN, ecsp_serial_parse(raw, sizeof raw, &sn));
    CHECK_EQ(0, sn.b[0]);
}

static void test_serial_parse_value_error(void)
{
    static const uint8_t bad_prefix[12] = {
        'X', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
    };
    static const uint8_t bad_digit[12] = {
        'S', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '0', 'A'
    };
    ecsp_serial_t sn;

    CHECK_EQ(ECSP_E_VAL, ecsp_serial_parse(bad_prefix, sizeof bad_prefix, &sn));
    CHECK_EQ(ECSP_E_VAL, ecsp_serial_parse(bad_digit, sizeof bad_digit, &sn));
}

/* spec 5.2.11: only modes 0x00 (exit) and 0x01 (enter) are legal. */
static void test_low_power_mode_valid(void)
{
    CHECK_EQ(1, ecsp_low_power_mode_valid(ECSP_LOW_POWER_EXIT));
    CHECK_EQ(1, ecsp_low_power_mode_valid(ECSP_LOW_POWER_ENTER));
    CHECK_EQ(0, ecsp_low_power_mode_valid(0x02u));
    CHECK_EQ(0, ecsp_low_power_mode_valid(0xFFu));
}

int main(void)
{
    test_equal_versions();
    test_differs_in_last_byte();
    test_no_nul_termination_assumption();
    test_embedded_nul_is_significant();
    test_version_parse_valid();
    test_version_parse_length_error();
    test_version_parse_value_error();
    test_serial_parse_valid();
    test_serial_parse_length_error();
    test_serial_parse_value_error();
    test_low_power_mode_valid();

    return ecsp_test_report();
}
