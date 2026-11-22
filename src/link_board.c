#include "CH59x_common.h"

#include "link_board.h"
#include "link_swd_delay.h"

#if LINK_USB_USE_CHERRYUSB
/* Only this module owns PA4 mode. Data bits reuse the selected direction. */
static uint8_t link_board_swd_dio_mode = 2u; /* 0=input, 1=output, 2=unknown */

LINK_SWD_RAM_CODE static void link_board_swd_dio_config_output(void)
{
    if (link_board_swd_dio_mode != 1u) {
        R32_PA_PU &= ~LINK_SWD_DIO_PIN;
        R32_PA_PD_DRV &= ~LINK_SWD_DIO_PIN;
        R32_PA_DIR |= LINK_SWD_DIO_PIN;
        link_board_swd_dio_mode = 1u;
    }
}
#endif

static void link_board_release_link_pins_to_gpio(void)
{
    /*
     * Keep shared peripherals away from the external SWD pins:
     * PA4/PA5: move UART3 off these pads
     * PB4: move UART0/PWM7 away from nRESET
     * PA4/PA5: make sure LCD segment driving is not active
     */
    R16_PIN_ALTERNATE |= (RB_PIN_UART0 | RB_PIN_UART3 | RB_PIN_PWMX);
    R16_PIN_ALTERNATE &= (uint16_t)~RB_RF_ANT_SW_EN;

    R16_PIN_ANALOG_IE &= (uint16_t)~(bAIN0 | bAIN1);
    R32_PIN_CONFIG2 &= ~(LINK_SWD_CLK_PIN | LINK_SWD_DIO_PIN | RB_PIN_PB4_DIS);
    R32_LCD_CMD &= ~(RB_LCD_SYS_EN | RB_LCD_ON |
                     RB_LCD_SEG0_7_EN | RB_LCD_SEG8_15_EN | RB_LCD_SEG16_19_EN);
}

LINK_SWD_RAM_CODE static void link_board_swd_dio_config_floating_input(void)
{
#if LINK_USB_USE_CHERRYUSB
    if (link_board_swd_dio_mode == 0u) {
        return;
    }
#endif
    R32_PA_PU &= ~LINK_SWD_DIO_PIN;
    R32_PA_PD_DRV &= ~LINK_SWD_DIO_PIN;
    R32_PA_DIR &= ~LINK_SWD_DIO_PIN;
#if LINK_USB_USE_CHERRYUSB
    link_board_swd_dio_mode = 0u;
#endif
}

void link_board_init(void)
{
    link_board_release_link_pins_to_gpio();

    /* Keep the external target SWD bus on regular GPIOs. */
    GPIOA_SetBits(LINK_SWD_CLK_PIN | LINK_SWD_DIO_PIN);
    GPIOB_SetBits(LINK_TARGET_NRST_PIN);
    GPIOA_ModeCfg(LINK_SWD_CLK_PIN, GPIO_ModeOut_PP_5mA);
    GPIOB_ModeCfg(LINK_TARGET_NRST_PIN, GPIO_ModeOut_PP_5mA);

#if LINK_USB_USE_CHERRYUSB
    link_board_swd_dio_mode = 2u;
#endif
    /* Standard SWD actively drives both high and low, then releases for reads. */
    R32_PA_OUT |= LINK_SWD_DIO_PIN;
    link_board_swd_dio_config_floating_input();
}

LINK_SWD_RAM_CODE void link_board_swd_clk_set(uint8_t high_level)
{
    if (high_level) {
        GPIOA_SetBits(LINK_SWD_CLK_PIN);
    } else {
        GPIOA_ResetBits(LINK_SWD_CLK_PIN);
    }
}

LINK_SWD_RAM_CODE uint8_t link_board_swd_dio_get(void)
{
    return GPIOA_ReadPortPin(LINK_SWD_DIO_PIN) ? 1 : 0;
}

LINK_SWD_RAM_CODE void link_board_swd_dio_drive_high(void)
{
    R32_PA_OUT |= LINK_SWD_DIO_PIN;
#if LINK_USB_USE_CHERRYUSB
    link_board_swd_dio_config_output();
#else
    R32_PA_PU &= ~LINK_SWD_DIO_PIN;
    R32_PA_PD_DRV &= ~LINK_SWD_DIO_PIN;
    R32_PA_DIR |= LINK_SWD_DIO_PIN;
#endif
}

LINK_SWD_RAM_CODE void link_board_swd_dio_drive_low(void)
{
    R32_PA_CLR = LINK_SWD_DIO_PIN;
#if LINK_USB_USE_CHERRYUSB
    link_board_swd_dio_config_output();
#else
    R32_PA_PU &= ~LINK_SWD_DIO_PIN;
    R32_PA_PD_DRV &= ~LINK_SWD_DIO_PIN;
    R32_PA_DIR |= LINK_SWD_DIO_PIN;
#endif
}

LINK_SWD_RAM_CODE void link_board_swd_dio_force_high(void)
{
    link_board_swd_dio_drive_high();
}

LINK_SWD_RAM_CODE void link_board_swd_dio_release(void)
{
    /* Do NOT set PA_OUT before switching to input.
     * On CH592, PA_OUT=1 in input mode enables the internal pull-up,
     * which would overpower the target's ACK drive. */
    link_board_swd_dio_config_floating_input();
}

void link_board_target_nrst_set(uint8_t high_level)
{
    if (high_level) {
        GPIOB_SetBits(LINK_TARGET_NRST_PIN);
    } else {
        GPIOB_ResetBits(LINK_TARGET_NRST_PIN);
    }
}

#if LINK_BLE_ENABLE && LINK_BLE_SWD_WORD_IO
/* BLE 32-bit payload. Keep the GPIO and shift operations in the RISC-V loop. */
static inline __attribute__((always_inline)) void swd_word_clock(uint16_t half_us)
{
    link_swd_half_delay(half_us);
    R32_PA_OUT |= LINK_SWD_CLK_PIN;
    link_swd_half_delay(half_us);
    R32_PA_CLR = LINK_SWD_CLK_PIN;
}

LINK_SWD_RAM_CODE void link_board_swd_write_bits_fast(uint32_t value, uint8_t bit_count, uint16_t half_us)
{
    if (!bit_count) return;
    if (value & 1u) link_board_swd_dio_drive_high();
    else link_board_swd_dio_drive_low();
    for (;;) {
        swd_word_clock(half_us);
        if (--bit_count == 0u) break;
        value >>= 1;
        if (value & 1u) R32_PA_OUT |= LINK_SWD_DIO_PIN;
        else R32_PA_CLR = LINK_SWD_DIO_PIN;
    }
}

LINK_SWD_RAM_CODE void link_board_swd_idle_low_fast(uint8_t cycles, uint16_t half_us)
{
    if (!cycles) return;
    link_board_swd_dio_drive_low();
    do {
        swd_word_clock(half_us);
    } while (--cycles);
}

LINK_SWD_RAM_CODE uint32_t link_board_swd_read_word_early(uint16_t half_us)
{
    uint32_t value = 0;
    unsigned remaining;
    for (remaining = 32; remaining != 0; --remaining) {
        uint32_t sample;
        link_swd_half_delay(half_us);
        R32_PA_OUT |= LINK_SWD_CLK_PIN;
        /* The target updates SWDIO on the rising edge. Keep a small settling
         * window in place of the old GPIO wrapper call/return instructions. */
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop" ::: "memory");
        sample = R32_PA_PIN;
        link_swd_half_delay(half_us);
        R32_PA_CLR = LINK_SWD_CLK_PIN;
        /* Samples arrive LSB first. A fixed shift avoids tracking bit position. */
        value = (value >> 1) | ((sample & LINK_SWD_DIO_PIN) ? 0x80000000u : 0u);
    }
    return value;
}

#endif
