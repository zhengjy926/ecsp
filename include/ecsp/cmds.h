#ifndef ECSP_CMDS_H
#define ECSP_CMDS_H

/*
 * L2 common-command catalog and field parsers (docs/specs/ecsp-impl-spec.md
 * section 5).
 *
 * The catalog is the one place that states, for each common command, the
 * request length, the largest response data, the command class, whether the
 * payload starts with a transaction sequence, the silence ceiling, and the
 * long-running triple. The slave pipeline reads the class, the length and
 * the sequence flag. The host reads the silence ceiling and the triple, and
 * does not read the class. 0x0F is not a request. Event codes are listed and
 * are not requests.
 *
 * The parsers only validate and copy fields. They never look at session
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

/* Command class and the transaction-sequence flag. The same bits are stored
 * in the catalog and in a board command table. The host does not consult
 * them when choosing a silence ceiling. */
typedef enum {
    ECSP_CMD_NONE       = 0,
    ECSP_CMD_PREEMPTIVE = 1u << 0,  /* accepted while long-running; a stop
                                     * reaches its handler (spec 4.3.1, 4.4) */
    ECSP_CMD_IDEMPOTENT = 1u << 1,  /* idempotent action: carries no transaction seq */
    ECSP_CMD_TXN_SEQ    = 1u << 2,  /* non-idempotent action: payload[0] is the
                                     * transaction sequence, and the answer is
                                     * cached by (cmd, seq) and replayed (4.2) */
    ECSP_CMD_QUERY      = 1u << 3,  /* getter: accepted while long-running,
                                     * does not preempt (spec 4.3) */
    ECSP_CMD_CONCURRENT = 1u << 4   /* orthogonal action: accepted while
                                     * long-running, does not preempt
                                     * (spec 4.3.1) */
} ecsp_cmd_flags_t;

int ecsp_catalog_request(uint8_t cmd, uint8_t *flags, uint16_t *req_len,
                         uint16_t *resp_max);
int ecsp_catalog_resp_ms(uint8_t cmd, uint32_t *resp_ms);
int ecsp_catalog_long_run(uint8_t action, uint8_t *event_cmd,
                          uint8_t *query_cmd);
uint16_t ecsp_catalog_max_resp_data(void);

/* Response codes (spec 3.6), the subset this slice emits. */
#define ECSP_RS_OK             0x00u
#define ECSP_RS_UNSPECIFIED    0x01u /* commit failed, or any other uncategorised failure */
#define ECSP_RS_BAD_LEN        0x02u
#define ECSP_RS_BAD_PARAM      0x03u
#define ECSP_RS_UNSUPPORTED    0x04u
#define ECSP_RS_NOT_HANDSHAKEN 0x10u
#define ECSP_RS_BUSY           0x11u /* operation busy (spec 3.6) */
#define ECSP_RS_INVALID        0x13u /* precondition not met (spec 3.6, 5.2.9) */
#define ECSP_RS_FAILED         0x20u /* execution failed (spec 3.6, 4.4) */
#define ECSP_RS_ABORTED        0x21u /* host stopped a long-running command */

/* Long-running control byte (spec 5.2.8, 5.2.12): payload is
 * transaction sequence + this control. */
#define ECSP_CTRL_START 0x00u
#define ECSP_CTRL_STOP  0x01u
#define ECSP_CTRL_LEN   1u

/* Fault-word bit0 (spec table 5-3). The common layer has no other bit:
 * a failed self-test sets it, a successful self-test clears it, and an
 * in-flight long-running command that cannot be brought to a safe state
 * sets it too (spec 4.4). bit5..bit7 stay untouched. */
#define ECSP_FAULT_SELFTEST 0x01u

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
