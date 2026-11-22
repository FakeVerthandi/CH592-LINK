#ifndef LINK_TARGET_DB_H
#define LINK_TARGET_DB_H

#include <stdint.h>

#define LINK_TARGET_FLASH_BASE      0x08000000u
#define LINK_TARGET_SRAM_BASE       0x20000000u
#define LINK_TARGET_VERIFY_CHUNK    1024u

typedef enum {
    LINK_FAMILY_NONE = 0,
    LINK_FAMILY_STM32F1 = 1,
    LINK_FAMILY_STM32F3 = 2,
    LINK_FAMILY_STM32F4 = 3
} link_target_family_t;

typedef enum {
    LINK_ERASE_UNIFORM_PAGE = 1,
    LINK_ERASE_STM32F4_SINGLE = 2
} link_erase_model_t;

typedef enum {
    LINK_DMA_MODEL_F1_1 = 1,    /* DMA1 (7 channels) */
    LINK_DMA_MODEL_F1_F3_2 = 2, /* DMA1 (7 channels) + DMA2 (5 channels) */
    LINK_DMA_MODEL_F4_2 = 3     /* DMA1 (8 streams) + DMA2 (8 streams) */
} link_dma_model_t;

typedef struct {
    uint16_t dev_id;
    uint8_t family;
    uint8_t erase_model;
    uint8_t dma_model;
    uint8_t reserved;
    uint16_t page_bytes;      /* 1024 or 2048 for uniform; 0 for F4 sectors */
    uint16_t min_flash_kb;
    uint16_t max_flash_kb;
    uint16_t sram_kb;
    uint32_t flash_size_reg;
} link_target_chip_desc_t;

typedef struct {
    const link_target_chip_desc_t *desc;
    uint32_t flash_base;
    uint32_t flash_bytes;
    uint16_t flash_kb;
    uint16_t sram_kb;
    uint8_t sector_count;
} link_target_active_t;

const link_target_chip_desc_t *link_target_db_find(uint16_t dev_id);
uint8_t link_target_db_init_active(const link_target_chip_desc_t *desc,
                                   uint16_t flash_kb,
                                   link_target_active_t *out);
uint8_t link_target_db_erase_unit(const link_target_active_t *target,
                                  uint32_t address,
                                  uint32_t *unit_bytes,
                                  uint8_t *sector_num);
uint8_t link_target_db_range_ok(const link_target_active_t *target,
                                uint32_t address,
                                uint32_t count);
uint8_t link_target_db_write_ok(const link_target_active_t *target,
                                uint32_t address,
                                uint16_t count);

#endif
