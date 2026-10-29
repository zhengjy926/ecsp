/*
 * L2 common-command codec: software version (ticket #4).
 *
 * The version is a fixed 7-byte string (spec 5.2.2); comparison is a byte-wise
 * equality over all 7 bytes (spec 9.2). It must not be parsed numerically and
 * must not be treated as a NUL-terminated C string, so versions that differ
 * only after an embedded 0x00 byte must compare unequal.
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

int main(void)
{
    test_equal_versions();
    test_differs_in_last_byte();
    test_no_nul_termination_assumption();
    test_embedded_nul_is_significant();

    return ecsp_test_report();
}
