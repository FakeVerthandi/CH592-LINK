#include "link_f103_loader.h"
#include "link_f103_loader_layout.h"
#include "target_arm_swd.h"
#include "link_ble_programmer.h"
#include "f103_loader_image.h"

#define ICSR          0xe000ed04u
#define VTOR          0xe000ed08u
#define DHCSR         0xe000edf0u
#define DCRSR         0xe000edf4u
#define DCRDR         0xe000edf8u
#define FLASH_CR      0x40022010u
#define FLASH_OBR     0x4002201cu
#define RCC_CR        0x40021000u
#define RCC_AHBENR    0x40021014u
#define DMA1_CCR1     0x40020008u
#define DMA1_CHANNELS 7u
#define FLASH_LOCK    0x80u
#define DBG_KEY       0xa05f0000u
#define C_DEBUGEN     1u
#define C_HALT        2u
#define S_REGRDY      (1u << 16)
#define S_HALT        (1u << 17)
#define S_LOCKUP      (1u << 19)
#define S_RESET_ST    (1u << 25)
#define REG_WRITE     (1u << 16)
#define RUN_MS        150u
#define REG_MS        100u
#define HALT_MS       250u

enum loader_phase {
    CHECK_HALT, CHECK_THREAD, CHECK_CLOCK, CHECK_PROTECTION, CHECK_DMA_CLOCK, CHECK_DMA_CHANNEL,
    UPLOAD_CODE, CHECK_CODE, SET_VTOR,
    UPLOAD_DATA, SET_REQUEST, SET_COMPLEMENT, REG_VALUE, REG_SELECT, REG_READY,
    START_CORE, WAIT_CORE, READ_PC_SELECT, READ_PC_READY, READ_PC,
    READ_RESULT, READ_LOCK, READ_CRC, CONFIRM_HALT, STOP_CORE, WAIT_STOP
};

static struct {
    enum loader_phase phase;
    const uint8_t *data;
    uint32_t request, deadline, crc;
    uint16_t offset, count;
    uint8_t reg, checked, loaded, may_run;
} loader;

/* Set privileged Thread mode (MSP) and PRIMASK before resuming.
 * VECTACTIVE is checked first so writing xPSR preserves IPSR == 0. */
static const uint8_t selectors[] = {20, 17, 16, 15};
static const uint32_t registers[] = {1u, F103_LOADER_STACK, 1u << 24, F103_LOADER_ENTRY};

static uint8_t expired(void)
{
    return (int32_t)(link_ble_now_ms() - loader.deadline) >= 0;
}

static uint8_t halted(uint32_t dhcsr)
{
    return (dhcsr & (S_HALT | C_DEBUGEN)) == (S_HALT | C_DEBUGEN);
}

static uint8_t healthy(uint32_t dhcsr)
{
    return !(dhcsr & (S_LOCKUP | S_RESET_ST));
}

static link_target_status_t write_next(uint32_t address, uint32_t value, enum loader_phase next)
{
    link_target_status_t status = target_arm_swd_ble_write32(address, value);
    if (status == LINK_TARGET_OK) {
        loader.phase = next;
        return LINK_SWD_PENDING;
    }
    return status;
}

void link_f103_loader_reset(void)
{
    loader.checked = 0;
    loader.loaded = 0;
    loader.may_run = 0;
}

void link_f103_loader_check_begin(uint32_t address)
{
    link_f103_loader_begin(address, 0, 0);
}

void link_f103_loader_begin(uint32_t address, const uint8_t *data, uint16_t count)
{
    loader.request = ((uint32_t)count << 16) | (address - F103_FLASH_BASE);
    loader.data = data;
    loader.count = count;
    loader.offset = 0;
    loader.reg = loader.loaded ? 3 : 0;
    loader.phase = CHECK_HALT;
}

void link_f103_loader_verify_begin(uint32_t address)
{
    link_f103_loader_begin(address, 0, 0);
    loader.request |= F103_REQUEST_VERIFY;
}

static link_target_status_t after_admission(void)
{
    loader.checked = 1;
    if (!loader.count && !(loader.request & F103_REQUEST_VERIFY)) return LINK_TARGET_OK;
    loader.phase = loader.loaded ? (loader.count ? UPLOAD_DATA : SET_REQUEST) : UPLOAD_CODE;
    return LINK_SWD_PENDING;
}

link_target_status_t link_f103_loader_poll(uint32_t *detail, uint32_t *crc)
{
    link_target_status_t status;
    uint32_t v;
    const uint8_t verify = (loader.request & F103_REQUEST_VERIFY) != 0;
    *detail = F103_FLASH_BASE + (loader.request & 0xffffu);
    switch (loader.phase) {
    case CHECK_HALT:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        /* ATTACH has already read DHCSR. A newly observed reset invalidates
         * the session, including any cached SRAM image and CPU context. */
        if (!halted(v) || !healthy(v) || !(v & S_REGRDY)) return LINK_F103_BAD_STATE;
        if (loader.checked) return after_admission();
        loader.phase = CHECK_THREAD;
        return LINK_SWD_PENDING;
    case CHECK_THREAD:
        status = target_arm_swd_ble_read32(ICSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (v & 0x1ffu) return LINK_F103_BAD_STATE;
        loader.phase = CHECK_CLOCK;
        return LINK_SWD_PENDING;
    case CHECK_CLOCK:
        status = target_arm_swd_ble_read32(RCC_CR, &v);
        if (status != LINK_TARGET_OK) return status;
        if ((v & 3u) != 3u) {
            *detail = RCC_CR;
            return LINK_F103_BAD_STATE;
        }
        loader.phase = CHECK_PROTECTION;
        return LINK_SWD_PENDING;
    case CHECK_PROTECTION:
        status = target_arm_swd_ble_read32(FLASH_OBR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (v & 3u) {
            *detail = FLASH_OBR;
            return LINK_F103_BAD_STATE;
        }
        loader.phase = CHECK_DMA_CLOCK;
        return LINK_SWD_PENDING;
    case CHECK_DMA_CLOCK:
        status = target_arm_swd_ble_read32(RCC_AHBENR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!(v & 1u)) return after_admission();
        loader.phase = CHECK_DMA_CHANNEL;
        return LINK_SWD_PENDING;
    case CHECK_DMA_CHANNEL: {
        uint32_t ccr = DMA1_CCR1 + 20u * loader.offset;
        status = target_arm_swd_ble_read32(ccr, &v);
        if (status != LINK_TARGET_OK) return status;
        /* A halted CPU does not stop DMA from modifying SRAM. */
        if (v & 1u) {
            *detail = ccr;
            return LINK_F103_BAD_STATE;
        }
        if (++loader.offset == DMA1_CHANNELS) {
            loader.offset = 0;
            return after_admission();
        }
        return LINK_SWD_PENDING;
    }
    case UPLOAD_CODE:
        status = target_arm_swd_ble_write_sram(F103_LOADER_BASE, (const uint8_t *)f103_loader_image,
                                               sizeof(f103_loader_image));
        if (status != LINK_TARGET_OK) return status;
        loader.offset = 0;
        loader.phase = CHECK_CODE;
        return LINK_SWD_PENDING;
    case CHECK_CODE:
        status = target_arm_swd_ble_read32(F103_LOADER_BASE + loader.offset, &v);
        if (status != LINK_TARGET_OK) return status;
        if (v != f103_loader_image[loader.offset / 4u]) {
            *detail = F103_LOADER_BASE + loader.offset;
            return LINK_F103_BAD_STATE;
        }
        loader.offset += 4;
        if (loader.offset == sizeof(f103_loader_image)) {
            loader.offset = 0;
            loader.phase = SET_VTOR;
        }
        return LINK_SWD_PENDING;
    case SET_VTOR:
        return write_next(VTOR, F103_LOADER_BASE, verify ? SET_REQUEST : UPLOAD_DATA);
    case UPLOAD_DATA:
        status = target_arm_swd_ble_write_sram(F103_LOADER_DATA, loader.data, loader.count);
        if (status != LINK_TARGET_OK) return status;
        loader.phase = SET_REQUEST;
        return LINK_SWD_PENDING;
    case SET_REQUEST:
        return write_next(F103_LOADER_MAILBOX, loader.request, SET_COMPLEMENT);
    case SET_COMPLEMENT:
        return write_next(F103_LOADER_MAILBOX + 4u, ~loader.request, REG_VALUE);
    case REG_VALUE:
        return write_next(DCRDR, registers[loader.reg], REG_SELECT);
    case REG_SELECT:
        status = write_next(DCRSR, REG_WRITE | selectors[loader.reg], REG_READY);
        if (loader.phase == REG_READY) loader.deadline = link_ble_now_ms() + REG_MS;
        return status;
    case REG_READY:
    case READ_PC_READY:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!healthy(v) || !halted(v)) return LINK_F103_BAD_STATE;
        if (!(v & S_REGRDY)) return expired() ? LINK_SWD_TIMEOUT : LINK_SWD_PENDING;
        if (loader.phase == READ_PC_READY) loader.phase = READ_PC;
        else if (++loader.reg == sizeof(selectors)) loader.phase = START_CORE;
        else loader.phase = REG_VALUE;
        return LINK_SWD_PENDING;
    case START_CORE:
        /* Mark launch uncertain before the write can be accepted on the wire. */
        loader.may_run = 1;
        status = write_next(DHCSR, DBG_KEY | C_DEBUGEN, WAIT_CORE);
        if (loader.phase == WAIT_CORE) loader.deadline = link_ble_now_ms() + RUN_MS;
        return status;
    case WAIT_CORE:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!healthy(v) || !(v & C_DEBUGEN)) return LINK_F103_BAD_STATE;
        if (!halted(v)) return expired() ? LINK_SWD_TIMEOUT : LINK_SWD_PENDING;
        if (!(v & S_REGRDY)) return expired() ? LINK_SWD_TIMEOUT : LINK_SWD_PENDING;
        loader.phase = READ_PC_SELECT;
        return LINK_SWD_PENDING;
    case READ_PC_SELECT:
        status = write_next(DCRSR, 15u, READ_PC_READY);
        if (loader.phase == READ_PC_READY) loader.deadline = link_ble_now_ms() + REG_MS;
        return status;
    case READ_PC:
        status = target_arm_swd_ble_read32(DCRDR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (v != F103_LOADER_STOP) {
            *detail = v;
            return LINK_F103_BAD_STATE;
        }
        loader.phase = READ_RESULT;
        return LINK_SWD_PENDING;
    case READ_RESULT: {
        unsigned completed, code;
        status = target_arm_swd_ble_read32(F103_LOADER_MAILBOX, &v);
        if (status != LINK_TARGET_OK) return status;
        completed = v & 0x1ffu;
        code = (v >> 9) & 0x7fu;
        if ((v & F103_RESULT_MASK) != (verify ? F103_VERIFY_TAG : F103_RESULT_TAG) ||
            completed > loader.count ||
            (completed & 1u) || code > F103_LOADER_PROTECTED)
            return LINK_F103_BAD_STATE;
        if (code) {
            *detail += completed;
            return code == F103_LOADER_BUSY_TIMEOUT ? LINK_SWD_TIMEOUT : LINK_F103_FLASH_ERROR;
        }
        if (completed != loader.count) return LINK_F103_BAD_STATE;
        loader.phase = READ_LOCK;
        return LINK_SWD_PENDING;
    }
    case READ_LOCK:
        status = target_arm_swd_ble_read32(FLASH_CR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!(v & FLASH_LOCK)) return LINK_F103_FLASH_ERROR;
        loader.phase = verify ? READ_CRC : CONFIRM_HALT;
        return LINK_SWD_PENDING;
    case READ_CRC:
        status = target_arm_swd_ble_read32(F103_VERIFY_VALUE, &loader.crc);
        if (status != LINK_TARGET_OK) return status;
        loader.phase = CONFIRM_HALT;
        return LINK_SWD_PENDING;
    case CONFIRM_HALT:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!healthy(v) || !halted(v)) return LINK_F103_BAD_STATE;
        loader.may_run = 0;
        loader.loaded = 1;
        if (verify) *crc = loader.crc;
        return LINK_TARGET_OK;
    default:
        return LINK_F103_BAD_STATE;
    }
}

uint8_t link_f103_loader_stop(void)
{
    loader.checked = 0;
    loader.loaded = 0;
    if (!loader.may_run) return 0;
    loader.phase = STOP_CORE;
    return 1;
}

link_target_status_t link_f103_loader_stop_poll(void)
{
    uint32_t v;
    link_target_status_t status;
    if (loader.phase == STOP_CORE) {
        status = write_next(DHCSR, DBG_KEY | C_DEBUGEN | C_HALT, WAIT_STOP);
        if (loader.phase == WAIT_STOP) loader.deadline = link_ble_now_ms() + HALT_MS;
        return status;
    }
    status = target_arm_swd_ble_read32(DHCSR, &v);
    if (status != LINK_TARGET_OK) return status;
    if (!halted(v)) return expired() ? LINK_SWD_TIMEOUT : LINK_SWD_PENDING;
    loader.may_run = 0;
    return LINK_TARGET_OK;
}
