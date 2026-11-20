#ifndef CH592_LINK_TIMING_H
#define CH592_LINK_TIMING_H
#include <stdint.h>
void link_timing_init(void);
uint32_t link_timing_now(void);
void link_timing_record(const uint8_t *request, uint8_t request_len,
                        const uint8_t *response, uint8_t response_len,
                        uint32_t start_tick, uint32_t tar);
uint8_t link_timing_read(const uint8_t *request, uint8_t request_len,
                         uint8_t *response, uint8_t response_size);
#endif
