#ifndef CH592_LINK_PROTOCOL_H
#define CH592_LINK_PROTOCOL_H

#include <stdint.h>

typedef enum {
    LINK_CMD_NOP = 0x00,
    LINK_CMD_CONNECT = 0x01,
    LINK_CMD_TARGET_RESET = 0x02,
    LINK_CMD_READ_ID = 0x03,
    LINK_CMD_READ_MEM = 0x04,
    LINK_CMD_WRITE_MEM = 0x05,
    LINK_CMD_FLASH_ERASE = 0x06,
    LINK_CMD_FLASH_WRITE = 0x07
} link_cmd_t;

void link_protocol_init(void);
void link_protocol_poll(void);
void link_protocol_on_rx(const uint8_t *data, uint16_t len);

#endif
