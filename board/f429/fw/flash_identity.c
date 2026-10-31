#include "flash_identity.h"

#include <stddef.h>
#include <string.h>

#include "stm32f4xx_hal.h"

/* Sector 11 on the 1 MB single-bank map (DB1M clear): 0x080E0000, 128 KB. */
#define IDENTITY_SECTOR      FLASH_SECTOR_11
#define IDENTITY_BASE        0x080E0000u
#define IDENTITY_SIZE        0x20000u
#define IDENTITY_RECORD_LEN  32u
#define IDENTITY_MAGIC       0x49534345u /* 'E','C','S','I' little-endian */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  hw[ECSP_VERSION_LEN];
    uint8_t  pad;
    uint8_t  serial[ECSP_SERIAL_LEN];
    uint32_t checksum;
} identity_record_t;

typedef union {
    identity_record_t r;
    uint32_t words[IDENTITY_RECORD_LEN / 4u];
    uint8_t  bytes[IDENTITY_RECORD_LEN];
} identity_record_u;

typedef char identity_record_size_ok[
    (sizeof(identity_record_t) == IDENTITY_RECORD_LEN) ? 1 : -1];

static uint32_t record_checksum(const identity_record_t *rec)
{
    const uint8_t *p = (const uint8_t *)rec;
    uint32_t sum = 0u;
    size_t i;

    for (i = 0u; i < offsetof(identity_record_t, checksum); i++) {
        sum += p[i];
    }
    return sum;
}

static int record_valid(const identity_record_t *rec)
{
    if (rec->magic != IDENTITY_MAGIC) {
        return 0;
    }
    if (rec->pad != 0u) {
        return 0;
    }
    return record_checksum(rec) == rec->checksum ? 1 : 0;
}

static int slot_erased(const identity_record_t *rec)
{
    const uint32_t *w = (const uint32_t *)rec;
    size_t i;

    for (i = 0u; i < (IDENTITY_RECORD_LEN / 4u); i++) {
        if (w[i] != 0xFFFFFFFFu) {
            return 0;
        }
    }
    return 1;
}

static uint32_t next_seq(uint32_t prev)
{
    return prev + 1u;
}

static int program_record(uint32_t addr, const identity_record_u *rec)
{
    size_t i;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return -1;
    }
    for (i = 0u; i < (IDENTITY_RECORD_LEN / 4u); i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              addr + (uint32_t)(i * 4u),
                              rec->words[i]) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return -1;
        }
    }
    if (HAL_FLASH_Lock() != HAL_OK) {
        return -1;
    }
    return 0;
}

static int erase_sector(void)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0u;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* 32-bit parallelism */
    erase.Sector = IDENTITY_SECTOR;
    erase.NbSectors = 1u;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return -1;
    }
    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
        (void)HAL_FLASH_Lock();
        return -1;
    }
    if (HAL_FLASH_Lock() != HAL_OK) {
        return -1;
    }
    return 0;
}

void flash_identity_load(ecsp_slave_identity_t *out,
                         const ecsp_slave_identity_t *blank)
{
    const identity_record_t *slot;
    const identity_record_t *last = NULL;
    uint32_t off;

    *out = *blank;

    /* Do not program option bytes; require the factory single-bank map. */
    if ((FLASH->OPTCR & FLASH_OPTCR_DB1M) != 0u) {
        return;
    }

    for (off = 0u; off + IDENTITY_RECORD_LEN <= IDENTITY_SIZE;
         off += IDENTITY_RECORD_LEN) {
        slot = (const identity_record_t *)(IDENTITY_BASE + off);
        if (slot_erased(slot) != 0) {
            break;
        }
        if (record_valid(slot) != 0) {
            last = slot;
        }
    }

    if (last != NULL) {
        memcpy(out->hardware_version.b, last->hw, ECSP_VERSION_LEN);
        memcpy(out->serial_number.b, last->serial, ECSP_SERIAL_LEN);
    }
}

int flash_identity_commit(const ecsp_slave_identity_t *next)
{
    identity_record_u rec;
    const identity_record_t *slot;
    uint32_t off;
    uint32_t free_addr = 0u;
    uint32_t last_seq = 0u;
    int have_seq = 0;
    int found_free = 0;

    if ((FLASH->OPTCR & FLASH_OPTCR_DB1M) != 0u) {
        return ECSP_E_STATE;
    }

    memset(&rec, 0, sizeof rec);
    for (off = 0u; off + IDENTITY_RECORD_LEN <= IDENTITY_SIZE;
         off += IDENTITY_RECORD_LEN) {
        slot = (const identity_record_t *)(IDENTITY_BASE + off);
        if (slot_erased(slot) != 0) {
            free_addr = IDENTITY_BASE + off;
            found_free = 1;
            break;
        }
        if (record_valid(slot) != 0) {
            last_seq = slot->seq;
            have_seq = 1;
        }
    }

    if (found_free == 0) {
        if (erase_sector() != 0) {
            return ECSP_E_LINK;
        }
        free_addr = IDENTITY_BASE;
    }

    rec.r.magic = IDENTITY_MAGIC;
    rec.r.seq = (have_seq != 0) ? next_seq(last_seq) : 1u;
    memcpy(rec.r.hw, next->hardware_version.b, ECSP_VERSION_LEN);
    rec.r.pad = 0u;
    memcpy(rec.r.serial, next->serial_number.b, ECSP_SERIAL_LEN);
    rec.r.checksum = record_checksum(&rec.r);

    if (program_record(free_addr, &rec) != 0) {
        return ECSP_E_LINK;
    }

    slot = (const identity_record_t *)free_addr;
    if (record_valid(slot) == 0) {
        return ECSP_E_LINK;
    }
    if (memcmp(slot->hw, next->hardware_version.b, ECSP_VERSION_LEN) != 0 ||
        memcmp(slot->serial, next->serial_number.b, ECSP_SERIAL_LEN) != 0) {
        return ECSP_E_LINK;
    }

    return ECSP_OK;
}
