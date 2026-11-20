#include <string.h>

#include "CH59x_common.h"

#include "link_config.h"
#include "link_protocol.h"
#include "link_usb.h"

#if !LINK_USB_USE_CHERRYUSB

#define LINK_USB_EP0_SIZE 64
#define LINK_USB_EP1_SIZE 8
#define LINK_USB_EP2_SIZE 64
#define LINK_USB_MS_VENDOR_CODE 0x20

static uint8_t usb_dev_config;
static uint8_t usb_setup_req_code;
static uint16_t usb_setup_req_len;
static const uint8_t *usb_descr;

static const uint8_t link_usb_dev_descr[] = {
    0x12, 0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, LINK_USB_EP0_SIZE,
    0x86, 0x1A, 0x92, 0x59, 0x01, 0x01, 0x01, 0x02,
    0x03, 0x01
};

static const uint8_t link_usb_cfg_descr[] = {
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x03, 0xFF, 0x01, 0x01, 0x00,
    0x07, 0x05, 0x81, 0x03, LINK_USB_EP1_SIZE, 0x00, 0x01,
    0x07, 0x05, 0x82, 0x02, LINK_USB_EP2_SIZE, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, LINK_USB_EP2_SIZE, 0x00, 0x00
};

static const uint8_t link_usb_lang_descr[] = {
    0x04, 0x03, 0x09, 0x04
};

static const uint8_t link_usb_manu_descr[] = {
    0x0E, 0x03, 'W', 0, 'C', 0, 'H', 0, '.', 0, 'c', 0, 'n', 0
};

static const uint8_t link_usb_prod_descr[] = {
    0x16, 0x03, 'C', 0, 'H', 0, '5', 0, '9', 0, '2', 0, ' ', 0, 'L', 0, 'I', 0, 'N', 0, 'K', 0
};

static const uint8_t link_usb_serial_descr[] = {
    0x12, 0x03, 'L', 0, 'I', 0, 'N', 0, 'K', 0, '0', 0, '0', 0, '0', 0, '1', 0
};

static const uint8_t link_usb_ms_os_string_descr[] = {
    0x12, 0x03,
    'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0,
    LINK_USB_MS_VENDOR_CODE, 0x00
};

static const uint8_t link_usb_ms_compat_id_descr[] = {
    0x28, 0x00, 0x00, 0x00,
    0x00, 0x01,
    0x04, 0x00,
    0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
    0x01,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t link_usb_ms_ext_prop_descr[] = {
    0x8E, 0x00, 0x00, 0x00,
    0x00, 0x01,
    0x05, 0x00,
    0x01, 0x00,
    0x84, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x28, 0x00,
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00,
    'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00,
    'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 0x00, 0x00,
    0x4E, 0x00, 0x00, 0x00,
    '{', 0x00, 'B', 0x00, 'F', 0x00, 'B', 0x00, '9', 0x00,
    'D', 0x00, '7', 0x00, '4', 0x00, '6', 0x00, '-', 0x00,
    '5', 0x00, '2', 0x00, '1', 0x00, '6', 0x00, '-', 0x00,
    '4', 0x00, 'A', 0x00, '7', 0x00, '8', 0x00, '-', 0x00,
    '8', 0x00, '1', 0x00, '5', 0x00, '6', 0x00, '-', 0x00,
    'E', 0x00, '4', 0x00, '1', 0x00, '4', 0x00, '1', 0x00,
    '1', 0x00, '3', 0x00, 'D', 0x00, '7', 0x00, '3', 0x00,
    'E', 0x00, '6', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00
};

__attribute__((aligned(4))) static uint8_t ep0_databuf[64 + 64 + 64];
__attribute__((aligned(4))) static uint8_t ep1_databuf[64 + 64];
__attribute__((aligned(4))) static uint8_t ep2_databuf[64 + 64];
__attribute__((aligned(4))) static uint8_t ep3_databuf[64 + 64];

void link_usb_init(void)
{
    pEP0_RAM_Addr = ep0_databuf;
    pEP1_RAM_Addr = ep1_databuf;
    pEP2_RAM_Addr = ep2_databuf;
    pEP3_RAM_Addr = ep3_databuf;

    usb_dev_config = 0;
    usb_setup_req_code = 0;
    usb_setup_req_len = 0;
    usb_descr = 0;

    USB_DeviceInit();
    PFIC_EnableIRQ(USB_IRQn);
}

void link_usb_poll(void)
{
}

uint8_t link_usb_send(const uint8_t *data, uint8_t len)
{
    if ((data == 0) || (len == 0)) {
        return 0;
    }

    if (!EP2_GetINSta()) {
        return 0;
    }

    if (len > LINK_USB_EP2_SIZE) {
        len = LINK_USB_EP2_SIZE;
    }

    memcpy(pEP2_IN_DataBuf, data, len);
    DevEP2_IN_Deal(len);
    return 1;
}

void DevEP1_OUT_Deal(uint8_t len)
{
    (void)len;
}

void DevEP2_OUT_Deal(uint8_t len)
{
    link_protocol_on_rx(pEP2_OUT_DataBuf, len);
}

void DevEP3_OUT_Deal(uint8_t len)
{
    (void)len;
}

void DevEP4_OUT_Deal(uint8_t len)
{
    (void)len;
}

void USB_DevTransProcess(void)
{
    uint8_t len;
    uint8_t chtype;
    uint8_t intflag;
    uint8_t errflag = 0;

    intflag = R8_USB_INT_FG;

    if (intflag & RB_UIF_TRANSFER) {
        if ((R8_USB_INT_ST & MASK_UIS_TOKEN) != MASK_UIS_TOKEN) {
            switch (R8_USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {
            case UIS_TOKEN_IN | 0:
                switch (usb_setup_req_code) {
                case USB_GET_DESCRIPTOR:
                    len = (usb_setup_req_len >= LINK_USB_EP0_SIZE) ? LINK_USB_EP0_SIZE : usb_setup_req_len;
                    memcpy(pEP0_DataBuf, usb_descr, len);
                    usb_setup_req_len -= len;
                    usb_descr += len;
                    R8_UEP0_T_LEN = len;
                    R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                    break;
                case USB_SET_ADDRESS:
                    R8_USB_DEV_AD = (R8_USB_DEV_AD & RB_UDA_GP_BIT) | usb_setup_req_len;
                    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;
                default:
                    R8_UEP0_T_LEN = 0;
                    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;
                }
                break;

            case UIS_TOKEN_OUT | 0:
                len = R8_USB_RX_LEN;
                (void)len;
                break;

            case UIS_TOKEN_OUT | 1:
                if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                    DevEP1_OUT_Deal(R8_USB_RX_LEN);
                }
                break;

            case UIS_TOKEN_IN | 1:
                R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                break;

            case UIS_TOKEN_OUT | 2:
                if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                    DevEP2_OUT_Deal(R8_USB_RX_LEN);
                }
                break;

            case UIS_TOKEN_IN | 2:
                R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                break;

            default:
                break;
            }

            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }

        if (R8_USB_INT_ST & RB_UIS_SETUP_ACT) {
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
            usb_setup_req_len = pSetupReqPak->wLength;
            usb_setup_req_code = pSetupReqPak->bRequest;
            chtype = pSetupReqPak->bRequestType;

            len = 0;
            errflag = 0;

            if ((pSetupReqPak->bRequestType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD) {
                if (((pSetupReqPak->bRequestType & USB_REQ_TYP_MASK) == USB_REQ_TYP_VENDOR) &&
                    (chtype & 0x80) &&
                    (usb_setup_req_code == LINK_USB_MS_VENDOR_CODE)) {
                    switch (pSetupReqPak->wIndex) {
                    case 0x0004:
                        usb_descr = link_usb_ms_compat_id_descr;
                        len = sizeof(link_usb_ms_compat_id_descr);
                        break;
                    case 0x0005:
                        usb_descr = link_usb_ms_ext_prop_descr;
                        len = sizeof(link_usb_ms_ext_prop_descr);
                        break;
                    default:
                        errflag = 0xFF;
                        break;
                    }

                    if ((errflag == 0) && (usb_setup_req_len > len)) {
                        usb_setup_req_len = len;
                    }
                    if (errflag == 0) {
                        len = (usb_setup_req_len >= LINK_USB_EP0_SIZE) ? LINK_USB_EP0_SIZE : usb_setup_req_len;
                        memcpy(pEP0_DataBuf, usb_descr, len);
                        usb_descr += len;
                    }
                } else {
                    errflag = 0xFF;
                }
            } else {
                switch (usb_setup_req_code) {
                case USB_GET_DESCRIPTOR:
                    switch ((pSetupReqPak->wValue) >> 8) {
                    case USB_DESCR_TYP_DEVICE:
                        usb_descr = link_usb_dev_descr;
                        len = sizeof(link_usb_dev_descr);
                        break;
                    case USB_DESCR_TYP_CONFIG:
                        usb_descr = link_usb_cfg_descr;
                        len = sizeof(link_usb_cfg_descr);
                        break;
                    case USB_DESCR_TYP_STRING:
                        switch ((pSetupReqPak->wValue) & 0xFF) {
                        case 0:
                            usb_descr = link_usb_lang_descr;
                            len = sizeof(link_usb_lang_descr);
                            break;
                        case 1:
                            usb_descr = link_usb_manu_descr;
                            len = sizeof(link_usb_manu_descr);
                            break;
                        case 2:
                            usb_descr = link_usb_prod_descr;
                            len = sizeof(link_usb_prod_descr);
                            break;
                        case 3:
                            usb_descr = link_usb_serial_descr;
                            len = sizeof(link_usb_serial_descr);
                            break;
                        case 0xEE:
                            usb_descr = link_usb_ms_os_string_descr;
                            len = sizeof(link_usb_ms_os_string_descr);
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                        }
                        break;
                    default:
                        errflag = 0xFF;
                        break;
                    }

                    if (usb_setup_req_len > len) {
                        usb_setup_req_len = len;
                    }
                    len = (usb_setup_req_len >= LINK_USB_EP0_SIZE) ? LINK_USB_EP0_SIZE : usb_setup_req_len;
                    memcpy(pEP0_DataBuf, usb_descr, len);
                    usb_descr += len;
                    break;

                case USB_SET_ADDRESS:
                    usb_setup_req_len = (pSetupReqPak->wValue) & 0xFF;
                    break;

                case USB_GET_CONFIGURATION:
                    pEP0_DataBuf[0] = usb_dev_config;
                    if (usb_setup_req_len > 1) {
                        usb_setup_req_len = 1;
                    }
                    break;

                case USB_SET_CONFIGURATION:
                    usb_dev_config = (pSetupReqPak->wValue) & 0xFF;
                    break;

                case USB_CLEAR_FEATURE:
                    if ((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                        switch ((pSetupReqPak->wIndex) & 0xFF) {
                        case 0x81:
                            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                            break;
                        case 0x01:
                            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK;
                            break;
                        case 0x82:
                            R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                            break;
                        case 0x02:
                            R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK;
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                        }
                    } else {
                        errflag = 0xFF;
                    }
                    break;

                case USB_GET_INTERFACE:
                    pEP0_DataBuf[0] = 0x00;
                    if (usb_setup_req_len > 1) {
                        usb_setup_req_len = 1;
                    }
                    break;

                case USB_GET_STATUS:
                    pEP0_DataBuf[0] = 0x00;
                    pEP0_DataBuf[1] = 0x00;
                    if (usb_setup_req_len > 2) {
                        usb_setup_req_len = 2;
                    }
                    break;

                default:
                    errflag = 0xFF;
                    break;
                }
            }

            if (errflag == 0xFF) {
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
            } else {
                if (chtype & 0x80) {
                    len = (usb_setup_req_len > LINK_USB_EP0_SIZE) ? LINK_USB_EP0_SIZE : usb_setup_req_len;
                    usb_setup_req_len -= len;
                } else {
                    len = 0;
                }
                R8_UEP0_T_LEN = len;
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
    } else if (intflag & RB_UIF_BUS_RST) {
        R8_USB_DEV_AD = 0;
        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    } else if (intflag & RB_UIF_SUSPEND) {
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    } else {
        R8_USB_INT_FG = intflag;
    }
}

__HIGH_CODE
__INTERRUPT
void USB_IRQHandler(void)
{
    USB_DevTransProcess();
}

#endif
