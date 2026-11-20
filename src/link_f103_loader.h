#ifndef LINK_F103_LOADER_H
#define LINK_F103_LOADER_H

#include "link_target.h"
#include <stdint.h>

#define LINK_F103_FLASH_ERROR ((link_target_status_t)0x82u)
#define LINK_F103_BAD_STATE   ((link_target_status_t)0x83u)

void link_f103_loader_reset(void);
/* Read-only admission check before ERASE, to avoid erasing a target on
 * which this session cannot execute the SRAM worker. */
void link_f103_loader_check_begin(uint32_t address);
/* Caller owns the halted target and keeps this validated WRITE buffer alive. */
void link_f103_loader_begin(uint32_t address, const uint8_t *data, uint16_t count);
/* Read-only CRC32 of a single aligned 1KiB page. */
void link_f103_loader_verify_begin(uint32_t address);
/* A CRC result is written only when a verify request returns OK. */
link_target_status_t link_f103_loader_poll(uint32_t *detail, uint32_t *crc);
/* Call after draining the prior SWD operation. Return 1 if recovery is needed. */
uint8_t link_f103_loader_stop(void);
link_target_status_t link_f103_loader_stop_poll(void);

#endif
