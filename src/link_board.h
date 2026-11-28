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
#if CONFIG_SWD_HW_SPI
#define LINK_SWD_CLK_PIN       GPIO_Pin_13
#define LINK_SWD_DIO_PIN       GPIO_Pin_15
#define LINK_SWD_SPI_MOSI_PIN  GPIO_Pin_14
#else
#define LINK_SWD_CLK_PIN       GPIO_Pin_5
#define LINK_SWD_DIO_PIN       GPIO_Pin_4
#endif
#define LINK_TARGET_NRST_PIN   GPIO_Pin_4

void link_board_init(void);
void link_board_swd_clk_set(uint8_t high_level);
uint8_t link_board_swd_dio_get(void);
void link_board_swd_dio_drive_high(void);
void link_board_swd_dio_drive_low(void);
void link_board_swd_dio_force_high(void);
void link_board_swd_dio_release(void);
#if CONFIG_SWD_HW_SPI
void link_board_swd_spi_set_clock(uint32_t clock_hz);
void link_board_swd_spi_write_byte(uint8_t value);
void link_board_swd_spi_write_bytes(const uint8_t *data, uint16_t len);
void link_board_swd_spi_write_zeros(uint8_t byte_count);
void link_board_swd_spi_write_u32(uint32_t value);
uint32_t link_board_swd_spi_read_u32(void);
#endif
#if LINK_BLE_ENABLE && LINK_BLE_SWD_WORD_IO
/* Call with SWCLK low, after the protocol selects the data phase. */
void link_board_swd_write_bits_fast(uint32_t value, uint8_t bit_count, uint16_t half_us);
void link_board_swd_idle_low_fast(uint8_t cycles, uint16_t half_us);
static inline void link_board_swd_write_word(uint32_t value, uint16_t half_us)
{
    link_board_swd_write_bits_fast(value, 32u, half_us);
}
/* SWDIO is already released. Sample at the early sample position. */
uint32_t link_board_swd_read_word_early(uint16_t half_us);
/* Fast RAM-only 5MHz SWD AP/DP single-word burst primitives (returns ACK or 0xFE on parity error). */
uint8_t link_board_swd_fast_ap_write(uint8_t req, uint32_t value, uint8_t trn, uint8_t wtrn);
uint8_t link_board_swd_fast_ap_read(uint8_t req, uint32_t *out, uint8_t trn);
#endif
void link_board_target_nrst_set(uint8_t high_level);

#endif
