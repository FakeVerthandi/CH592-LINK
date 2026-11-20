#ifndef CH592_LINK_TARGET_ARM_SWD_H
#define CH592_LINK_TARGET_ARM_SWD_H

#include <stdint.h>

#include "link_target.h"

void target_arm_swd_init(void);
#if defined(LINK_BLE_ENABLE) && LINK_BLE_ENABLE
link_target_status_t target_arm_swd_write16(uint32_t addr, uint16_t value);
link_target_status_t target_arm_swd_ble_attach(uint32_t *idcode);
void target_arm_swd_ble_attach_cancel(void);
typedef struct {
    uint8_t step;
    uint8_t status;
    uint16_t reserved;
    uint32_t idcode;
    uint32_t ctrl_stat;
    uint32_t csw;
} target_arm_swd_ble_attach_diag_t;
void target_arm_swd_ble_get_attach_diag(target_arm_swd_ble_attach_diag_t *diag);
/* Cooperative memory API: PENDING is scheduling progress, not a wire ACK.
 * Retry the same API/arguments until completion. Each call does <=1 raw SWD
 * transaction. An accepted DRW is never replayed while waiting for RDBUFF. */
#define LINK_SWD_PENDING ((link_target_status_t)0x80u)
#define LINK_SWD_TIMEOUT ((link_target_status_t)0x81u)
link_target_status_t target_arm_swd_ble_read32(uint32_t addr, uint32_t *value);
/* 1..256 bytes of F103 Flash. Buffer is provisional until OK, and must
 * remain valid until completion or mem_cancel(). */
link_target_status_t target_arm_swd_ble_read_flash(uint32_t addr, uint8_t *data, uint16_t count);
/* Word-aligned write into one private F103 loader SRAM region. The final
 * word is padded with FF. Keep data stable until completion or cancel. */
link_target_status_t target_arm_swd_ble_write_sram(uint32_t addr, const uint8_t *data, uint16_t count);
link_target_status_t target_arm_swd_ble_write32(uint32_t addr, uint32_t value);
link_target_status_t target_arm_swd_ble_write16(uint32_t addr, uint16_t value);
void target_arm_swd_ble_mem_reset(void);
void target_arm_swd_ble_mem_cancel(void);
link_target_status_t target_arm_swd_ble_mem_cancel_poll(void);
#endif
link_target_status_t target_arm_swd_connect(void);
void target_arm_swd_disconnect(void);
link_target_status_t target_arm_swd_reset(void);
link_target_status_t target_arm_swd_read_idcode(uint32_t *idcode);
link_target_status_t target_arm_swd_read32(uint32_t addr, uint32_t *value);
link_target_status_t target_arm_swd_read32_block(uint32_t addr, uint32_t *values, uint8_t word_count);
link_target_status_t target_arm_swd_write32(uint32_t addr, uint32_t value);
link_target_status_t target_arm_swd_flash_erase_page(uint32_t addr);
link_target_status_t target_arm_swd_flash_write(uint32_t addr, const uint8_t *data, uint8_t len);
void target_arm_swd_set_clock(uint32_t clock_hz);
void target_arm_swd_configure(uint8_t config);
void target_arm_swd_swj_sequence(uint16_t bit_count, const uint8_t *data);
void target_arm_swd_swd_sequence(uint8_t output, uint16_t bit_count, const uint8_t *out_data, uint8_t *in_data);
uint8_t target_arm_swd_swj_pins(uint8_t pin_out, uint8_t pin_select);
link_target_status_t target_arm_swd_dap_calibrate_write_phase(void);
link_target_status_t target_arm_swd_dap_transfer(uint8_t request, uint32_t write_data, uint32_t *read_data);
#if LINK_USB_USE_CHERRYUSB
/* NOT_IMPLEMENTED means unsupported class, with no physical transfer issued. */
link_target_status_t target_arm_swd_dap_write_block(uint8_t request, const uint8_t *data,
                                                   uint16_t word_count, uint16_t wait_retry,
                                                   uint16_t *done);
link_target_status_t target_arm_swd_dap_read_block(uint8_t request, uint8_t *data,
                                                  uint16_t word_count, uint16_t wait_retry,
                                                  uint16_t *done);
#endif
link_target_status_t target_arm_swd_dap_ap_read_post(uint8_t addr, uint32_t *posted_data);
link_target_status_t target_arm_swd_dap_read_rdbuff(uint32_t *read_data);
void target_arm_swd_get_diag(link_target_diag_t *diag);

typedef struct {
    uint8_t mem_stage;
    uint8_t ack_window;
    uint8_t turnaround;
    uint8_t write_turnaround;
    uint8_t dp_port;
    uint8_t dp_rnw;
    uint8_t dp_addr;
    uint8_t dp_status;
    uint8_t dp_ack;
    uint8_t dp_request;
    uint32_t mem_detail;
    uint32_t dp_select;
    uint32_t mem_ap_tar;
    uint32_t mem_ap_csw;
    uint32_t dp_data;
    uint8_t ap_post_addr;
    uint8_t ap_post_status;
    uint8_t ap_post_ack;
    uint8_t ap_post_request;
    uint32_t ap_post_data;
} target_arm_swd_debug_diag_t;

void target_arm_swd_get_debug_diag(target_arm_swd_debug_diag_t *diag);

#endif
