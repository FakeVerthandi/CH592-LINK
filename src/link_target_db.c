#include "link_target_db.h"

#define F1_FLASH_SIZE_REG 0x1FFFF7E0u
#define F3_FLASH_SIZE_REG 0x1FFFF7CCu
#define F4_FLASH_SIZE_REG 0x1FFF7A22u

static const link_target_chip_desc_t target_db[] = {
    /* STM32F1 single-bank families */
    {0x412u, LINK_FAMILY_STM32F1, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_1,    0u, 1024u,  16u,   32u,  10u, F1_FLASH_SIZE_REG},
    {0x410u, LINK_FAMILY_STM32F1, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_1,    0u, 1024u,  64u,  128u,  20u, F1_FLASH_SIZE_REG},
    {0x414u, LINK_FAMILY_STM32F1, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_F3_2, 0u, 2048u, 256u,  512u,  64u, F1_FLASH_SIZE_REG},
    {0x418u, LINK_FAMILY_STM32F1, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_F3_2, 0u, 2048u,  64u,  256u,  64u, F1_FLASH_SIZE_REG},

    /* STM32F3 families (uniform 2KiB pages) */
    {0x439u, LINK_FAMILY_STM32F3, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_1,    0u, 2048u,  16u,   64u,  16u, F3_FLASH_SIZE_REG},
    {0x438u, LINK_FAMILY_STM32F3, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_1,    0u, 2048u,  32u,   64u,  16u, F3_FLASH_SIZE_REG},
    {0x422u, LINK_FAMILY_STM32F3, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_F3_2, 0u, 2048u, 128u,  256u,  40u, F3_FLASH_SIZE_REG},
    {0x432u, LINK_FAMILY_STM32F3, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_F3_2, 0u, 2048u,  64u,  256u,  32u, F3_FLASH_SIZE_REG},
    {0x446u, LINK_FAMILY_STM32F3, LINK_ERASE_UNIFORM_PAGE,   LINK_DMA_MODEL_F1_F3_2, 0u, 2048u, 384u,  512u,  64u, F3_FLASH_SIZE_REG},

    /* STM32F4 single-bank families (16K*4 + 64K*1 + 128K*N) */
    {0x423u, LINK_FAMILY_STM32F4, LINK_ERASE_STM32F4_SINGLE, LINK_DMA_MODEL_F4_2,    0u,    0u, 128u,  256u,  64u, F4_FLASH_SIZE_REG},
    {0x433u, LINK_FAMILY_STM32F4, LINK_ERASE_STM32F4_SINGLE, LINK_DMA_MODEL_F4_2,    0u,    0u, 384u,  512u,  96u, F4_FLASH_SIZE_REG},
    {0x431u, LINK_FAMILY_STM32F4, LINK_ERASE_STM32F4_SINGLE, LINK_DMA_MODEL_F4_2,    0u,    0u, 256u,  512u, 128u, F4_FLASH_SIZE_REG},
    {0x413u, LINK_FAMILY_STM32F4, LINK_ERASE_STM32F4_SINGLE, LINK_DMA_MODEL_F4_2,    0u,    0u, 512u, 1024u, 128u, F4_FLASH_SIZE_REG},
    {0x421u, LINK_FAMILY_STM32F4, LINK_ERASE_STM32F4_SINGLE, LINK_DMA_MODEL_F4_2,    0u,    0u, 256u,  512u, 128u, F4_FLASH_SIZE_REG}
};

static uint32_t f4_sector_size(uint8_t sector)
{
    if (sector < 4u) return 16384u;
    if (sector == 4u) return 65536u;
    return 131072u;
}

const link_target_chip_desc_t *link_target_db_find(uint16_t dev_id)
{
    uint32_t i;
    for (i = 0; i < sizeof(target_db) / sizeof(target_db[0]); i++) {
        if (target_db[i].dev_id == dev_id) {
            return &target_db[i];
        }
    }
    return 0;
}

uint8_t link_target_db_init_active(const link_target_chip_desc_t *desc,
                                   uint16_t flash_kb,
                                   link_target_active_t *out)
{
    uint32_t bytes;
    uint8_t sectors = 0;

    if (!desc || !out) return 0;
    if (flash_kb < desc->min_flash_kb || flash_kb > desc->max_flash_kb) return 0;

    bytes = (uint32_t)flash_kb * 1024u;
    if (desc->erase_model == LINK_ERASE_UNIFORM_PAGE) {
        if (!desc->page_bytes || (bytes % desc->page_bytes) != 0u) return 0;
    } else if (desc->erase_model == LINK_ERASE_STM32F4_SINGLE) {
        uint32_t acc = 0;
        while (acc < bytes && sectors < 12u) {
            acc += f4_sector_size(sectors);
            sectors++;
        }
        if (acc != bytes) return 0;
    } else {
        return 0;
    }

    out->desc = desc;
    out->flash_base = LINK_TARGET_FLASH_BASE;
    out->flash_bytes = bytes;
    out->flash_kb = flash_kb;
    out->sram_kb = desc->sram_kb;
    out->sector_count = sectors;
    return 1;
}

uint8_t link_target_db_range_ok(const link_target_active_t *target,
                                uint32_t address,
                                uint32_t count)
{
    if (!target || !target->desc || !count) return 0;
    if (address < target->flash_base) return 0;
    if (address >= target->flash_base + target->flash_bytes) return 0;
    return count <= (target->flash_base + target->flash_bytes) - address;
}

uint8_t link_target_db_erase_unit(const link_target_active_t *target,
                                  uint32_t address,
                                  uint32_t *unit_bytes,
                                  uint8_t *sector_num)
{
    uint32_t offset;
    if (!target || !target->desc || !unit_bytes || !sector_num) return 0;
    if (address < target->flash_base || address >= target->flash_base + target->flash_bytes) return 0;
    offset = address - target->flash_base;

    if (target->desc->erase_model == LINK_ERASE_UNIFORM_PAGE) {
        uint32_t page = target->desc->page_bytes;
        if ((offset & (page - 1u)) != 0u || page > target->flash_bytes - offset) return 0;
        *unit_bytes = page;
        *sector_num = 0;
        return 1;
    }

    if (target->desc->erase_model == LINK_ERASE_STM32F4_SINGLE) {
        uint32_t acc = 0;
        uint8_t s;
        for (s = 0; s < target->sector_count; s++) {
            uint32_t sz = f4_sector_size(s);
            if (acc == offset) {
                *unit_bytes = sz;
                *sector_num = s;
                return 1;
            }
            if (acc > offset) return 0;
            acc += sz;
        }
    }
    return 0;
}

uint8_t link_target_db_write_ok(const link_target_active_t *target,
                                uint32_t address,
                                uint16_t count)
{
    if (!link_target_db_range_ok(target, address, count)) return 0;
    if (!count || count > 256u) return 0;
    if ((address & 1u) != 0u || (count & 1u) != 0u) return 0;
    if (((address & (LINK_TARGET_VERIFY_CHUNK - 1u)) + count) > LINK_TARGET_VERIFY_CHUNK) return 0;
    return 1;
}
