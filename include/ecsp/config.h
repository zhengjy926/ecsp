#ifndef ECSP_CONFIG_H
#define ECSP_CONFIG_H

/* Compile-time configuration and defaults (docs/specs/ecsp-impl-spec.md
 * section 3). Every macro may be overridden with -D. */

#include <stddef.h>
#include <stdint.h>

#ifndef ECSP_MAX_FRAME
#define ECSP_MAX_FRAME 128u
#endif

#ifndef ECSP_ENABLE_SLAVE
#define ECSP_ENABLE_SLAVE 1
#endif

#ifndef ECSP_ENABLE_HOST
#define ECSP_ENABLE_HOST 1
#endif

#ifndef ECSP_SLAVE_TXN_CACHE
#define ECSP_SLAVE_TXN_CACHE 4
#endif

#ifndef ECSP_HOST_SLAVES
#define ECSP_HOST_SLAVES 8
#endif

/* Local error codes (docs/specs/ecsp-impl-spec.md section 3): functions return
 * 0 (ECSP_OK) on success and a negative value on failure. These are library
 * failure reasons, never protocol response codes - those live only inside
 * frames (spec 3.6). Grown per ticket: only what this slice returns is defined
 * here; later tickets add codes as their functions need them. */
enum {
    ECSP_OK    = 0,
    ECSP_E_ARG = -1   /* invalid argument or configuration */
};

#endif /* ECSP_CONFIG_H */
