#ifndef CH592_LINK_CONFIG_H
#define CH592_LINK_CONFIG_H

/*
 * Keep the proven WCH USB backend as the default while CherryUSB is being
 * brought up. Set this to 1 only for dedicated CherryUSB enumeration tests.
 */
#ifndef LINK_USB_USE_CHERRYUSB
#define LINK_USB_USE_CHERRYUSB 0
#endif

#ifndef LINK_BLE_ENABLE
#define LINK_BLE_ENABLE 0
#endif

#ifndef LINK_BLE_SWD_WORD_IO
#define LINK_BLE_SWD_WORD_IO 0
#endif
#ifndef LINK_S9_FAST_CLOCK
#define LINK_S9_FAST_CLOCK 0
#endif
#ifndef LINK_BLE_TWO_WRITES
#define LINK_BLE_TWO_WRITES 0
#endif
#define LINK_BLE_WIDE_FRAME 139u
#ifndef LINK_USB_DRW_WORD_IO
#define LINK_USB_DRW_WORD_IO 0
#endif

#ifndef LINK_BLE_SRAM_WRITE
#define LINK_BLE_SRAM_WRITE 0
#endif

/* Disabled by default so Cortex-M3/M4 targets report real CPUID/ROM tables. */
#ifndef LINK_SWD_SYNTHETIC_M3_IDS
#define LINK_SWD_SYNTHETIC_M3_IDS 0
#endif

/* 1 = CH592 hardware SPI0 + GPIO hybrid SWD (PA13=SCK, PA15=DIO 2-wire or PA14+PA15 3-wire);
 * 0 = original pure software GPIO SWD on PA4(DIO)/PA5(SCK). */
#ifndef CONFIG_SWD_HW_SPI
#define CONFIG_SWD_HW_SPI 1
#endif

/* 1 = 2-wire half-duplex on PA13(SCK) + PA15(MISO/SDX), no resistor or PA14 needed;
 * 0 = 3-wire PA13(SCK) + PA14(MOSI) + PA15(MISO) tied to SWDIO (330-470 ohm on PA14). */
#ifndef CONFIG_SWD_HW_SPI_2WIRE
#define CONFIG_SWD_HW_SPI_2WIRE 1
#endif

/* Mode 0 rising-edge SPI0 RX captures ACK[2] on clock 12 and DATA[0..30] on clocks 13..43,
 * leaving DATA[31] stable on SWDIO right after clock 43 falling edge. */
#ifndef CONFIG_SWD_SPI_RX_SHIFT1
#define CONFIG_SWD_SPI_RX_SHIFT1 1
#endif

#endif
