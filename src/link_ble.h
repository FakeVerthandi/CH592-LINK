#ifndef LINK_BLE_H
#define LINK_BLE_H
#include <stdint.h>
void link_ble_init(void);
void link_ble_poll(void);
uint8_t link_ble_usb_diag(uint8_t *out,uint8_t capacity);
uint8_t link_ble_usb_rx_diag(uint8_t *out,uint8_t capacity);
uint32_t link_ble_now_ms(void);
uint8_t link_ble_rx_frame_limit(void);
#endif
