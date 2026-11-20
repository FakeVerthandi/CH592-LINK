#include "CH59x_common.h"
#include "link_timing.h"

#if LINK_USB_PROFILE_TIMING

#define TIMING_CAPACITY 512u
#define TIMING_RECORD_SIZE 16u

typedef struct {
    uint8_t cmd;
    uint8_t hint;
    uint8_t status;
    uint8_t op;
    uint32_t start;
    uint32_t duration;
    uint32_t tar;
} timing_record_t;

static timing_record_t timing_records[TIMING_CAPACITY];
static uint16_t timing_count;
static uint32_t timing_dropped;
static uint32_t timing_epoch;
static uint32_t timing_hz;
static uint8_t timing_enabled;

uint32_t link_timing_now(void)
{
    /* CH592 SysTick CNT low word; modulo arithmetic covers one command. */
    return *(volatile uint32_t *)(uintptr_t)&SysTick->CNT;
}

void link_timing_init(void)
{
    /* The current USB-only main does not initialize BLE/TMOS or SysTick.
     * Use the official clock configuration without STIE or a PFIC IRQ. */
    if ((SysTick->CTLR & SysTick_CTLR_STE) == 0u) {
        SysTick->CMP = UINT64_MAX;
        SysTick->CNT = 0;
        SysTick->SR = 0;
        SysTick->CTLR = SysTick_CTLR_INIT | SysTick_CTLR_STRE |
                        SysTick_CTLR_STCLK | SysTick_CTLR_STE;
    }
    timing_hz = (SysTick->CTLR & SysTick_CTLR_STCLK) ? FREQ_SYS : FREQ_SYS / 8u;
    timing_enabled = 0;
    timing_count = 0;
    timing_dropped = 0;
}

void link_timing_record(const uint8_t *request, uint8_t request_len,
                        const uint8_t *response, uint8_t response_len,
                        uint32_t start_tick, uint32_t tar)
{
    uint32_t end = link_timing_now();
    timing_record_t *record;

    if (!timing_enabled) {
        return;
    }
    if (timing_count >= TIMING_CAPACITY) {
        timing_dropped++;
        return;
    }
    record = &timing_records[timing_count++];
    record->cmd = request[0];
    record->hint = (request_len > 1u) ? request[1] : 0;
    record->op = 0;
    record->status = (response_len > 1u) ? response[1] : 0xFFu;
    if ((request[0] == 0x05u) && (request_len >= 4u)) {
        record->hint = request[2];
        record->op = request[3];
        record->status = (response_len >= 3u) ? response[2] : 0xFFu;
    } else if ((request[0] == 0x06u) && (request_len >= 5u)) {
        record->hint = request[2];
        record->op = request[4];
        record->status = (response_len >= 4u) ? response[3] : 0xFFu;
    }
    record->start = start_tick - timing_epoch;
    record->duration = end - start_tick;
    record->tar = tar;
}

static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

uint8_t link_timing_read(const uint8_t *request, uint8_t request_len,
                         uint8_t *response, uint8_t response_size)
{
    uint8_t page = (request_len > 1u) ? request[1] : 0u;
    uint8_t pos = 4;
    uint8_t count = 0;
    uint16_t index;

    if (response_size < 4u) {
        return 0;
    }
    response[0] = 0x83;
    response[1] = 1;
    response[2] = page;
    response[3] = 0;
    if (page == 0xFFu) {
        timing_enabled = 0;
        timing_count = 0;
        timing_dropped = 0;
        timing_epoch = link_timing_now();
        timing_enabled = 1;
        return 4;
    }
    if (page == 0xFDu) {
        timing_enabled = 0;
        return 4;
    }
    if (page == 0xFEu) {
        if (response_size < 20u) {
            return 4;
        }
        response[3] = TIMING_RECORD_SIZE;
        put16(&response[4], TIMING_CAPACITY);
        put16(&response[6], timing_count);
        put32(&response[8], timing_dropped);
        put32(&response[12], timing_hz);
        put32(&response[16], link_timing_now() - timing_epoch);
        return 20;
    }
    index = (uint16_t)page * 3u;
    while ((index < timing_count) && (count < 3u) &&
           ((uint16_t)pos + TIMING_RECORD_SIZE <= response_size)) {
        const timing_record_t *record = &timing_records[index++];
        response[pos++] = record->cmd;
        response[pos++] = record->hint;
        response[pos++] = record->status;
        response[pos++] = record->op;
        put32(&response[pos], record->start);
        pos += 4;
        put32(&response[pos], record->duration);
        pos += 4;
        put32(&response[pos], record->tar);
        pos += 4;
        count++;
    }
    response[3] = count;
    return pos;
}
#endif
