#ifndef CH592_LINK_CMSIS_DAP_H
#define CH592_LINK_CMSIS_DAP_H

#include <stdint.h>

#define CMSIS_DAP_PACKET_SIZE 64

void cmsis_dap_init(void);
uint8_t cmsis_dap_process_packet(const uint8_t *request, uint16_t request_len,
                                 uint8_t *response, uint16_t response_size);

typedef struct {
    uint8_t valid;
    uint8_t transfer_request;
    uint16_t retries;
    uint32_t read_data;
    uint32_t match_value;
    uint32_t match_mask;
} cmsis_dap_debug_diag_t;

void cmsis_dap_get_debug_diag(cmsis_dap_debug_diag_t *diag);

#endif
