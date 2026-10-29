#ifndef ECSP_TEST_UTIL_H
#define ECSP_TEST_UTIL_H

/*
 * Minimal check macros shared by the unit-test binaries. Each binary is a
 * single translation unit and owns its own counters, so these are `static`.
 *
 * Keep assertions about external behavior only (parent ticket #1, Testing
 * Decisions): assert on frame bytes, response codes and the public seam, not on
 * library internals.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int ecsp_test_checks = 0;
static int ecsp_test_failures = 0;

static void ecsp_test_fail(const char *file, int line, const char *msg)
{
    ecsp_test_failures++;
    printf("FAIL %s:%d: %s\n", file, line, msg);
}

#define CHECK(cond) \
    do { \
        ecsp_test_checks++; \
        if (!(cond)) { \
            ecsp_test_fail(__FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(want, got) \
    do { \
        ecsp_test_checks++; \
        if ((long)(want) != (long)(got)) { \
            char ecsp_test_msg_[96]; \
            (void)snprintf(ecsp_test_msg_, sizeof ecsp_test_msg_, \
                           "%s == %s (want %ld, got %ld)", \
                           #got, #want, (long)(want), (long)(got)); \
            ecsp_test_fail(__FILE__, __LINE__, ecsp_test_msg_); \
        } \
    } while (0)

/* Compare two byte sequences: length first, then contents. */
#define CHECK_MEM(want, want_len, got, got_len) \
    do { \
        ecsp_test_checks++; \
        if ((size_t)(want_len) != (size_t)(got_len)) { \
            char ecsp_test_msg_[96]; \
            (void)snprintf(ecsp_test_msg_, sizeof ecsp_test_msg_, \
                           "byte length (want %lu, got %lu)", \
                           (unsigned long)(want_len), (unsigned long)(got_len)); \
            ecsp_test_fail(__FILE__, __LINE__, ecsp_test_msg_); \
        } else if (memcmp((want), (got), (size_t)(want_len)) != 0) { \
            ecsp_test_fail(__FILE__, __LINE__, "byte sequence mismatch"); \
        } \
    } while (0)

/* Print the tally; returns 1 when any check failed, else 0. */
static int ecsp_test_report(void)
{
    if (ecsp_test_failures != 0) {
        printf("%d of %d checks FAILED\n", ecsp_test_failures, ecsp_test_checks);
        return 1;
    }
    printf("OK: %d checks passed\n", ecsp_test_checks);
    return 0;
}

#endif /* ECSP_TEST_UTIL_H */
