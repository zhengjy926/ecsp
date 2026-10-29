#ifndef ECSP_CMDS_H
#define ECSP_CMDS_H

/*
 * L2 common-command codec (docs/specs/ecsp-impl-spec.md section 5).
 *
 * Ticket #5 scope: the fixed-layout payloads of the passthrough commands -
 * software/hardware version (5.2.2, 5.2.3), serial number (5.2.5), low-power
 * mode (5.2.11) - plus the command and response codes this slice uses.
 *
 * These functions only validate and copy fields. They never look at session
 * state, transaction state or the node's identity storage; that is L3's job.
 * Length errors return ECSP_E_LEN and value/format errors return ECSP_E_VAL so
 * the two failure kinds stay distinguishable: L3's length gate answers 0x02,
 * its parameter gate answers 0x03.
 */

#include <stddef.h>
#include <stdint.h>

#include "ecsp/config.h"

/* Software/hardware full version "X.Y.Z.N": a fixed 7-byte ASCII string
 * (spec 5.2.2, 5.2.3). It is NOT NUL-terminated and is never interpreted
 * numerically. */
#define ECSP_VERSION_LEN 7u

/* Serial number "SN" + 10 ASCII digits (spec 5.2.5), fixed 12 bytes, not
 * NUL-terminated. */
#define ECSP_SERIAL_LEN 12u

/* Non-idempotent action transaction sequence number (spec 4.2). */
#define ECSP_TXN_SEQ_LEN 1u

typedef struct {
    uint8_t b[ECSP_VERSION_LEN];
} ecsp_version_t;

typedef struct {
    uint8_t b[ECSP_SERIAL_LEN];
} ecsp_serial_t;

/* Common command codes (spec 5.1.1). Event-class codes are listed for
 * completeness; the engine has no entry for them, so a host request using one
 * is answered 0x04 (undefined / wrong direction). */
#define ECSP_CMD_HANDSHAKE         0x01u
#define ECSP_CMD_GET_VERSION       0x02u
#define ECSP_CMD_SET_HW_VERSION    0x03u
#define ECSP_CMD_GET_HW_VERSION    0x04u
#define ECSP_CMD_SET_SERIAL        0x05u
#define ECSP_CMD_GET_SERIAL        0x06u
#define ECSP_CMD_SYSTEM_RESET      0x07u
#define ECSP_CMD_CONTROL_SELFTEST  0x08u
#define ECSP_CMD_GET_SELFTEST      0x09u
#define ECSP_CMD_SELFTEST_REPORT   0x0Au
#define ECSP_CMD_SET_LOW_POWER     0x0Bu
#define ECSP_CMD_START_UPGRADE     0x0Cu
#define ECSP_CMD_GET_UPGRADE       0x0Du
#define ECSP_CMD_UPGRADE_REPORT    0x0Eu
#define ECSP_CMD_CONTROL_UPLOAD    0x0Fu
#define ECSP_CMD_HEARTBEAT         0x10u
#define ECSP_CMD_FAULT_REPORT      0x11u
#define ECSP_CMD_GET_FAULT         0x12u

/* Command-code ranges (spec 3.5). 0x00 is reserved; 0x13~0x2F, 0xF0~0xFF are
 * unassigned and therefore answered 0x04. */
#define ECSP_CMD_COMMON_MIN 0x01u
#define ECSP_CMD_COMMON_MAX 0x2Fu
#define ECSP_CMD_BOARD_MIN  0x30u
#define ECSP_CMD_BOARD_MAX  0xEFu

/* Response codes (spec 3.6), the subset this slice emits. */
#define ECSP_RS_OK             0x00u
#define ECSP_RS_BAD_LEN        0x02u
#define ECSP_RS_BAD_PARAM      0x03u
#define ECSP_RS_UNSUPPORTED    0x04u
#define ECSP_RS_NOT_HANDSHAKEN 0x10u
#define ECSP_RS_BUSY           0x11u /* operation busy (spec 3.6) */

/* Low-power mode values (spec 5.2.11). */
#define ECSP_LOW_POWER_EXIT  0x00u
#define ECSP_LOW_POWER_ENTER 0x01u

/* Byte-wise equality over all 7 version bytes (spec 9.2: a fixed 7-byte
 * memcmp, no numeric parsing, no NUL-termination assumption). Returns 1 when
 * equal, 0 otherwise. */
int ecsp_version_equals(const ecsp_version_t *a, const ecsp_version_t *b);

/* Parse and validate a 7-byte "X.Y.Z.N" version (spec 5.2.2, 5.2.3).
 * Returns ECSP_OK on success, ECSP_E_ARG for a NULL output, ECSP_E_LEN when
 * `n` is not ECSP_VERSION_LEN, or ECSP_E_VAL when the shape is wrong (dots at
 * positions 1/3/5, ASCII digits at 0/2/4/6). On failure `*v` is untouched. */
int ecsp_version_parse(const uint8_t *p, size_t n, ecsp_version_t *v);

/* Parse and validate a 12-byte "SN" + 10 digits serial number (spec 5.2.5).
 * Returns ECSP_OK on success, ECSP_E_ARG for a NULL output, ECSP_E_LEN when
 * `n` is not ECSP_SERIAL_LEN, or ECSP_E_VAL when the prefix or a digit is
 * wrong. On failure `*sn` is untouched. */
int ecsp_serial_parse(const uint8_t *p, size_t n, ecsp_serial_t *sn);

/* 1 when `mode` is a defined low-power mode (spec 5.2.11), else 0. */
int ecsp_low_power_mode_valid(uint8_t mode);

#endif /* ECSP_CMDS_H */
