#ifndef LINK_TARGET_LOADER_H
#define LINK_TARGET_LOADER_H

#include "link_target.h"
#include "link_target_db.h"
#include <stdint.h>

#define LINK_LOADER_FLASH_ERROR ((link_target_status_t)0x82u)
#define LINK_LOADER_BAD_STATE   ((link_target_status_t)0x83u)

void link_target_loader_reset(void);
void link_target_loader_erase_begin(const link_target_active_t *target,
                                    uint32_t address,
                                    uint32_t unit_bytes,
                                    uint8_t sector_num);
void link_target_loader_write_begin(const link_target_active_t *target,
                                    uint32_t address,
                                    const uint8_t *data,
                                    uint16_t count);
void link_target_loader_verify_begin(const link_target_active_t *target,
                                     uint32_t address);
link_target_status_t link_target_loader_poll(uint32_t *detail, uint32_t *crc);
uint8_t link_target_loader_stop(void);
link_target_status_t link_target_loader_stop_poll(void);

#endif
