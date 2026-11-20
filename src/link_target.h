#ifndef CH592_LINK_TARGET_H
#define CH592_LINK_TARGET_H

#include <stdint.h>

typedef enum {
    LINK_TARGET_NONE = 0,
    LINK_TARGET_ARM_SWD,
    LINK_TARGET_WCH_RISCV
} link_target_type_t;

typedef enum {
    LINK_TARGET_OK = 0x00,
    LINK_TARGET_NOT_CONNECTED = 0x01,
    LINK_TARGET_NOT_IMPLEMENTED = 0x02,
    LINK_TARGET_IO_ERROR = 0x03,
    LINK_TARGET_WAIT = 0x04,
    LINK_TARGET_FAULT = 0x05,
    LINK_TARGET_PARITY = 0x06,
    LINK_TARGET_BAD_PARAM = 0x07
} link_target_status_t;

typedef struct {
    uint8_t raw_ack;
    uint8_t attempts;
    uint8_t dio_read_high;
    uint8_t dio_read_low;
    uint8_t dio_read_release;
    uint8_t strategy;
    uint8_t turnaround;
    uint8_t ack_window;
} link_target_diag_t;

void link_target_init(void);
void link_target_poll(void);
link_target_status_t link_target_connect(void);
link_target_status_t link_target_reset(void);
link_target_status_t link_target_read_id(uint32_t *idcode);
link_target_status_t link_target_read32(uint32_t addr, uint32_t *value);
link_target_status_t link_target_read32_block(uint32_t addr, uint32_t *values, uint8_t word_count);
link_target_status_t link_target_write32(uint32_t addr, uint32_t value);
link_target_status_t link_target_flash_erase_page(uint32_t addr);
link_target_status_t link_target_flash_write(uint32_t addr, const uint8_t *data, uint8_t len);
void link_target_get_diag(link_target_diag_t *diag);

#endif
