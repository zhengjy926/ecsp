#ifndef UID_H
#define UID_H

#include <stdint.h>

/* Same width as F429_CHIP_ID_LEN in bringup.h (board command 0x30). */
#define UID_LEN 12u

/* Copy this die's 96-bit unique id in the chip's native byte order. */
void uid_read(uint8_t out[UID_LEN]);

#endif /* UID_H */
