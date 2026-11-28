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
     * PA12/PA13: move PWM4/PWM5 off these pads; keep SPI0 on default PA12..PA15
     * PB4: move UART0/PWM7 away from nRESET
     * PA4/PA5/PA13/PA14/PA15: ensure LCD/analog input disable are cleared
     */
    R16_PIN_ALTERNATE |= (RB_PIN_UART0 | RB_PIN_UART3 | RB_PIN_PWMX);
    R16_PIN_ALTERNATE &= (uint16_t)~(RB_RF_ANT_SW_EN | RB_PIN_SPI0);

    R16_PIN_ANALOG_IE &= (uint16_t)~(bAIN0 | bAIN1);
#if CONFIG_SWD_HW_SPI
    R32_PIN_CONFIG2 &= ~(LINK_SWD_CLK_PIN | LINK_SWD_DIO_PIN | LINK_SWD_SPI_MOSI_PIN | RB_PIN_PB4_DIS);
#else
    R32_PIN_CONFIG2 &= ~(LINK_SWD_CLK_PIN | LINK_SWD_DIO_PIN | RB_PIN_PB4_DIS);
#endif
    R32_LCD_CMD &= ~(RB_LCD_SYS_EN | RB_LCD_ON |
                     RB_LCD_SEG0_7_EN | RB_LCD_SEG8_15_EN | RB_LCD_SEG16_19_EN);
}

#if CONFIG_SWD_HW_SPI
#if CONFIG_SWD_HW_SPI_2WIRE
#define LINK_SWD_SPI_TX_OE  RB_SPI_MISO_OE
#define LINK_SWD_SPI_TX_PIN LINK_SWD_DIO_PIN
#else
#define LINK_SWD_SPI_TX_OE  RB_SPI_MOSI_OE
#define LINK_SWD_SPI_TX_PIN LINK_SWD_SPI_MOSI_PIN
#endif

static inline __attribute__((always_inline)) void link_board_swd_spi_tx_pin_enable(void)
{
#if CONFIG_SWD_HW_SPI_2WIRE
    link_board_swd_dio_config_output();
#else
    /* Ensure PA15(MISO) stays floating input while PA14(MOSI) drives TX. */
    R32_PA_PU &= ~(LINK_SWD_DIO_PIN | LINK_SWD_SPI_MOSI_PIN);
    R32_PA_PD_DRV &= ~(LINK_SWD_DIO_PIN | LINK_SWD_SPI_MOSI_PIN);
    R32_PA_DIR = (R32_PA_DIR & ~LINK_SWD_DIO_PIN) | LINK_SWD_SPI_MOSI_PIN;
    link_board_swd_dio_mode = 2u;
#endif
}

static inline __attribute__((always_inline)) void link_board_swd_spi_rx_pin_enable(void)
{
#if CONFIG_SWD_HW_SPI_2WIRE
    if (link_board_swd_dio_mode != 0u) {
        R32_PA_PU &= ~LINK_SWD_DIO_PIN;
        R32_PA_PD_DRV &= ~LINK_SWD_DIO_PIN;
        R32_PA_DIR &= ~LINK_SWD_DIO_PIN;
        link_board_swd_dio_mode = 0u;
    }
#else
    /* Release BOTH PA14(MOSI) and PA15(MISO) to floating input before target drives SWDIO. */
    R32_PA_PU &= ~(LINK_SWD_DIO_PIN | LINK_SWD_SPI_MOSI_PIN);
    R32_PA_PD_DRV &= ~(LINK_SWD_DIO_PIN | LINK_SWD_SPI_MOSI_PIN);
    R32_PA_DIR &= ~(LINK_SWD_DIO_PIN | LINK_SWD_SPI_MOSI_PIN);
    link_board_swd_dio_mode = 0u;
#endif
}

void link_board_swd_spi_set_clock(uint32_t clock_hz)
{
    uint32_t div;

    if (clock_hz == 0u) {
        div = 240u; /* 250 kHz */
    } else {
        div = (FREQ_SYS + clock_hz - 1u) / clock_hz;
        if (div < 6u) {
            div = 6u; /* 10 MHz cap at 60 MHz */
        }
        if (div > 254u) {
            div = 254u; /* ~236 kHz floor */
        }
        if (div & 1u) {
            div++;
        }
    }
    R8_SPI0_CLOCK_DIV = (uint8_t)div;
}

LINK_SWD_RAM_CODE void link_board_swd_spi_write_byte(uint8_t value)
{
    link_board_swd_spi_tx_pin_enable();
    R8_SPI0_CTRL_MOD = RB_SPI_ALL_CLEAR;
    R8_SPI0_CTRL_MOD = LINK_SWD_SPI_TX_OE | RB_SPI_SCK_OE;
    R8_SPI0_BUFFER = value;
    while (!(R8_SPI0_INT_FLAG & RB_SPI_FREE)) {}
    R8_SPI0_CTRL_MOD = 0;
}

void link_board_swd_spi_write_bytes(const uint8_t *data, uint16_t len)
{
    if ((data == 0) || (len == 0u)) {
        return;
    }
    link_board_swd_spi_tx_pin_enable();
    R8_SPI0_CTRL_MOD = RB_SPI_ALL_CLEAR;
    R8_SPI0_CTRL_MOD = LINK_SWD_SPI_TX_OE | RB_SPI_SCK_OE;
    R16_SPI0_TOTAL_CNT = len;
    R8_SPI0_INT_FLAG = RB_SPI_IF_CNT_END;
    while (len) {
        if (R8_SPI0_FIFO_COUNT < SPI_FIFO_SIZE) {
            R8_SPI0_FIFO = *data++;
            len--;
        }
    }
    while (!(R8_SPI0_INT_FLAG & RB_SPI_IF_CNT_END)) {}
    while (!(R8_SPI0_INT_FLAG & RB_SPI_FREE)) {}
    R8_SPI0_CTRL_MOD = 0;
}

void link_board_swd_spi_write_zeros(uint8_t byte_count)
{
    if (byte_count == 0u) {
        return;
    }
    link_board_swd_spi_tx_pin_enable();
    R8_SPI0_CTRL_MOD = RB_SPI_ALL_CLEAR;
    R8_SPI0_CTRL_MOD = LINK_SWD_SPI_TX_OE | RB_SPI_SCK_OE;
    R16_SPI0_TOTAL_CNT = byte_count;
    R8_SPI0_INT_FLAG = RB_SPI_IF_CNT_END;
    while (byte_count) {
        if (R8_SPI0_FIFO_COUNT < SPI_FIFO_SIZE) {
            R8_SPI0_FIFO = 0x00u;
            byte_count--;
        }
    }
    while (!(R8_SPI0_INT_FLAG & RB_SPI_IF_CNT_END)) {}
    while (!(R8_SPI0_INT_FLAG & RB_SPI_FREE)) {}
    R8_SPI0_CTRL_MOD = 0;
}

LINK_SWD_RAM_CODE void link_board_swd_spi_write_u32(uint32_t value)
{
    link_board_swd_spi_tx_pin_enable();
    R8_SPI0_CTRL_MOD = RB_SPI_ALL_CLEAR;
    R8_SPI0_CTRL_MOD = LINK_SWD_SPI_TX_OE | RB_SPI_SCK_OE;
    R16_SPI0_TOTAL_CNT = 4u;
    R8_SPI0_INT_FLAG = RB_SPI_IF_CNT_END;
    R8_SPI0_FIFO = (uint8_t)value;
    R8_SPI0_FIFO = (uint8_t)(value >> 8);
    R8_SPI0_FIFO = (uint8_t)(value >> 16);
    R8_SPI0_FIFO = (uint8_t)(value >> 24);
    while (!(R8_SPI0_INT_FLAG & RB_SPI_IF_CNT_END)) {}
    while (!(R8_SPI0_INT_FLAG & RB_SPI_FREE)) {}
    R8_SPI0_CTRL_MOD = 0;
}

LINK_SWD_RAM_CODE uint32_t link_board_swd_spi_read_u32(void)
{
    uint32_t b0, b1, b2, b3;
    uint32_t raw;

    link_board_swd_spi_rx_pin_enable();
    R8_SPI0_CTRL_MOD = RB_SPI_ALL_CLEAR;
    R8_SPI0_CTRL_MOD = RB_SPI_SCK_OE | RB_SPI_FIFO_DIR;
    R16_SPI0_TOTAL_CNT = 4u;
    R8_SPI0_INT_FLAG = RB_SPI_IF_CNT_END;
    while (R8_SPI0_FIFO_COUNT < 4u) {}
    b0 = R8_SPI0_FIFO;
    b1 = R8_SPI0_FIFO;
    b2 = R8_SPI0_FIFO;
    b3 = R8_SPI0_FIFO;
    while (!(R8_SPI0_INT_FLAG & RB_SPI_FREE)) {}
    R8_SPI0_CTRL_MOD = 0;
    raw = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
#if CONFIG_SWD_SPI_RX_SHIFT1
    {
        uint32_t bit31 = (R32_PA_PIN & LINK_SWD_DIO_PIN) ? 0x80000000u : 0u;
        return (raw >> 1) | bit31;
    }
#else
    return raw;
#endif
}
#endif

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

    GPIOB_SetBits(LINK_TARGET_NRST_PIN);
    GPIOB_ModeCfg(LINK_TARGET_NRST_PIN, GPIO_ModeOut_PP_5mA);

#if CONFIG_SWD_HW_SPI
    /* Latch SCK (PA13) LOW to match SPI Mode 0 idle state before enabling OE. */
    GPIOA_ResetBits(LINK_SWD_CLK_PIN);
    GPIOA_ModeCfg(LINK_SWD_CLK_PIN, GPIO_ModeOut_PP_5mA);
#if LINK_USB_USE_CHERRYUSB
    link_board_swd_dio_mode = 2u;
#endif
    link_board_swd_spi_rx_pin_enable();

    sys_safe_access_enable();
    R8_SLP_CLK_OFF1 &= (uint8_t)~RB_SLP_CLK_SPI0;
    sys_safe_access_disable();

    R8_SPI0_CTRL_MOD = RB_SPI_ALL_CLEAR;
    R8_SPI0_CTRL_MOD = 0; /* Master Mode 0, all OE disabled (GPIO owns pins while idle) */
    R8_SPI0_CTRL_CFG = (uint8_t)((R8_SPI0_CTRL_CFG & ~(RB_SPI_DMA_ENABLE | RB_SPI_MST_DLY_EN)) |
                                 RB_SPI_AUTO_IF | RB_SPI_BIT_ORDER);
    link_board_swd_spi_set_clock(250000u);
#else
    /* Keep the external target SWD bus on regular GPIOs. */
    GPIOA_SetBits(LINK_SWD_CLK_PIN | LINK_SWD_DIO_PIN);
    GPIOA_ModeCfg(LINK_SWD_CLK_PIN, GPIO_ModeOut_PP_5mA);

#if LINK_USB_USE_CHERRYUSB
    link_board_swd_dio_mode = 2u;
#endif
    /* Standard SWD actively drives both high and low, then releases for reads. */
    R32_PA_OUT |= LINK_SWD_DIO_PIN;
    link_board_swd_dio_config_floating_input();
#endif
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

#if CONFIG_SWD_HW_SPI
#define LINK_SWD_SOFT_RAM_CODE
#else
#define LINK_SWD_SOFT_RAM_CODE LINK_SWD_RAM_CODE
#endif

LINK_SWD_SOFT_RAM_CODE void link_board_swd_write_bits_fast(uint32_t value, uint8_t bit_count, uint16_t half_us)
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

LINK_SWD_SOFT_RAM_CODE void link_board_swd_idle_low_fast(uint8_t cycles, uint16_t half_us)
{
    if (!cycles) return;
    link_board_swd_dio_drive_low();
    do {
        swd_word_clock(half_us);
    } while (--cycles);
}

LINK_SWD_SOFT_RAM_CODE uint32_t link_board_swd_read_word_early(uint16_t half_us)
{
    uint32_t value = 0;
    unsigned remaining;
    for (remaining = 32; remaining != 0; --remaining) {
        uint32_t sample;
        link_swd_half_delay(half_us);
        R32_PA_OUT |= LINK_SWD_CLK_PIN;
        /* The target updates SWDIO on the rising edge. Keep a small settling
         * window in place of the old GPIO wrapper call/return instructions. */
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory");
        sample = R32_PA_PIN;
        link_swd_half_delay(half_us);
        R32_PA_CLR = LINK_SWD_CLK_PIN;
        /* Samples arrive LSB first. A fixed shift avoids tracking bit position. */
        value = (value >> 1) | ((sample & LINK_SWD_DIO_PIN) ? 0x80000000u : 0u);
    }
    return value;
}

static inline __attribute__((always_inline)) uint32_t swd_parity32(uint32_t x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    return (0x6996u >> (x & 0xFu)) & 1u;
}

LINK_SWD_RAM_CODE static __attribute__((noinline)) void swd_fast_write_bits(uint32_t value, uint8_t bits)
{
    uint32_t base = R32_PA_OUT & ~(LINK_SWD_CLK_PIN | LINK_SWD_DIO_PIN);
    uint32_t d = base | ((value & 1u) ? LINK_SWD_DIO_PIN : 0u);
    R32_PA_OUT = d;
    link_board_swd_dio_config_output();
    for (;;) {
        __asm__ volatile ("nop\n\tnop" ::: "memory");
        R32_PA_OUT = d | LINK_SWD_CLK_PIN;
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop" ::: "memory");
        R32_PA_OUT = d;
        if (--bits == 0u) break;
        value >>= 1;
        d = base | ((value & 1u) ? LINK_SWD_DIO_PIN : 0u);
        R32_PA_OUT = d;
    }
}

LINK_SWD_RAM_CODE static __attribute__((noinline)) void swd_fast_clock_only(uint8_t cycles)
{
    uint32_t base = R32_PA_OUT & ~LINK_SWD_CLK_PIN;
    while (cycles--) {
        __asm__ volatile ("nop\n\tnop" ::: "memory");
        R32_PA_OUT = base | LINK_SWD_CLK_PIN;
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop" ::: "memory");
        R32_PA_OUT = base;
    }
}

LINK_SWD_RAM_CODE static __attribute__((noinline)) uint32_t swd_fast_read_bits_lsb(uint8_t bits)
{
    uint32_t base = R32_PA_OUT & ~LINK_SWD_CLK_PIN;
    uint32_t val = 0;
    uint8_t rem = bits;
    while (rem--) {
        uint32_t sample;
        __asm__ volatile ("nop\n\tnop\n\tnop" ::: "memory");
        R32_PA_OUT = base | LINK_SWD_CLK_PIN;
        __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory");
        sample = R32_PA_PIN;
        __asm__ volatile ("nop\n\tnop" ::: "memory");
        R32_PA_OUT = base;
        val = (val >> 1) | ((sample & LINK_SWD_DIO_PIN) ? 0x80000000u : 0u);
    }
    return val >> (32u - bits);
}

LINK_SWD_RAM_CODE uint8_t link_board_swd_fast_ap_write(uint8_t req, uint32_t value, uint8_t trn, uint8_t wtrn)
{
    uint8_t ack;
#if CONFIG_SWD_HW_SPI
    link_board_swd_spi_write_byte(req);
    link_board_swd_spi_rx_pin_enable();
    if (trn) swd_fast_clock_only(trn);
    ack = (uint8_t)swd_fast_read_bits_lsb(3u);
    if (ack != 1u) {
        swd_fast_clock_only(wtrn ? wtrn : 1u);
        link_board_swd_spi_write_byte(0x00u);
        link_board_swd_spi_rx_pin_enable();
        return ack;
    }
    if (wtrn) swd_fast_clock_only(wtrn);
    link_board_swd_spi_write_u32(value);
    /* Bit 0 = parity, bit 1 = 1 idle low clock before next START bit. */
    swd_fast_write_bits(swd_parity32(value), 2u);
    return 1u;
#else
    swd_fast_write_bits(req, 8u);
    link_board_swd_dio_config_floating_input();
    if (trn) swd_fast_clock_only(trn);
    ack = (uint8_t)swd_fast_read_bits_lsb(3u);
    if (ack != 1u) {
        swd_fast_clock_only(wtrn ? wtrn : 1u);
        swd_fast_write_bits(0u, 8u);
        link_board_swd_dio_config_floating_input();
        return ack;
    }
    if (wtrn) swd_fast_clock_only(wtrn);
    swd_fast_write_bits(value, 32u);
    /* Bit 0 = parity, bit 1 = 1 idle low clock before next START bit. */
    swd_fast_write_bits(swd_parity32(value), 2u);
    return 1u;
#endif
}

LINK_SWD_RAM_CODE uint8_t link_board_swd_fast_ap_read(uint8_t req, uint32_t *out, uint8_t trn)
{
    uint8_t ack;
    uint32_t val;
    uint32_t par;
    uint8_t turn_to_host = trn ? trn : 2u;
#if CONFIG_SWD_HW_SPI
    link_board_swd_spi_write_byte(req);
    link_board_swd_spi_rx_pin_enable();
    if (trn) swd_fast_clock_only(trn);
    ack = (uint8_t)swd_fast_read_bits_lsb(3u);
    if (ack != 1u) {
        swd_fast_clock_only(turn_to_host);
        link_board_swd_spi_write_byte(0x00u);
        link_board_swd_spi_rx_pin_enable();
        return ack;
    }
    val = link_board_swd_spi_read_u32();
    par = swd_fast_read_bits_lsb(1u);
    swd_fast_clock_only(turn_to_host);
    if (swd_parity32(val) != par) {
        link_board_swd_spi_write_byte(0x00u);
        link_board_swd_spi_rx_pin_enable();
        return 0xFEu;
    }
    /* 2 idle low clocks guarantee clean target release before next START bit. */
    swd_fast_write_bits(0u, 2u);
    *out = val;
    return 1u;
#else
    swd_fast_write_bits(req, 8u);
    link_board_swd_dio_config_floating_input();
    if (trn) swd_fast_clock_only(trn);
    ack = (uint8_t)swd_fast_read_bits_lsb(3u);
    if (ack != 1u) {
        swd_fast_clock_only(turn_to_host);
        swd_fast_write_bits(0u, 8u);
        link_board_swd_dio_config_floating_input();
        return ack;
    }
    val = swd_fast_read_bits_lsb(32u);
    par = swd_fast_read_bits_lsb(1u);
    swd_fast_clock_only(turn_to_host);
    if (swd_parity32(val) != par) {
        swd_fast_write_bits(0u, 8u);
        link_board_swd_dio_config_floating_input();
        return 0xFEu;
    }
    /* 2 idle low clocks guarantee clean target release before next START bit. */
    swd_fast_write_bits(0u, 2u);
    *out = val;
    return 1u;
#endif
}

#endif
