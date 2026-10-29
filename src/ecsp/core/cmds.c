#include "ecsp/cmds.h"

#include <string.h>

int ecsp_version_equals(const ecsp_version_t *a, const ecsp_version_t *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (memcmp(a->b, b->b, ECSP_VERSION_LEN) == 0) ? 1 : 0;
}
