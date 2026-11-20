#ifndef LINK_BLE_PROGRAMMER_H
#define LINK_BLE_PROGRAMMER_H

#include <stdint.h>

/* Main-loop only except disconnect(), which only posts cancellation. */
void link_ble_programmer_init(void);
uint8_t link_ble_programmer_start(const uint8_t *body, uint16_t len);
void link_ble_programmer_poll(void);
uint8_t link_ble_programmer_response(uint8_t *body, uint16_t *len);
void link_ble_programmer_disconnect(void);

/* TMOS monotonic clock supplied by the transport. */
uint32_t link_ble_now_ms(void);

#endif
