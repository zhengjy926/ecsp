#ifndef ECSP_CMDS_H
#define ECSP_CMDS_H

/*
 * L2 common-command codec (docs/specs/ecsp-impl-spec.md section 5).
 *
 * Ticket #4 scope: the software full version (spec 5.2.2, 9.2) and the two
 * command/response codes this slice implements. The remaining 0x03~0x12 codecs
 * land with the pipeline in later tickets.
 */

#include <stdint.h>

/* Software full version "X.Y.Z.N": a fixed 7-byte ASCII string (spec 5.2.2).
 * It is NOT NUL-terminated and is never interpreted numerically. */
#define ECSP_VERSION_LEN 7u

typedef struct {
    uint8_t b[ECSP_VERSION_LEN];
} ecsp_version_t;

/* Common command codes (spec 5.1.1), the subset this slice implements. */
#define ECSP_CMD_HANDSHAKE    0x01u
#define ECSP_CMD_GET_VERSION  0x02u

/* Response codes (spec 3.6), the subset this slice emits. */
#define ECSP_RS_OK             0x00u
#define ECSP_RS_BAD_LEN        0x02u
#define ECSP_RS_NOT_HANDSHAKEN 0x10u

/* Byte-wise equality over all 7 version bytes (spec 9.2: a fixed 7-byte
 * memcmp, no numeric parsing, no NUL-termination assumption). Returns 1 when
 * equal, 0 otherwise. */
int ecsp_version_equals(const ecsp_version_t *a, const ecsp_version_t *b);

#endif /* ECSP_CMDS_H */
