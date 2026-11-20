#ifndef CH592_LINK_USB_H
#define CH592_LINK_USB_H

#include <stdint.h>

void link_usb_init(void);
void link_usb_poll(void);
uint8_t link_usb_send(const uint8_t *data, uint8_t len);

#endif
