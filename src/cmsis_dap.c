#include <string.h>

#include "CH59x_common.h"

#include "cmsis_dap.h"
#include "target_arm_swd.h"
#include "link_config.h"
#if LINK_BLE_ENABLE
#include "link_session.h"
#include "link_ble.h"
#include "HAL.h"
#endif

#define ID_DAP_INFO                0x00
#define ID_DAP_HOST_STATUS         0x01
#define ID_DAP_CONNECT             0x02
#define ID_DAP_DISCONNECT          0x03
#define ID_DAP_TRANSFER_CONFIGURE  0x04
#define ID_DAP_TRANSFER            0x05
#define ID_DAP_TRANSFER_BLOCK      0x06
#define ID_DAP_TRANSFER_ABORT      0x07
#define ID_DAP_WRITE_ABORT         0x08
#define ID_DAP_DELAY               0x09
#define ID_DAP_RESET_TARGET        0x0A
#define ID_DAP_SWJ_PINS            0x10
#define ID_DAP_SWJ_CLOCK           0x11
#define ID_DAP_SWJ_SEQUENCE        0x12
#define ID_DAP_SWD_CONFIGURE       0x13
#define ID_DAP_SWD_SEQUENCE        0x1D

#define DAP_INFO_VENDOR            0x01
#define DAP_INFO_PRODUCT           0x02
#define DAP_INFO_SER_NUM           0x03
#define DAP_INFO_PROTOCOL_VER      0x04
#define DAP_INFO_PRODUCT_FW_VER    0x09
#define DAP_INFO_CAPABILITIES      0xF0
#define DAP_INFO_PACKET_COUNT      0xFE
#define DAP_INFO_PACKET_SIZE       0xFF

#define DAP_PORT_DISABLED          0x00
#define DAP_PORT_SWD               0x01

#define DAP_OK                     0x00
#define DAP_ERROR                  0xFF

#define DAP_TRANSFER_APNDP         0x01
#define DAP_TRANSFER_RNW           0x02
#define DAP_TRANSFER_MATCH_VALUE   0x10
#define DAP_TRANSFER_MATCH_MASK    0x20

#define DAP_TRANSFER_OK            0x01
#define DAP_TRANSFER_WAIT          0x02
#define DAP_TRANSFER_FAULT         0x04
#define DAP_TRANSFER_ERROR         0x08
#define DAP_TRANSFER_MISMATCH      0x10

static uint16_t dap_wait_retry;
static uint16_t dap_match_retry;
static uint32_t dap_match_mask;
static uint8_t dap_connected;
static cmsis_dap_debug_diag_t dap_debug_diag;

static uint16_t dap_get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t dap_get_u32(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void dap_delay_us(uint16_t delay_us)
{
    while (delay_us > 1000u) {
        DelayMs(1);
        delay_us = (uint16_t)(delay_us - 1000u);
    }
    if (delay_us > 0u) {
        DelayUs(delay_us);
    }
}

static void dap_put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)(value >> 8);
}

static void dap_put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8) & 0xFFu);
    data[2] = (uint8_t)((value >> 16) & 0xFFu);
    data[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint8_t dap_status_from_target(link_target_status_t status)
{
    if (status == LINK_TARGET_OK) {
        return DAP_TRANSFER_OK;
    }
    if (status == LINK_TARGET_WAIT) {
        return DAP_TRANSFER_WAIT;
    }
    if (status == LINK_TARGET_FAULT) {
        return DAP_TRANSFER_FAULT;
    }
    return DAP_TRANSFER_ERROR;
}

static link_target_status_t dap_run_transfer_with_retry(uint8_t transfer_request,
                                                        uint32_t write_data,
                                                        uint32_t *read_data)
{
    uint32_t retry;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    for (retry = 0; retry <= dap_wait_retry; retry++) {
        status = target_arm_swd_dap_transfer(transfer_request, write_data, read_data);
        if (status != LINK_TARGET_WAIT) {
            break;
        }
    }
    return status;
}

static link_target_status_t dap_post_ap_read_with_retry(uint8_t transfer_request, uint32_t *posted_data)
{
    uint32_t retry;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    for (retry = 0; retry <= dap_wait_retry; retry++) {
        status = target_arm_swd_dap_ap_read_post((uint8_t)(transfer_request & 0x0Cu), posted_data);
        if (status != LINK_TARGET_WAIT) {
            break;
        }
    }
    return status;
}

static link_target_status_t dap_read_rdbuff_with_retry(uint32_t *read_data)
{
    uint32_t retry;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    for (retry = 0; retry <= dap_wait_retry; retry++) {
        status = target_arm_swd_dap_read_rdbuff(read_data);
        if (status != LINK_TARGET_WAIT) {
            break;
        }
    }
    return status;
}

static link_target_status_t dap_check_write_with_retry(void)
{
    uint32_t ignored = 0;

    return dap_read_rdbuff_with_retry(&ignored);
}

static uint8_t dap_write_needs_rdbuff_check(uint8_t transfer_request)
{
    (void)transfer_request;
    return 0;
}

static uint8_t dap_info_string(uint8_t id, uint8_t *response, uint16_t response_size)
{
    const char *text = "";
    uint8_t len;

    switch (id) {
    case DAP_INFO_VENDOR:
        text = "Pointful";
        break;
    case DAP_INFO_PRODUCT:
        text = "CMSIS-DAP CH592";
        break;
    case DAP_INFO_SER_NUM:
        text = "DAP0001";
        break;
    case DAP_INFO_PROTOCOL_VER:
        text = "2.1.0";
        break;
    case DAP_INFO_PRODUCT_FW_VER:
#if LINK_USB_PROFILE_TIMING
        text = "0.2.7-prof";
#else
#if LINK_BLE_ENABLE
#if LINK_BLE_SRAM_WRITE
        text = "0.4.2-ble";
#else
        text = "0.3.10-ble";
#endif
#else
        text = "0.2.7-clock";
#endif
#endif
        break;
    default:
        response[1] = 0;
        return 2;
    }

    len = (uint8_t)strlen(text);
    if ((uint16_t)len + 2u > response_size) {
        len = (uint8_t)(response_size - 2u);
    }
    response[1] = (uint8_t)(len + 1u);
    memcpy(&response[2], text, len + 1u);
    return (uint8_t)(len + 3u);
}

static uint8_t dap_cmd_info(const uint8_t *request, uint16_t request_len,
                            uint8_t *response, uint16_t response_size)
{
    uint8_t id;

    if ((request_len < 2u) || (response_size < 4u)) {
        response[1] = 0;
        return 2;
    }

    id = request[1];
    response[0] = ID_DAP_INFO;

    switch (id) {
    case DAP_INFO_VENDOR:
    case DAP_INFO_PRODUCT:
    case DAP_INFO_SER_NUM:
    case DAP_INFO_PROTOCOL_VER:
    case DAP_INFO_PRODUCT_FW_VER:
        return dap_info_string(id, response, response_size);
    case DAP_INFO_CAPABILITIES:
        response[1] = 1;
        response[2] = 0x01; /* SWD supported */
        return 3;
    case DAP_INFO_PACKET_COUNT:
        response[1] = 1;
#if LINK_USB_USE_CHERRYUSB && LINK_BLE_ENABLE
        response[2] = 3;
#else
        response[2] = 1;
#endif
        return 3;
    case DAP_INFO_PACKET_SIZE:
        response[1] = 2;
        dap_put_u16(&response[2], CMSIS_DAP_PACKET_SIZE);
        return 4;
    default:
        response[1] = 0;
        return 2;
    }
}

static uint8_t dap_cmd_transfer_configure(const uint8_t *request, uint16_t request_len,
                                          uint8_t *response)
{
    if (request_len >= 6u) {
        dap_wait_retry = dap_get_u16(&request[2]);
        dap_match_retry = dap_get_u16(&request[4]);
    }
    response[0] = ID_DAP_TRANSFER_CONFIGURE;
    response[1] = DAP_OK;
    return 2;
}

static uint8_t dap_cmd_transfer(const uint8_t *request, uint16_t request_len,
                                uint8_t *response, uint16_t response_size)
{
    uint8_t transfer_count;
    uint8_t transfer_index;
    uint16_t req_pos = 3;
    uint16_t resp_pos = 3;
    uint8_t transfer_response = DAP_TRANSFER_OK;
    uint8_t post_read = 0;
    uint8_t check_write = 0;

    response[0] = ID_DAP_TRANSFER;
    response[1] = 0;
    response[2] = DAP_TRANSFER_OK;

    if ((request_len < 3u) || !dap_connected) {
        response[2] = DAP_TRANSFER_ERROR;
        return 3;
    }

    transfer_count = request[2];
    for (transfer_index = 0; transfer_index < transfer_count; transfer_index++) {
        uint8_t transfer_request;
        uint32_t write_data = 0;
        uint32_t read_data = 0;
        link_target_status_t status = LINK_TARGET_IO_ERROR;

        if (req_pos >= request_len) {
            transfer_response = DAP_TRANSFER_ERROR;
            break;
        }

        transfer_request = request[req_pos++];
        if (transfer_request & DAP_TRANSFER_MATCH_MASK) {
            if (req_pos + 4u > request_len) {
                transfer_response = DAP_TRANSFER_ERROR;
                break;
            }
            dap_match_mask = dap_get_u32(&request[req_pos]);
            req_pos += 4u;
            response[1]++;
            continue;
        }

        if ((transfer_request & DAP_TRANSFER_RNW) == 0u) {
            if (req_pos + 4u > request_len) {
                transfer_response = DAP_TRANSFER_ERROR;
                break;
            }
            write_data = dap_get_u32(&request[req_pos]);
            req_pos += 4u;
        } else if (resp_pos + 4u > response_size) {
            transfer_response = DAP_TRANSFER_ERROR;
            break;
        }

        if (post_read) {
            if ((transfer_request & DAP_TRANSFER_RNW) &&
                ((transfer_request & (DAP_TRANSFER_APNDP | DAP_TRANSFER_MATCH_VALUE)) == DAP_TRANSFER_APNDP)) {
                /* AP reads are posted: this transaction returns the previous AP data
                 * while starting the next AP read, matching ARM CMSIS-DAP behavior. */
                status = dap_post_ap_read_with_retry(transfer_request, &read_data);
            } else {
                status = dap_read_rdbuff_with_retry(&read_data);
                post_read = 0;
            }
            transfer_response = dap_status_from_target(status);
            if (transfer_response != DAP_TRANSFER_OK) {
                break;
            }
            if (resp_pos + 4u > response_size) {
                transfer_response = DAP_TRANSFER_ERROR;
                break;
            }
            dap_put_u32(&response[resp_pos], read_data);
            resp_pos += 4u;
        }

        if (transfer_request & DAP_TRANSFER_RNW) {
            check_write = 0;
            if (transfer_request & DAP_TRANSFER_MATCH_VALUE) {
                uint32_t match_retry = 0;
                uint32_t match_value;

                if (post_read) {
                    status = dap_read_rdbuff_with_retry(&read_data);
                    post_read = 0;
                    transfer_response = dap_status_from_target(status);
                    if (transfer_response != DAP_TRANSFER_OK) {
                        break;
                    }
                }
                if (req_pos + 4u > request_len) {
                    transfer_response = DAP_TRANSFER_ERROR;
                    break;
                }
                match_value = dap_get_u32(&request[req_pos]);
                if (transfer_request & DAP_TRANSFER_APNDP) {
                    status = dap_post_ap_read_with_retry(transfer_request, 0);
                    transfer_response = dap_status_from_target(status);
                    if (transfer_response != DAP_TRANSFER_OK) {
                        break;
                    }
                    status = dap_read_rdbuff_with_retry(&read_data);
                    transfer_response = dap_status_from_target(status);
                    if (transfer_response != DAP_TRANSFER_OK) {
                        break;
                    }
                } else {
                    status = dap_run_transfer_with_retry(transfer_request, 0, &read_data);
                    transfer_response = dap_status_from_target(status);
                    if (transfer_response != DAP_TRANSFER_OK) {
                        break;
                    }
                }
                dap_debug_diag.valid = 0;
                /* The first sample is already available: permit at most the
                 * configured number of additional samples.  Use a wide counter
                 * so the maximum 16-bit retry setting cannot wrap. */
                while (((read_data & dap_match_mask) != (match_value & dap_match_mask)) &&
                       (match_retry < dap_match_retry)) {
                    match_retry++;
                    if (transfer_request & DAP_TRANSFER_APNDP) {
                        status = dap_post_ap_read_with_retry(transfer_request, 0);
                        if (status == LINK_TARGET_OK) {
                            status = dap_read_rdbuff_with_retry(&read_data);
                        }
                    } else {
                        status = dap_run_transfer_with_retry(transfer_request, 0, &read_data);
                    }
                    if (status != LINK_TARGET_OK) {
                        transfer_response = dap_status_from_target(status);
                        break;
                    }
                }
                dap_debug_diag.valid = 1;
                dap_debug_diag.transfer_request = transfer_request;
                dap_debug_diag.retries = (uint16_t)match_retry;
                dap_debug_diag.read_data = read_data;
                dap_debug_diag.match_value = match_value;
                dap_debug_diag.match_mask = dap_match_mask;
                if ((read_data & dap_match_mask) != (match_value & dap_match_mask)) {
                    /* Mismatch is a flag in addition to the underlying ACK. */
                    transfer_response |= DAP_TRANSFER_MISMATCH;
                }
                if (transfer_response != DAP_TRANSFER_OK) {
                    break;
                }
                req_pos += 4u;
            } else {
                if (transfer_request & DAP_TRANSFER_APNDP) {
                    if (!post_read) {
                        status = dap_post_ap_read_with_retry(transfer_request, 0);
                        transfer_response = dap_status_from_target(status);
                        if (transfer_response != DAP_TRANSFER_OK) {
                            break;
                        }
                        post_read = 1;
                    }
                } else {
                    status = dap_run_transfer_with_retry(transfer_request, 0, &read_data);
                    transfer_response = dap_status_from_target(status);
                    if (transfer_response != DAP_TRANSFER_OK) {
                        break;
                    }
                    dap_put_u32(&response[resp_pos], read_data);
                    resp_pos += 4u;
                }
            }
        } else {
            status = dap_run_transfer_with_retry(transfer_request, write_data, &read_data);
            transfer_response = dap_status_from_target(status);
            if (transfer_response != DAP_TRANSFER_OK) {
                break;
            }
            check_write = dap_write_needs_rdbuff_check(transfer_request);
        }

        response[1]++;
    }

    if (transfer_response == DAP_TRANSFER_OK && post_read) {
        uint32_t read_data = 0;
        link_target_status_t status = dap_read_rdbuff_with_retry(&read_data);

        transfer_response = dap_status_from_target(status);
        if (transfer_response == DAP_TRANSFER_OK) {
            if (resp_pos + 4u > response_size) {
                transfer_response = DAP_TRANSFER_ERROR;
            } else {
                dap_put_u32(&response[resp_pos], read_data);
                resp_pos += 4u;
            }
        }
    } else if (transfer_response == DAP_TRANSFER_OK && check_write) {
        link_target_status_t status = dap_check_write_with_retry();

        transfer_response = dap_status_from_target(status);
    }

    response[2] = transfer_response;
    return (uint8_t)resp_pos;
}

static uint8_t dap_cmd_transfer_block(const uint8_t *request, uint16_t request_len,
                                      uint8_t *response, uint16_t response_size)
{
    uint16_t transfer_count;
    uint16_t transfer_index;
    uint16_t req_pos = 5;
    uint16_t resp_pos = 4;
    uint8_t transfer_request;
    uint8_t transfer_response = DAP_TRANSFER_OK;
    uint8_t check_write = 0;

    response[0] = ID_DAP_TRANSFER_BLOCK;
    dap_put_u16(&response[1], 0);
    response[3] = DAP_TRANSFER_OK;

    if ((request_len < 5u) || !dap_connected) {
        response[3] = DAP_TRANSFER_ERROR;
        return 4;
    }

    transfer_count = dap_get_u16(&request[2]);
    transfer_request = request[4];
#if LINK_USB_USE_CHERRYUSB
    if ((transfer_request == 0x0Fu) && (transfer_count >= 2u) &&
        (4u + (uint32_t)transfer_count * 4u <= response_size)) {
        uint16_t done = 0;
        link_target_status_t status = target_arm_swd_dap_read_block(
            transfer_request, &response[4], transfer_count, dap_wait_retry, &done);

        if (status != LINK_TARGET_NOT_IMPLEMENTED) {
            dap_put_u16(&response[1], done);
            response[3] = dap_status_from_target(status);
            return (uint8_t)(4u + done * 4u);
        }
    }
    if ((transfer_request == 0x0Du) && (transfer_count >= 2u) &&
        (5u + (uint32_t)transfer_count * 4u <= request_len)) {
        uint16_t done = 0;
        link_target_status_t status = target_arm_swd_dap_write_block(
            transfer_request, &request[5], transfer_count, dap_wait_retry, &done);

        if (status != LINK_TARGET_NOT_IMPLEMENTED) {
            dap_put_u16(&response[1], done);
            response[3] = dap_status_from_target(status);
            return 4;
        }
    }
#endif
    for (transfer_index = 0; transfer_index < transfer_count; transfer_index++) {
        uint32_t write_data = 0;
        uint32_t read_data = 0;
        link_target_status_t status = LINK_TARGET_IO_ERROR;

        if (transfer_request & DAP_TRANSFER_RNW) {
            if (resp_pos + 4u > response_size) {
                transfer_response = DAP_TRANSFER_ERROR;
                break;
            }
        } else {
            if (req_pos + 4u > request_len) {
                transfer_response = DAP_TRANSFER_ERROR;
                break;
            }
            write_data = dap_get_u32(&request[req_pos]);
            req_pos += 4u;
        }

        if ((transfer_index == 0u) && (transfer_request & DAP_TRANSFER_RNW) &&
            (transfer_request & DAP_TRANSFER_APNDP)) {
            status = dap_post_ap_read_with_retry(transfer_request, 0);
            transfer_response = dap_status_from_target(status);
            if (transfer_response != DAP_TRANSFER_OK) {
                break;
            }
        }

        if ((transfer_request & DAP_TRANSFER_RNW) && (transfer_request & DAP_TRANSFER_APNDP)) {
            if (transfer_index == (uint16_t)(transfer_count - 1u)) {
                status = dap_read_rdbuff_with_retry(&read_data);
            } else {
                /* TransferBlock AP read block: return previous posted data while
                 * posting the next AP read; final data is drained from DP RDBUFF. */
                status = dap_post_ap_read_with_retry(transfer_request, &read_data);
            }
        } else {
            status = dap_run_transfer_with_retry(transfer_request, write_data, &read_data);
        }

        transfer_response = dap_status_from_target(status);
        if (transfer_response != DAP_TRANSFER_OK) {
            break;
        }

        if (transfer_request & DAP_TRANSFER_RNW) {
            dap_put_u32(&response[resp_pos], read_data);
            resp_pos += 4u;
        } else {
            check_write = dap_write_needs_rdbuff_check(transfer_request);
        }
    }

    if ((transfer_response == DAP_TRANSFER_OK) && check_write) {
        link_target_status_t status = dap_check_write_with_retry();

        transfer_response = dap_status_from_target(status);
    }

    dap_put_u16(&response[1], transfer_index);
    response[3] = transfer_response;
    return (uint8_t)resp_pos;
}

static uint8_t dap_cmd_swd_sequence(const uint8_t *request, uint16_t request_len,
                                    uint8_t *response, uint16_t response_size)
{
    uint8_t sequence_count;
    uint8_t sequence_index;
    uint16_t req_pos = 2;
    uint16_t resp_pos = 2;

    response[0] = ID_DAP_SWD_SEQUENCE;
    response[1] = DAP_OK;
    if (request_len < 2u) {
        response[1] = DAP_ERROR;
        return 2;
    }

    sequence_count = request[1];
    for (sequence_index = 0; sequence_index < sequence_count; sequence_index++) {
        uint8_t info;
        uint8_t bit_count;
        uint16_t byte_count;

        if (req_pos >= request_len) {
            break;
        }
        info = request[req_pos++];
        bit_count = info & 0x3Fu;
        if (bit_count == 0u) {
            bit_count = 64u;
        }
        byte_count = (uint16_t)((bit_count + 7u) >> 3);
        if ((info & 0x80u) == 0u) {
            if (req_pos + byte_count > request_len) {
                response[1] = DAP_ERROR;
                break;
            }
            target_arm_swd_swd_sequence(1, bit_count, &request[req_pos], 0);
            req_pos += byte_count;
        } else {
            if (resp_pos + byte_count > response_size) {
                response[1] = DAP_ERROR;
                break;
            }
            target_arm_swd_swd_sequence(0, bit_count, 0, &response[resp_pos]);
            resp_pos += byte_count;
        }
    }

    return (uint8_t)resp_pos;
}

void cmsis_dap_init(void)
{
    dap_wait_retry = 100;
    dap_match_retry = 0;
    dap_match_mask = 0xFFFFFFFFu;
    dap_connected = 0;
    memset(&dap_debug_diag, 0, sizeof(dap_debug_diag));
}

void cmsis_dap_get_debug_diag(cmsis_dap_debug_diag_t *diag)
{
    if (diag == 0) {
        return;
    }
    *diag = dap_debug_diag;
}

uint8_t cmsis_dap_process_packet(const uint8_t *request, uint16_t request_len,
                                 uint8_t *response, uint16_t response_size)
{
    uint8_t cmd;

    if ((request == 0) || (request_len == 0) || (response == 0) || (response_size == 0)) {
        return 0;
    }

    cmd = request[0];
    response[0] = cmd;

#if LINK_BLE_ENABLE
    if (cmd == ID_DAP_CONNECT) {
        if (!link_session_claim(LINK_OWNER_USB)) { response[1] = 0; return 2; }
    } else if (cmd == ID_DAP_DISCONNECT) {
        if (link_session_owner() != LINK_OWNER_USB) { response[1] = DAP_OK; return 2; }
    } else if ((cmd >= ID_DAP_TRANSFER && cmd <= ID_DAP_SWD_SEQUENCE) &&
               link_session_owner() != LINK_OWNER_USB) {
        if (cmd == ID_DAP_TRANSFER) { response[1]=0; response[2]=DAP_TRANSFER_ERROR; return 3; }
        if (cmd == ID_DAP_TRANSFER_BLOCK) { response[1]=0; response[2]=0; response[3]=DAP_TRANSFER_ERROR; return 4; }
        response[1] = DAP_ERROR; return 2;
    }
#endif
    switch (cmd) {
    case ID_DAP_INFO:
        return dap_cmd_info(request, request_len, response, response_size);
    case ID_DAP_HOST_STATUS:
        response[1] = DAP_OK;
        return 2;
    case ID_DAP_CONNECT:
#if !LINK_BLE_ENABLE
        (void)target_arm_swd_connect();
#endif
        {
            uint32_t idcode = 0;
#if LINK_BLE_ENABLE
            uint32_t start = link_ble_now_ms();
            link_target_status_t status;
            target_arm_swd_ble_attach_cancel();
            do {
                status = target_arm_swd_ble_attach(&idcode);
                /* Callbacks only queue data. BLE programmer runs in outer loop. */
                TMOS_SystemProcess();
                if (status != LINK_TARGET_WAIT) break;
            } while ((uint32_t)(link_ble_now_ms() - start) < 1000u);
            target_arm_swd_ble_attach_cancel();
#else
            link_target_status_t status = target_arm_swd_read_idcode(&idcode);

            if (status == LINK_TARGET_OK) {
                status = target_arm_swd_dap_calibrate_write_phase();
            }
#endif
            if (status == LINK_TARGET_OK) {
                dap_connected = DAP_PORT_SWD;
                response[1] = DAP_PORT_SWD;
            } else {
                dap_connected = DAP_PORT_DISABLED;
                response[1] = DAP_PORT_DISABLED;
#if LINK_BLE_ENABLE
                link_session_release(LINK_OWNER_USB);
#endif
            }
        }
        return 2;
    case ID_DAP_DISCONNECT:
        target_arm_swd_disconnect();
#if LINK_BLE_ENABLE
        link_session_release(LINK_OWNER_USB);
#endif
        dap_connected = DAP_PORT_DISABLED;
        response[1] = 0;
        return 2;
    case ID_DAP_TRANSFER_CONFIGURE:
        return dap_cmd_transfer_configure(request, request_len, response);
    case ID_DAP_TRANSFER:
        return dap_cmd_transfer(request, request_len, response, response_size);
    case ID_DAP_TRANSFER_BLOCK:
        return dap_cmd_transfer_block(request, request_len, response, response_size);
    case ID_DAP_TRANSFER_ABORT:
        return 0;
    case ID_DAP_WRITE_ABORT:
        if (request_len >= 6u) {
            link_target_status_t status = target_arm_swd_dap_transfer(0x00, dap_get_u32(&request[2]), 0);

            response[1] = ((status == LINK_TARGET_OK) || (status == LINK_TARGET_FAULT)) ? DAP_OK : DAP_ERROR;
        } else {
            response[1] = DAP_ERROR;
        }
        return 2;
    case ID_DAP_DELAY:
        if (request_len >= 3u) {
            dap_delay_us(dap_get_u16(&request[1]));
        }
        response[1] = DAP_OK;
        return 2;
    case ID_DAP_RESET_TARGET:
        response[1] = DAP_OK;
        response[2] = 0;
        return 3;
    case ID_DAP_SWJ_PINS:
        if (request_len >= 8u) {
            response[1] = target_arm_swd_swj_pins(request[1], request[2]);
        } else {
            response[1] = target_arm_swd_swj_pins(0, 0);
        }
        return 2;
    case ID_DAP_SWJ_CLOCK:
        if (request_len >= 5u) {
            target_arm_swd_set_clock(dap_get_u32(&request[1]));
        }
        response[1] = 0;
        return 2;
    case ID_DAP_SWJ_SEQUENCE:
        if (request_len >= 3u) {
            uint16_t bit_count = request[1];

            if (bit_count == 0u) {
                bit_count = 256u;
            }
            target_arm_swd_swj_sequence(bit_count, &request[2]);
        }
        response[1] = DAP_OK;
        return 2;
    case ID_DAP_SWD_CONFIGURE:
        response[1] = DAP_ERROR;
        if (request_len >= 2u) {
            target_arm_swd_configure(request[1]);
            /* Command boundaries have no outstanding DAP posted read. */
            response[1] = (!dap_connected || (target_arm_swd_dap_calibrate_write_phase() == LINK_TARGET_OK)) ? DAP_OK : DAP_ERROR;
        }
        return 2;
    case ID_DAP_SWD_SEQUENCE:
        return dap_cmd_swd_sequence(request, request_len, response, response_size);
    default:
        response[0] = DAP_ERROR;
        return 1;
    }
}
