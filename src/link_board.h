#ifndef CH592_LINK_BOARD_H
#define CH592_LINK_BOARD_H

#include "CH59x_common.h"
#include "link_config.h"

/* The official startup copies .highcode into RAM before main(). */
#if LINK_USB_USE_CHERRYUSB
#define LINK_SWD_RAM_CODE __HIGH_CODE
#else
#define LINK_SWD_RAM_CODE
#endif

/* Avoid CH592 download/debug pins PA8/PA9 and PB14/PB15 for the external SWD bus. */
#define LINK_SWD_CLK_PIN      GPIO_Pin_5
#define LINK_SWD_DIO_PIN      GPIO_Pin_4
#define LINK_TARGET_NRST_PIN  GPIO_Pin_4

void link_board_init(void);
void link_board_swd_clk_set(uint8_t high_level);
uint8_t link_board_swd_dio_get(void);
void link_board_swd_dio_drive_high(void);
void link_board_swd_dio_drive_low(void);
void link_board_swd_dio_force_high(void);
void link_board_swd_dio_release(void);
#if LINK_BLE_ENABLE && LINK_BLE_SWD_WORD_IO
/* Call with SWCLK low, after the protocol selects the data phase. */
void link_board_swd_write_word(uint32_t value, uint16_t half_us);
/* SWDIO is already released. Sample at the BLE early sample position. */
uint32_t link_board_swd_read_word_early(uint16_t half_us);
#endif
void link_board_target_nrst_set(uint8_t high_level);

#endif
