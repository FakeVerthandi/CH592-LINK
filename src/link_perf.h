#ifndef CH592_LINK_PERF_H
#define CH592_LINK_PERF_H

#include <stdint.h>
#include "link_config.h"

/* Counter-only instrumentation. No timer, GPIO, clock or interrupt writes. */
typedef struct {
    uint32_t dap_packets;
    uint32_t dp_read_attempts;
    uint32_t dp_write_attempts;
    uint32_t ap_read_attempts;
    uint32_t ap_write_attempts;
    uint32_t ack_wait;
    uint32_t ack_fault;
    uint32_t protocol_error;
    uint32_t write_phase_retries;
    uint32_t rdbuff_recoveries;
    uint32_t powerup_calls;
    uint32_t powerup_polls;
    uint32_t dp_power_req_calls;
    uint32_t dp_power_req_polls;
} link_perf_counters_t;

#if LINK_USB_USE_CHERRYUSB

extern link_perf_counters_t link_perf_counters;

#define LINK_PERF_INC(field) (++link_perf_counters.field)

/* Called after SYS_RecoverIrq, once per completed physical SWD attempt.
 * Synthetic AP/ROM reads never pass through these four wire-level hooks. */
static inline void link_perf_note_swd(uint8_t ap_ndp, uint8_t rnw,
                                      uint8_t ack, uint8_t protocol_error)
{
    if (ap_ndp) {
        if (rnw) {
            LINK_PERF_INC(ap_read_attempts);
        } else {
            LINK_PERF_INC(ap_write_attempts);
        }
    } else if (rnw) {
        LINK_PERF_INC(dp_read_attempts);
    } else {
        LINK_PERF_INC(dp_write_attempts);
    }
    if (ack == 0x02u) {
        LINK_PERF_INC(ack_wait);
    } else if (ack == 0x04u) {
        LINK_PERF_INC(ack_fault);
    }
    if (protocol_error || ((ack != 0x01u) && (ack != 0x02u) && (ack != 0x04u))) {
        LINK_PERF_INC(protocol_error);
    }
}

#else

#define LINK_PERF_INC(field) ((void)0)
#define link_perf_note_swd(ap_ndp, rnw, ack, protocol_error) ((void)0)

#endif
#endif
