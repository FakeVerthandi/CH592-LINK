#include <stdbool.h>
#include <string.h>

#include "CH59x_common.h"

#include "link_config.h"
#include "link_perf.h"

#if LINK_USB_USE_CHERRYUSB

#include "cmsis_dap.h"
#if LINK_BLE_ENABLE
#include "link_session.h"
#include "link_ble.h"
static volatile uint8_t cherry_pending;
static volatile uint8_t cherry_reset_pending;
static uint8_t cherry_work_buf[64];
static volatile uint8_t cherry_epoch;
static uint8_t cherry_work_epoch;
#endif
#if LINK_USB_PROFILE_TIMING
#include "link_timing.h"
#endif
#include "link_usb.h"
#include "target_arm_swd.h"
#include "usbd_core.h"

#define LINK_USB_CHERRY_BUSID 0
#define LINK_USB_CHERRY_EP_IN  0x82
#define LINK_USB_CHERRY_EP_OUT 0x02
#define LINK_USB_CHERRY_EP_MPS 64
#define LINK_USB_CHERRY_VENDOR_CODE 0x20
#define LINK_USB_CHERRY_CONFIG_SIZE (9 + 9 + 7 + 7)
#define LINK_USB_CHERRY_VID 0x1A86
#define LINK_USB_CHERRY_PID 0x5993
#define LINK_USB_CHERRY_TRACE_CMD 0x80
#define LINK_USB_CHERRY_DIAG_CMD 0x81
#define LINK_USB_CHERRY_PERF_CMD 0x82
#define LINK_USB_CHERRY_PERF_VERSION 1
#define LINK_USB_CHERRY_PERF_COUNT 14
#if LINK_BLE_ENABLE
/* BLE keeps two recent USB records plus the independent first-error record.
 * The USB-only 0.2.7 profile below retains its original 64-record history. */
#define LINK_USB_CHERRY_TRACE_COUNT 2
#else
#define LINK_USB_CHERRY_TRACE_COUNT 64
#endif
#define LINK_USB_CHERRY_TRACE_BYTES 28
#define LINK_USB_CHERRY_TRACE_RECORD_WIRE_SIZE (4 + LINK_USB_CHERRY_TRACE_BYTES + LINK_USB_CHERRY_TRACE_BYTES)

__attribute__((aligned(4))) static uint8_t cherry_rx_buf[LINK_USB_CHERRY_EP_MPS];
__attribute__((aligned(4))) static uint8_t cherry_tx_buf[LINK_USB_CHERRY_EP_MPS];
static volatile bool cherry_tx_busy;
link_perf_counters_t link_perf_counters;

typedef struct {
    uint8_t seq;
    uint8_t cmd;
    uint8_t req_len;
    uint8_t resp_len;
    uint8_t req[LINK_USB_CHERRY_TRACE_BYTES];
    uint8_t resp[LINK_USB_CHERRY_TRACE_BYTES];
} cherry_trace_record_t;

static cherry_trace_record_t cherry_first_error_trace;
static uint8_t cherry_trace_seq;
static cherry_trace_record_t cherry_trace[LINK_USB_CHERRY_TRACE_COUNT];
static uint8_t cherry_trace_next;

static bool cherry_trace_is_error_response(const uint8_t *request, uint8_t request_len,
                                           const uint8_t *response, uint8_t response_len)
{
    uint8_t cmd;

    if ((request == 0) || (response == 0) || (request_len == 0u) || (response_len == 0u)) {
        return false;
    }

    cmd = request[0];
    if ((response[0] == 0xFFu) || (response[0] != cmd)) {
        return true;
    }
    if ((cmd == 0x05u) && (response_len >= 3u)) {
        return response[2] != 0x01u;
    }
    if ((cmd == 0x06u) && (response_len >= 4u)) {
        return response[3] != 0x01u;
    }
    return false;
}

static void cherry_trace_copy_record(cherry_trace_record_t *record,
                                     const uint8_t *request, uint8_t request_len,
                                     const uint8_t *response, uint8_t response_len,
                                     uint8_t seq)
{
    uint8_t index;

    record->seq = seq;
    record->cmd = request[0];
    record->req_len = request_len;
    record->resp_len = response_len;

    for (index = 0; index < sizeof(record->req); index++) {
        record->req[index] = (index < request_len) ? request[index] : 0;
    }
    for (index = 0; index < sizeof(record->resp); index++) {
        record->resp[index] = (index < response_len) ? response[index] : 0;
    }
}

static void cherry_trace_log(const uint8_t *request, uint8_t request_len,
                             const uint8_t *response, uint8_t response_len)
{
    cherry_trace_record_t *record;

    if ((request == 0) || (request_len == 0) || (request[0] == LINK_USB_CHERRY_TRACE_CMD)) {
        return;
    }
    if (request[0] == 0x02u) {
        memset(&cherry_first_error_trace, 0, sizeof(cherry_first_error_trace));
    }

    record = &cherry_trace[cherry_trace_next];
    cherry_trace_seq++;
    if (cherry_trace_seq == 0u) {
        cherry_trace_seq = 1u;
    }
    cherry_trace_copy_record(record, request, request_len, response, response_len, cherry_trace_seq);
    if ((cherry_first_error_trace.seq == 0u) &&
        cherry_trace_is_error_response(request, request_len, response, response_len)) {
        cherry_trace_copy_record(&cherry_first_error_trace, request, request_len, response, response_len,
                                 record->seq);
    }

    cherry_trace_next++;
    if (cherry_trace_next >= LINK_USB_CHERRY_TRACE_COUNT) {
        cherry_trace_next = 0;
    }
}

static uint8_t cherry_trace_read(const uint8_t *request, uint8_t request_len,
                                 uint8_t *response, uint8_t response_size)
{
    uint8_t pos = 3;
    uint8_t count = 0;
    uint8_t offset;
    uint8_t page = 0;
    uint8_t skip;
    uint8_t records_per_page;

    if ((response == 0) || (response_size < 3u)) {
        return 0;
    }
    if (request_len >= 2u) {
        page = request[1];
    }
    if (page == 0xFEu) {
        response[0] = LINK_USB_CHERRY_TRACE_CMD;
        response[1] = cherry_trace_seq;
        response[2] = 0;
        if (cherry_first_error_trace.seq != 0u &&
            (uint8_t)(pos + LINK_USB_CHERRY_TRACE_RECORD_WIRE_SIZE) <= response_size) {
            response[pos++] = cherry_first_error_trace.seq;
            response[pos++] = cherry_first_error_trace.cmd;
            response[pos++] = cherry_first_error_trace.req_len;
            response[pos++] = cherry_first_error_trace.resp_len;
            memcpy(&response[pos], cherry_first_error_trace.req, sizeof(cherry_first_error_trace.req));
            pos = (uint8_t)(pos + sizeof(cherry_first_error_trace.req));
            memcpy(&response[pos], cherry_first_error_trace.resp, sizeof(cherry_first_error_trace.resp));
            pos = (uint8_t)(pos + sizeof(cherry_first_error_trace.resp));
            response[2] = 1;
        }
        return pos;
    }
    records_per_page = (uint8_t)((response_size - 3u) / LINK_USB_CHERRY_TRACE_RECORD_WIRE_SIZE);
    if (records_per_page == 0u) {
        records_per_page = 1u;
    }
    skip = (uint8_t)(page * records_per_page);

    response[0] = LINK_USB_CHERRY_TRACE_CMD;
    response[1] = cherry_trace_seq;
    response[2] = 0;

    for (offset = 0; offset < LINK_USB_CHERRY_TRACE_COUNT; offset++) {
        uint8_t slot = (uint8_t)(cherry_trace_next + offset);
        const cherry_trace_record_t *record;

        while (slot >= LINK_USB_CHERRY_TRACE_COUNT) {
            slot = (uint8_t)(slot - LINK_USB_CHERRY_TRACE_COUNT);
        }

        record = &cherry_trace[slot];
        if (record->seq == 0u) {
            continue;
        }
        if (skip > 0u) {
            skip--;
            continue;
        }
        if ((uint8_t)(pos + LINK_USB_CHERRY_TRACE_RECORD_WIRE_SIZE) > response_size) {
            break;
        }

        response[pos++] = record->seq;
        response[pos++] = record->cmd;
        response[pos++] = record->req_len;
        response[pos++] = record->resp_len;
        memcpy(&response[pos], record->req, sizeof(record->req));
        pos = (uint8_t)(pos + sizeof(record->req));
        memcpy(&response[pos], record->resp, sizeof(record->resp));
        pos = (uint8_t)(pos + sizeof(record->resp));
        count++;
    }

    response[2] = count;
    return pos;
}

static void cherry_put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8) & 0xFFu);
    data[2] = (uint8_t)((value >> 16) & 0xFFu);
    data[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint8_t cherry_perf_read(const uint8_t *request, uint8_t request_len,
                                 uint8_t *response, uint8_t response_size)
{
    uint8_t page = (request_len >= 2u) ? request[1] : 0u;

    if ((response == 0) || (response_size < 4u)) {
        return 0;
    }
    response[0] = LINK_USB_CHERRY_PERF_CMD;
    response[1] = LINK_USB_CHERRY_PERF_VERSION;
    response[2] = page;
    response[3] = 0;
    if ((page != 0u) || (response_size < (4u + LINK_USB_CHERRY_PERF_COUNT * 4u))) {
        return 4;
    }
    /* USB callback and SWD counters share the current serialized ISR path.
     * Querying this page does not reset or alter the saved diagnostics. */
    cherry_put_u32(&response[4], link_perf_counters.dap_packets);
    cherry_put_u32(&response[8], link_perf_counters.dp_read_attempts);
    cherry_put_u32(&response[12], link_perf_counters.dp_write_attempts);
    cherry_put_u32(&response[16], link_perf_counters.ap_read_attempts);
    cherry_put_u32(&response[20], link_perf_counters.ap_write_attempts);
    cherry_put_u32(&response[24], link_perf_counters.ack_wait);
    cherry_put_u32(&response[28], link_perf_counters.ack_fault);
    cherry_put_u32(&response[32], link_perf_counters.protocol_error);
    cherry_put_u32(&response[36], link_perf_counters.write_phase_retries);
    cherry_put_u32(&response[40], link_perf_counters.rdbuff_recoveries);
    cherry_put_u32(&response[44], link_perf_counters.powerup_calls);
    cherry_put_u32(&response[48], link_perf_counters.powerup_polls);
    cherry_put_u32(&response[52], link_perf_counters.dp_power_req_calls);
    cherry_put_u32(&response[56], link_perf_counters.dp_power_req_polls);
    response[3] = LINK_USB_CHERRY_PERF_COUNT;
    return (uint8_t)(4u + LINK_USB_CHERRY_PERF_COUNT * 4u);
}

static uint8_t cherry_diag_read(uint8_t *response, uint8_t response_size)
{
    target_arm_swd_debug_diag_t diag;
    cmsis_dap_debug_diag_t dap_diag;

    if ((response == 0) || (response_size < 59u)) {
        return 0;
    }

    target_arm_swd_get_debug_diag(&diag);
    cmsis_dap_get_debug_diag(&dap_diag);
    response[0] = LINK_USB_CHERRY_DIAG_CMD;
    response[1] = diag.mem_stage;
    response[2] = diag.ack_window;
    response[3] = diag.turnaround;
    response[4] = diag.write_turnaround;
    cherry_put_u32(&response[5], diag.mem_detail);
    cherry_put_u32(&response[9], diag.dp_select);
    cherry_put_u32(&response[13], diag.mem_ap_tar);
    cherry_put_u32(&response[17], diag.mem_ap_csw);
    response[21] = diag.dp_port;
    response[22] = diag.dp_rnw;
    response[23] = diag.dp_addr;
    response[24] = diag.dp_status;
    response[25] = diag.dp_ack;
    response[26] = diag.dp_request;
    cherry_put_u32(&response[27], diag.dp_data);
    response[31] = diag.ap_post_addr;
    response[32] = diag.ap_post_status;
    response[33] = diag.ap_post_ack;
    response[34] = diag.ap_post_request;
    cherry_put_u32(&response[35], diag.ap_post_data);
    response[39] = dap_diag.valid;
    response[40] = dap_diag.transfer_request;
    response[41] = (uint8_t)(dap_diag.retries & 0xFFu);
    response[42] = (uint8_t)(dap_diag.retries >> 8);
    cherry_put_u32(&response[43], dap_diag.read_data);
    cherry_put_u32(&response[47], dap_diag.match_value);
    cherry_put_u32(&response[51], dap_diag.match_mask);
    cherry_put_u32(&response[55], 0);
    return 59;
}

static const uint8_t cherry_ms_os_string_desc[18] = {
    USB_MSOSV1_STRING_DESCRIPTOR_INIT(LINK_USB_CHERRY_VENDOR_CODE)
};

static const uint8_t cherry_wcid_desc[] = {
    USB_MSOSV1_COMP_ID_HEADER_DESCRIPTOR_INIT(1),
    USB_MSOSV1_COMP_ID_FUNCTION_WINUSB_DESCRIPTOR_INIT(0),
};

static const uint8_t cherry_if0_wcid_props[] = {
    0x8E, 0x00, 0x00, 0x00,
    0x00, 0x01,
    0x05, 0x00,
    0x01, 0x00,
    0x84, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x28, 0x00,
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00,
    'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 0x00, 0x00,
    0x4E, 0x00, 0x00, 0x00,
    '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00,
    '3', 0x00, 'B', 0x00, '5', 0x00, 'A', 0x00,
    'D', 0x00, '-', 0x00, '2', 0x00, '9', 0x00,
    '3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00,
    '6', 0x00, '6', 0x00, '3', 0x00, '-', 0x00,
    'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00,
    '-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00,
    'E', 0x00, '4', 0x00, '6', 0x00, '4', 0x00,
    '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00,
    '6', 0x00, '}', 0x00, 0x00, 0x00
};

static const uint8_t *cherry_wcid_props[] = {
    cherry_if0_wcid_props,
};

static struct usb_msosv1_descriptor cherry_msosv1_desc = {
    .string = cherry_ms_os_string_desc,
    .vendor_code = LINK_USB_CHERRY_VENDOR_CODE,
    .compat_id = cherry_wcid_desc,
    .comp_id_property = cherry_wcid_props,
};

static const uint8_t cherry_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xFF, 0x00, 0x00, LINK_USB_CHERRY_VID, LINK_USB_CHERRY_PID, 0x0102, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(LINK_USB_CHERRY_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 100),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x04),
    USB_ENDPOINT_DESCRIPTOR_INIT(LINK_USB_CHERRY_EP_IN, USB_ENDPOINT_TYPE_BULK, LINK_USB_CHERRY_EP_MPS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(LINK_USB_CHERRY_EP_OUT, USB_ENDPOINT_TYPE_BULK, LINK_USB_CHERRY_EP_MPS, 0x00),
    USB_LANGID_INIT(1033),
    0x0E, USB_DESCRIPTOR_TYPE_STRING, 'W', 0, 'C', 0, 'H', 0, '.', 0, 'c', 0, 'n', 0,
    0x20, USB_DESCRIPTOR_TYPE_STRING, 'C', 0, 'M', 0, 'S', 0, 'I', 0, 'S', 0, '-', 0, 'D', 0, 'A', 0, 'P', 0,
    ' ', 0, 'C', 0, 'H', 0, '5', 0, '9', 0, '2', 0,
    0x10, USB_DESCRIPTOR_TYPE_STRING, 'D', 0, 'A', 0, 'P', 0, '0', 0, '0', 0, '0', 0, '1', 0,
    0x1A, USB_DESCRIPTOR_TYPE_STRING, 'C', 0, 'M', 0, 'S', 0, 'I', 0, 'S', 0, '-', 0, 'D', 0, 'A', 0, 'P', 0,
    ' ', 0, 'v', 0, '2', 0,
    0x00
};

#if !LINK_BLE_ENABLE
static void cherry_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (nbytes > LINK_USB_CHERRY_EP_MPS) {
        nbytes = LINK_USB_CHERRY_EP_MPS;
    }
    if (nbytes > 0) {
        uint8_t tx_len;

        if (cherry_rx_buf[0] == LINK_USB_CHERRY_TRACE_CMD) {
            tx_len = cherry_trace_read(cherry_rx_buf, (uint8_t)nbytes, cherry_tx_buf, sizeof(cherry_tx_buf));
        } else if (cherry_rx_buf[0] == LINK_USB_CHERRY_DIAG_CMD) {
            tx_len = cherry_diag_read(cherry_tx_buf, sizeof(cherry_tx_buf));
        } else if (cherry_rx_buf[0] == LINK_USB_CHERRY_PERF_CMD) {
            tx_len = cherry_perf_read(cherry_rx_buf, (uint8_t)nbytes, cherry_tx_buf, sizeof(cherry_tx_buf));
#if LINK_USB_PROFILE_TIMING
        } else if (cherry_rx_buf[0] == 0x83u) {
            tx_len = link_timing_read(cherry_rx_buf, (uint8_t)nbytes, cherry_tx_buf, sizeof(cherry_tx_buf));
#endif
        } else {
#if LINK_USB_PROFILE_TIMING
            uint32_t timing_start = link_timing_now();
            target_arm_swd_debug_diag_t timing_context;
            target_arm_swd_get_debug_diag(&timing_context);
#endif
            if (cherry_rx_buf[0] == 0x02u) {
                memset(&link_perf_counters, 0, sizeof(link_perf_counters));
            }
            LINK_PERF_INC(dap_packets);
            tx_len = cmsis_dap_process_packet(cherry_rx_buf, (uint16_t)nbytes,
                                              cherry_tx_buf, sizeof(cherry_tx_buf));
            cherry_trace_log(cherry_rx_buf, (uint8_t)nbytes, cherry_tx_buf, tx_len);
#if LINK_USB_PROFILE_TIMING
            link_timing_record(cherry_rx_buf, (uint8_t)nbytes, cherry_tx_buf, tx_len,
                               timing_start, timing_context.mem_ap_tar);
#endif
        }

        if (tx_len > 0) {
            cherry_tx_busy = true;
            if (usbd_ep_start_write(busid, LINK_USB_CHERRY_EP_IN, cherry_tx_buf, tx_len) < 0) {
                cherry_tx_busy = false;
                usbd_ep_start_read(busid, LINK_USB_CHERRY_EP_OUT, cherry_rx_buf, sizeof(cherry_rx_buf));
            }
            return;
        }
    }
    usbd_ep_start_read(busid, LINK_USB_CHERRY_EP_OUT, cherry_rx_buf, sizeof(cherry_rx_buf));
}

#else
static void cherry_complete_out(uint8_t busid, uint8_t tx_len, uint8_t processing_epoch)
{
    uint32_t usb_irq_enabled = PFIC_GetStatusIRQ(USB_IRQn);
    /* Epoch validation and endpoint handoff form one short USB-only critical
     * section. Radio IRQs remain enabled throughout. */
    PFIC_DisableIRQ(USB_IRQn);
    if (cherry_epoch == processing_epoch && !cherry_reset_pending) {
        if (tx_len > 0) {
            cherry_tx_busy = true;
            if (usbd_ep_start_write(busid, LINK_USB_CHERRY_EP_IN, cherry_tx_buf, tx_len) < 0) {
                cherry_tx_busy = false;
                usbd_ep_start_read(busid, LINK_USB_CHERRY_EP_OUT, cherry_rx_buf, sizeof(cherry_rx_buf));
            }
        } else {
            usbd_ep_start_read(busid, LINK_USB_CHERRY_EP_OUT, cherry_rx_buf, sizeof(cherry_rx_buf));
        }
    }
    if (usb_irq_enabled) PFIC_EnableIRQ(USB_IRQn);
}

static void cherry_process_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
#if LINK_BLE_ENABLE
    uint8_t processing_epoch=cherry_work_epoch;
    const uint8_t *request=cherry_work_buf;
    if (processing_epoch != cherry_epoch || cherry_reset_pending) return;
#else
    const uint8_t *request=cherry_rx_buf;
#endif
    (void)ep;
    if (nbytes > LINK_USB_CHERRY_EP_MPS) {
        nbytes = LINK_USB_CHERRY_EP_MPS;
    }
    if (nbytes > 0) {
        uint8_t tx_len;

        if (request[0] == LINK_USB_CHERRY_TRACE_CMD) {
            tx_len = cherry_trace_read(request, (uint8_t)nbytes, cherry_tx_buf, sizeof(cherry_tx_buf));
        } else if (request[0] == LINK_USB_CHERRY_DIAG_CMD) {
            tx_len = cherry_diag_read(cherry_tx_buf, sizeof(cherry_tx_buf));
        } else if (request[0] == LINK_USB_CHERRY_PERF_CMD) {
            tx_len = cherry_perf_read(request, (uint8_t)nbytes, cherry_tx_buf, sizeof(cherry_tx_buf));
        } else if (request[0] == 0x84u) {
            tx_len = link_ble_usb_diag(cherry_tx_buf,sizeof(cherry_tx_buf));
#if LINK_BLE_ENABLE
        } else if (request[0] == 0x85u) {
            tx_len = link_ble_usb_rx_diag(cherry_tx_buf,sizeof(cherry_tx_buf));
#endif
#if LINK_USB_PROFILE_TIMING
        } else if (request[0] == 0x83u) {
            tx_len = link_timing_read(request, (uint8_t)nbytes, cherry_tx_buf, sizeof(cherry_tx_buf));
#endif
        } else {
#if LINK_USB_PROFILE_TIMING
            uint32_t timing_start = link_timing_now();
            target_arm_swd_debug_diag_t timing_context;
            target_arm_swd_get_debug_diag(&timing_context);
#endif
            if (request[0] == 0x02u) {
                memset(&link_perf_counters, 0, sizeof(link_perf_counters));
            }
            LINK_PERF_INC(dap_packets);
            tx_len = cmsis_dap_process_packet(request, (uint16_t)nbytes,
                                              cherry_tx_buf, sizeof(cherry_tx_buf));
            cherry_trace_log(request, (uint8_t)nbytes, cherry_tx_buf, tx_len);
#if LINK_USB_PROFILE_TIMING
            link_timing_record(request, (uint8_t)nbytes, cherry_tx_buf, tx_len,
                               timing_start, timing_context.mem_ap_tar);
#endif
        }

        cherry_complete_out(busid, tx_len, processing_epoch);
        return;
    }
    cherry_complete_out(busid, 0, processing_epoch);
}

static void cherry_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
#if LINK_BLE_ENABLE
    (void)busid; (void)ep;
    if (nbytes > LINK_USB_CHERRY_EP_MPS) nbytes = LINK_USB_CHERRY_EP_MPS;
    /* OUT remains NAK until response completes: buffer cannot be overwritten. */
    cherry_pending = (uint8_t)nbytes;
    if (!nbytes) usbd_ep_start_read(busid, LINK_USB_CHERRY_EP_OUT, cherry_rx_buf, sizeof(cherry_rx_buf));
#else
    cherry_process_out(busid, ep, nbytes);
#endif
}

#endif

static void cherry_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    cherry_tx_busy = false;
    usbd_ep_start_read(busid, LINK_USB_CHERRY_EP_OUT, cherry_rx_buf, sizeof(cherry_rx_buf));
}

static struct usbd_interface cherry_intf;

static struct usbd_endpoint cherry_out_ep = {
    .ep_addr = LINK_USB_CHERRY_EP_OUT,
    .ep_cb = cherry_out_callback,
};

static struct usbd_endpoint cherry_in_ep = {
    .ep_addr = LINK_USB_CHERRY_EP_IN,
    .ep_cb = cherry_in_callback,
};

static void cherry_event_handler(uint8_t busid, uint8_t event)
{
#if LINK_BLE_ENABLE
    if (event == USBD_EVENT_RESET || event == USBD_EVENT_DISCONNECTED) {
        cherry_pending = 0;
        ++cherry_epoch;
        cherry_reset_pending = 1;
    }
#endif
    if (event == USBD_EVENT_CONFIGURED) {
        cherry_tx_busy = false;
        usbd_ep_start_read(busid, LINK_USB_CHERRY_EP_OUT, cherry_rx_buf, sizeof(cherry_rx_buf));
    }
}

void usb_dc_low_level_init(void)
{
    PFIC_EnableIRQ(USB_IRQn);
}

void usb_dc_low_level_deinit(void)
{
    PFIC_DisableIRQ(USB_IRQn);
}

void link_usb_init(void)
{
    cherry_tx_busy = false;
    memset(&link_perf_counters, 0, sizeof(link_perf_counters));
    cmsis_dap_init();
#if LINK_USB_PROFILE_TIMING
    link_timing_init();
#endif
    usbd_desc_register(LINK_USB_CHERRY_BUSID, cherry_descriptor);
    usbd_msosv1_desc_register(LINK_USB_CHERRY_BUSID, &cherry_msosv1_desc);
    usbd_add_interface(LINK_USB_CHERRY_BUSID, &cherry_intf);
    usbd_add_endpoint(LINK_USB_CHERRY_BUSID, &cherry_out_ep);
    usbd_add_endpoint(LINK_USB_CHERRY_BUSID, &cherry_in_ep);
    usbd_initialize(LINK_USB_CHERRY_BUSID, 0x40008000u, cherry_event_handler);
}

void link_usb_poll(void)
{
#if LINK_BLE_ENABLE
    if (cherry_reset_pending) {
        cherry_reset_pending = 0;
        if (link_session_owner() == LINK_OWNER_USB) {
            target_arm_swd_disconnect();
            link_session_release(LINK_OWNER_USB);
            cmsis_dap_init();
        }
    }
    if (cherry_pending && !cherry_tx_busy) {
        uint8_t count;
        uint32_t usb_irq_enabled = PFIC_GetStatusIRQ(USB_IRQn);
        /* ISR cannot rearm OUT except on reset; guard only the short snapshot. */
        PFIC_DisableIRQ(USB_IRQn);
        count = cherry_pending;
        cherry_work_epoch = cherry_epoch;
        memcpy(cherry_work_buf,cherry_rx_buf,count);
        cherry_pending = 0;
        if (usb_irq_enabled) PFIC_EnableIRQ(USB_IRQn);
        if (count) cherry_process_out(LINK_USB_CHERRY_BUSID, LINK_USB_CHERRY_EP_OUT, count);
    }
#endif
}

uint8_t link_usb_send(const uint8_t *data, uint8_t len)
{
    if ((data == 0) || (len == 0) || cherry_tx_busy || !usb_device_is_configured(LINK_USB_CHERRY_BUSID)) {
        return 0;
    }
    if (len > sizeof(cherry_tx_buf)) {
        len = sizeof(cherry_tx_buf);
    }
    memcpy(cherry_tx_buf, data, len);
    cherry_tx_busy = true;
    if (usbd_ep_start_write(LINK_USB_CHERRY_BUSID, LINK_USB_CHERRY_EP_IN, cherry_tx_buf, len) < 0) {
        cherry_tx_busy = false;
        return 0;
    }
    return 1;
}

#endif
