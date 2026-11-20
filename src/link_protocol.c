#include <string.h>

#include "CH59x_common.h"

#include "link_protocol.h"
#include "link_target.h"
#include "link_usb.h"

static uint8_t link_rx_buf[64];
static uint16_t link_rx_len;
static uint8_t link_tx_buf[64];
static uint8_t link_tx_len;

static uint32_t link_protocol_get_u32(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void link_protocol_put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFF);
    data[1] = (uint8_t)((value >> 8) & 0xFF);
    data[2] = (uint8_t)((value >> 16) & 0xFF);
    data[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void link_protocol_prepare_reply(uint8_t reply_cmd, uint8_t status, const uint8_t *payload, uint8_t payload_len)
{
    uint8_t total_len = (uint8_t)(payload_len + 3);

    if (total_len > sizeof(link_tx_buf)) {
        total_len = sizeof(link_tx_buf);
        if (total_len >= 3) {
            payload_len = (uint8_t)(total_len - 3);
        } else {
            payload_len = 0;
        }
    }

    link_tx_buf[0] = reply_cmd;
    link_tx_buf[1] = status;
    link_tx_buf[2] = payload_len;

    if ((payload != 0) && (payload_len > 0)) {
        memcpy(&link_tx_buf[3], payload, payload_len);
    }

    link_tx_len = total_len;
}

static void link_protocol_prepare_diag_reply(uint8_t reply_cmd, uint8_t status)
{
    link_target_diag_t diag;

    link_target_get_diag(&diag);
    link_tx_buf[0] = reply_cmd;
    link_tx_buf[1] = status;
    link_tx_buf[2] = 8;
    link_tx_buf[3] = diag.raw_ack;
    link_tx_buf[4] = diag.attempts;
    link_tx_buf[5] = diag.dio_read_high;
    link_tx_buf[6] = diag.dio_read_low;
    link_tx_buf[7] = diag.dio_read_release;
    link_tx_buf[8] = diag.strategy;
    link_tx_buf[9] = diag.turnaround;
    link_tx_buf[10] = diag.ack_window;
    link_tx_len = 11;
}

void link_protocol_init(void)
{
    link_rx_len = 0;
    link_tx_len = 0;
    memset(link_rx_buf, 0, sizeof(link_rx_buf));
    memset(link_tx_buf, 0, sizeof(link_tx_buf));
}

void link_protocol_on_rx(const uint8_t *data, uint16_t len)
{
    if ((data == 0) || (len == 0)) {
        return;
    }

    if (len > sizeof(link_rx_buf)) {
        len = sizeof(link_rx_buf);
    }

    memcpy(link_rx_buf, data, len);
    link_rx_len = len;
}

void link_protocol_poll(void)
{
    if (link_tx_len != 0) {
        if (link_usb_send(link_tx_buf, link_tx_len)) {
            link_tx_len = 0;
        }
        return;
    }

    if (link_rx_len == 0) {
        return;
    }

    switch (link_rx_buf[0]) {
    case LINK_CMD_NOP:
        link_protocol_prepare_reply(0x80 | LINK_CMD_NOP, 0x00, 0, 0);
        break;
    case LINK_CMD_CONNECT:
        link_protocol_prepare_reply(0x80 | LINK_CMD_CONNECT, link_target_connect(), 0, 0);
        break;
    case LINK_CMD_TARGET_RESET:
        link_protocol_prepare_reply(0x80 | LINK_CMD_TARGET_RESET, link_target_reset(), 0, 0);
        break;
    case LINK_CMD_READ_ID:
    {
        uint32_t idcode = 0;
        link_target_status_t status = link_target_read_id(&idcode);
        link_target_diag_t diag;

        link_tx_buf[0] = 0x80 | LINK_CMD_READ_ID;
        link_tx_buf[1] = status;
        if (status == LINK_TARGET_OK) {
            link_tx_buf[2] = 4;
            link_tx_buf[3] = (uint8_t)(idcode & 0xFF);
            link_tx_buf[4] = (uint8_t)((idcode >> 8) & 0xFF);
            link_tx_buf[5] = (uint8_t)((idcode >> 16) & 0xFF);
            link_tx_buf[6] = (uint8_t)((idcode >> 24) & 0xFF);
            link_tx_len = 7;
        } else {
            link_target_get_diag(&diag);
            link_tx_buf[2] = 8;
            link_tx_buf[3] = diag.raw_ack;
            link_tx_buf[4] = diag.attempts;
            link_tx_buf[5] = diag.dio_read_high;
            link_tx_buf[6] = diag.dio_read_low;
            link_tx_buf[7] = diag.dio_read_release;
            link_tx_buf[8] = diag.strategy;
            link_tx_buf[9] = diag.turnaround;
            link_tx_buf[10] = diag.ack_window;
            link_tx_len = 11;
        }
        break;
    }
    case LINK_CMD_READ_MEM:
    {
        uint32_t addr;
        uint32_t values[15];
        uint8_t word_count;
        uint8_t word_index;
        link_target_status_t status = LINK_TARGET_OK;

        if (link_rx_len < 6) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_READ_MEM, LINK_TARGET_BAD_PARAM, 0, 0);
            break;
        }

        addr = link_protocol_get_u32(&link_rx_buf[1]);
        word_count = link_rx_buf[5];
        if ((word_count == 0) || (word_count > 15)) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_READ_MEM, LINK_TARGET_BAD_PARAM, 0, 0);
            break;
        }

        link_tx_buf[0] = 0x80 | LINK_CMD_READ_MEM;
        link_tx_buf[2] = (uint8_t)(word_count * 4);
        status = link_target_read32_block(addr, values, word_count);
        for (word_index = 0; (status == LINK_TARGET_OK) && (word_index < word_count); word_index++) {
            link_protocol_put_u32(&link_tx_buf[3 + (word_index * 4)], values[word_index]);
        }
        link_tx_buf[1] = status;
        if (status != LINK_TARGET_OK) {
            link_target_diag_t diag;

            link_target_get_diag(&diag);
            link_tx_buf[2] = 8;
            link_tx_buf[3] = diag.raw_ack;
            link_tx_buf[4] = diag.attempts;
            link_tx_buf[5] = diag.dio_read_high;
            link_tx_buf[6] = diag.dio_read_low;
            link_tx_buf[7] = diag.dio_read_release;
            link_tx_buf[8] = diag.strategy;
            link_tx_buf[9] = diag.turnaround;
            link_tx_buf[10] = diag.ack_window;
            link_tx_len = 11;
        } else {
            link_tx_len = (uint8_t)(3 + link_tx_buf[2]);
        }
        break;
    }
    case LINK_CMD_WRITE_MEM:
    {
        uint32_t addr;
        uint8_t word_count;
        uint8_t word_index;
        link_target_status_t status = LINK_TARGET_OK;

        if (link_rx_len < 6) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_WRITE_MEM, LINK_TARGET_BAD_PARAM, 0, 0);
            break;
        }

        addr = link_protocol_get_u32(&link_rx_buf[1]);
        word_count = link_rx_buf[5];
        if ((word_count == 0) || (word_count > 14) || (link_rx_len < (uint16_t)(6 + (word_count * 4)))) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_WRITE_MEM, LINK_TARGET_BAD_PARAM, 0, 0);
            break;
        }

        for (word_index = 0; word_index < word_count; word_index++) {
            uint32_t value = link_protocol_get_u32(&link_rx_buf[6 + (word_index * 4)]);

            status = link_target_write32(addr + ((uint32_t)word_index * 4), value);
            if (status != LINK_TARGET_OK) {
                break;
            }
        }
        link_protocol_prepare_reply(0x80 | LINK_CMD_WRITE_MEM, status, 0, 0);
        break;
    }
    case LINK_CMD_FLASH_ERASE:
    {
        uint32_t addr;
        link_target_status_t status;

        if (link_rx_len < 5) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_FLASH_ERASE, LINK_TARGET_BAD_PARAM, 0, 0);
            break;
        }

        addr = link_protocol_get_u32(&link_rx_buf[1]);
        status = link_target_flash_erase_page(addr);
        if ((status == LINK_TARGET_OK) || (status == LINK_TARGET_BAD_PARAM) ||
            (status == LINK_TARGET_NOT_CONNECTED) || (status == LINK_TARGET_NOT_IMPLEMENTED)) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_FLASH_ERASE, status, 0, 0);
        } else {
            link_protocol_prepare_diag_reply(0x80 | LINK_CMD_FLASH_ERASE, status);
        }
        break;
    }
    case LINK_CMD_FLASH_WRITE:
    {
        uint32_t addr;
        uint8_t data_len;
        link_target_status_t status;

        if (link_rx_len < 6) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_FLASH_WRITE, LINK_TARGET_BAD_PARAM, 0, 0);
            break;
        }

        addr = link_protocol_get_u32(&link_rx_buf[1]);
        data_len = link_rx_buf[5];
        if ((data_len == 0) || ((data_len & 0x01u) != 0) ||
            (data_len > 58) || (link_rx_len < (uint16_t)(6 + data_len))) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_FLASH_WRITE, LINK_TARGET_BAD_PARAM, 0, 0);
            break;
        }

        status = link_target_flash_write(addr, &link_rx_buf[6], data_len);
        if ((status == LINK_TARGET_OK) || (status == LINK_TARGET_BAD_PARAM) ||
            (status == LINK_TARGET_NOT_CONNECTED) || (status == LINK_TARGET_NOT_IMPLEMENTED)) {
            link_protocol_prepare_reply(0x80 | LINK_CMD_FLASH_WRITE, status, 0, 0);
        } else {
            link_protocol_prepare_diag_reply(0x80 | LINK_CMD_FLASH_WRITE, status);
        }
        break;
    }
    default:
        link_protocol_prepare_reply(0xFF, 0xFE, 0, 0);
        break;
    }

    link_rx_len = 0;
}
