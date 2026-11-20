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

#endif
