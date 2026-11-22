#include "link_target_loader.h"
#include "link_target_loader_layout.h"
#include "target_arm_swd.h"
#include "link_ble_programmer.h"
#include "target_loader_images.h"

#define ICSR          0xe000ed04u
#define VTOR          0xe000ed08u
#define DHCSR         0xe000edf0u
#define DCRSR         0xe000edf4u
#define DCRDR         0xe000edf8u
#define DBGMCU_ID     0xe0042000u

#define F1_RCC_CR     0x40021000u
#define F1_RCC_AHBENR 0x40021014u
#define F1_DMA1_CCR1  0x40020008u
#define F1_DMA2_CCR1  0x40020408u
#define F1_FLASH_CR   0x40022010u
#define F1_FLASH_OBR  0x4002201cu
#define F1_FLASH_LOCK 0x80u

#define F4_RCC_CR      0x40023800u
#define F4_RCC_AHB1ENR 0x40023830u
#define F4_DMA1_S0CR   0x40026010u
#define F4_DMA2_S0CR   0x40026410u
#define F4_FLASH_CR    0x40023c10u
#define F4_FLASH_OPTCR 0x40023c14u
#define F4_FLASH_LOCK  (1u << 31)

#define DBG_KEY       0xa05f0000u
#define C_DEBUGEN     1u
#define C_HALT        2u
#define S_REGRDY      (1u << 16)
#define S_HALT        (1u << 17)
#define S_LOCKUP      (1u << 19)
#define S_RESET_ST    (1u << 25)
#define REG_WRITE     (1u << 16)

#define WRITE_RUN_MS     200u
#define F1_ERASE_RUN_MS  1000u
#define F4_ERASE_RUN_MS  5000u
#define REG_MS           100u
#define HALT_MS          250u

enum loader_phase {
    CHECK_HALT, CHECK_IDENTITY, CHECK_THREAD, CHECK_CLOCK, CHECK_PROTECTION, CHECK_DMA_CLOCK, CHECK_DMA_CHANNEL,
    UPLOAD_CODE, CHECK_CODE, SET_VTOR,
    UPLOAD_DATA, SET_MB0, SET_MB1, SET_MB2, SET_MB3, REG_VALUE, REG_SELECT, REG_READY,
    START_CORE, WAIT_CORE, READ_PC_SELECT, READ_PC_READY, READ_PC,
    READ_RESULT, READ_LOCK, READ_CRC, CONFIRM_HALT, STOP_CORE, WAIT_STOP
};

static struct {
    enum loader_phase phase;
    const link_target_active_t *target;
    const uint32_t *image;
    uint32_t image_bytes;
    uint32_t entry_addr;
    uint32_t stop_addr;
    const uint8_t *data;
    uint32_t mb_cmd, mb_offset, mb_flash_size, mb_checksum;
    uint32_t deadline, crc, run_ms;
    uint16_t offset, count;
    uint8_t op, reg, checked, loaded, loaded_family, may_run, dma_mask, dma_idx;
} loader;

static const uint8_t selectors[] = {20, 17, 16, 15};

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

static uint8_t is_f4(void)
{
    return loader.target && loader.target->desc &&
           loader.target->desc->family == LINK_FAMILY_STM32F4;
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

void link_target_loader_reset(void)
{
    loader.checked = 0;
    loader.loaded = 0;
    loader.loaded_family = 0;
    loader.may_run = 0;
    loader.target = 0;
}

static void prepare_common(const link_target_active_t *target, uint8_t op,
                           uint32_t cmd_word, uint32_t address,
                           const uint8_t *data, uint16_t count, uint32_t run_ms)
{
    uint8_t family = target->desc->family;
    if (loader.loaded_family != family) {
        loader.checked = 0;
        loader.loaded = 0;
    }
    loader.target = target;
    if (family == LINK_FAMILY_STM32F4) {
        loader.image = f4_loader_image;
        loader.image_bytes = sizeof(f4_loader_image);
        loader.entry_addr = F4_LOADER_ENTRY;
        loader.stop_addr = F4_LOADER_STOP;
    } else {
        loader.image = f1_f3_loader_image;
        loader.image_bytes = sizeof(f1_f3_loader_image);
        loader.entry_addr = F1_F3_LOADER_ENTRY;
        loader.stop_addr = F1_F3_LOADER_STOP;
    }
    loader.op = op;
    loader.mb_cmd = cmd_word;
    loader.mb_offset = address - target->flash_base;
    loader.mb_flash_size = target->flash_bytes;
    loader.mb_checksum = ~(loader.mb_cmd ^ loader.mb_offset ^ loader.mb_flash_size);
    loader.data = data;
    loader.count = count;
    loader.run_ms = run_ms;
    loader.offset = 0;
    loader.dma_mask = 0;
    loader.dma_idx = 0;
    loader.reg = loader.loaded ? 3 : 0;
    loader.phase = CHECK_HALT;
}

void link_target_loader_erase_begin(const link_target_active_t *target,
                                    uint32_t address,
                                    uint32_t unit_bytes,
                                    uint8_t sector_num)
{
    uint32_t flags = (target->desc->family == LINK_FAMILY_STM32F3) ? TARGET_FLAG_F3_OBR : 0u;
    uint32_t arg0 = (target->desc->family == LINK_FAMILY_STM32F4) ? (uint32_t)sector_num : unit_bytes;
    uint32_t cmd = ((uint32_t)TARGET_OP_ERASE << TARGET_OP_SHIFT) |
                   (flags << TARGET_FLAG_SHIFT) | (arg0 & 0xffffu);
    uint32_t timeout = (target->desc->family == LINK_FAMILY_STM32F4) ? F4_ERASE_RUN_MS : F1_ERASE_RUN_MS;
    prepare_common(target, TARGET_OP_ERASE, cmd, address, 0, 0, timeout);
}

void link_target_loader_write_begin(const link_target_active_t *target,
                                    uint32_t address,
                                    const uint8_t *data,
                                    uint16_t count)
{
    uint32_t flags = (target->desc->family == LINK_FAMILY_STM32F3) ? TARGET_FLAG_F3_OBR : 0u;
    uint32_t cmd = ((uint32_t)TARGET_OP_WRITE << TARGET_OP_SHIFT) |
                   (flags << TARGET_FLAG_SHIFT) | (uint32_t)count;
    prepare_common(target, TARGET_OP_WRITE, cmd, address, data, count, WRITE_RUN_MS);
}

void link_target_loader_verify_begin(const link_target_active_t *target,
                                     uint32_t address)
{
    uint32_t flags = (target->desc->family == LINK_FAMILY_STM32F3) ? TARGET_FLAG_F3_OBR : 0u;
    uint32_t cmd = ((uint32_t)TARGET_OP_VERIFY << TARGET_OP_SHIFT) |
                   (flags << TARGET_FLAG_SHIFT) | TARGET_VERIFY_CHUNK;
    prepare_common(target, TARGET_OP_VERIFY, cmd, address, 0, 0, WRITE_RUN_MS);
}

static link_target_status_t after_admission(void)
{
    loader.checked = 1;
    loader.offset = 0;
    loader.phase = loader.loaded ? (loader.op == TARGET_OP_WRITE ? UPLOAD_DATA : SET_MB0)
                                 : UPLOAD_CODE;
    return LINK_SWD_PENDING;
}

static uint32_t reg_value_for(uint8_t idx)
{
    switch (idx) {
    case 0: return 1u;                  /* PRIMASK */
    case 1: return TARGET_LOADER_STACK; /* MSP */
    case 2: return 1u << 24;            /* xPSR (Thumb) */
    default: return loader.entry_addr;  /* PC */
    }
}

static uint8_t next_dma_reg(uint32_t *reg_addr)
{
    uint8_t model = loader.target->desc->dma_model;
    if (model == LINK_DMA_MODEL_F4_2) {
        while (loader.dma_idx < 16u) {
            uint8_t idx = loader.dma_idx++;
            if (idx < 8u && (loader.dma_mask & 1u)) {
                *reg_addr = F4_DMA1_S0CR + 24u * (uint32_t)idx;
                return 1;
            }
            if (idx >= 8u && (loader.dma_mask & 2u)) {
                *reg_addr = F4_DMA2_S0CR + 24u * (uint32_t)(idx - 8u);
                return 1;
            }
        }
        return 0;
    }
    while (loader.dma_idx < 12u) {
        uint8_t idx = loader.dma_idx++;
        if (idx < 7u && (loader.dma_mask & 1u)) {
            *reg_addr = F1_DMA1_CCR1 + 20u * (uint32_t)idx;
            return 1;
        }
        if (idx >= 7u && model == LINK_DMA_MODEL_F1_F3_2 && (loader.dma_mask & 2u)) {
            *reg_addr = F1_DMA2_CCR1 + 20u * (uint32_t)(idx - 7u);
            return 1;
        }
    }
    return 0;
}

link_target_status_t link_target_loader_poll(uint32_t *detail, uint32_t *crc)
{
    link_target_status_t status;
    uint32_t v;

    if (!loader.target || !loader.target->desc) return LINK_LOADER_BAD_STATE;
    *detail = loader.target->flash_base + loader.mb_offset;

    switch (loader.phase) {
    case CHECK_HALT:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!halted(v) || !healthy(v) || !(v & S_REGRDY)) return LINK_LOADER_BAD_STATE;
        if (!loader.checked || loader.op == TARGET_OP_ERASE) {
            loader.phase = CHECK_IDENTITY;
            return LINK_SWD_PENDING;
        }
        return after_admission();
    case CHECK_IDENTITY:
        /* Re-verify DBGMCU DEV_ID before first admission and before every ERASE
         * so swapping boards mid-session (e.g. F4 <-> F1) aborts safely before
         * running the wrong family loader or touching Flash. */
        status = target_arm_swd_ble_read32(DBGMCU_ID, &v);
        if (status != LINK_TARGET_OK) return status;
        if ((v & 0xfffu) != loader.target->desc->dev_id) {
            *detail = v;
            return LINK_LOADER_BAD_STATE;
        }
        if (loader.checked) return after_admission();
        loader.phase = CHECK_THREAD;
        return LINK_SWD_PENDING;
    case CHECK_THREAD:
        status = target_arm_swd_ble_read32(ICSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (v & 0x1ffu) return LINK_LOADER_BAD_STATE;
        loader.phase = CHECK_CLOCK;
        return LINK_SWD_PENDING;
    case CHECK_CLOCK: {
        uint32_t rcc_cr = is_f4() ? F4_RCC_CR : F1_RCC_CR;
        status = target_arm_swd_ble_read32(rcc_cr, &v);
        if (status != LINK_TARGET_OK) return status;
        if ((v & 3u) != 3u) {
            *detail = rcc_cr;
            return LINK_LOADER_BAD_STATE;
        }
        loader.phase = CHECK_PROTECTION;
        return LINK_SWD_PENDING;
    }
    case CHECK_PROTECTION:
        if (is_f4()) {
            status = target_arm_swd_ble_read32(F4_FLASH_OPTCR, &v);
            if (status != LINK_TARGET_OK) return status;
            if (((v >> 8) & 0xffu) != 0xaau) {
                *detail = F4_FLASH_OPTCR;
                return LINK_LOADER_BAD_STATE;
            }
        } else {
            uint32_t mask = (loader.target->desc->family == LINK_FAMILY_STM32F3) ? 7u : 3u;
            status = target_arm_swd_ble_read32(F1_FLASH_OBR, &v);
            if (status != LINK_TARGET_OK) return status;
            if (v & mask) {
                *detail = F1_FLASH_OBR;
                return LINK_LOADER_BAD_STATE;
            }
        }
        loader.phase = CHECK_DMA_CLOCK;
        return LINK_SWD_PENDING;
    case CHECK_DMA_CLOCK:
        if (loader.target->desc->dma_model == LINK_DMA_MODEL_F4_2) {
            status = target_arm_swd_ble_read32(F4_RCC_AHB1ENR, &v);
            if (status != LINK_TARGET_OK) return status;
            loader.dma_mask = (uint8_t)(((v >> 21) & 1u) | (((v >> 22) & 1u) << 1));
        } else {
            status = target_arm_swd_ble_read32(F1_RCC_AHBENR, &v);
            if (status != LINK_TARGET_OK) return status;
            loader.dma_mask = (uint8_t)(v & (loader.target->desc->dma_model == LINK_DMA_MODEL_F1_F3_2 ? 3u : 1u));
        }
        loader.dma_idx = 0;
        if (!loader.dma_mask) return after_admission();
        loader.phase = CHECK_DMA_CHANNEL;
        return LINK_SWD_PENDING;
    case CHECK_DMA_CHANNEL: {
        uint32_t reg_addr = 0;
        if (loader.offset == 0u) {
            if (!next_dma_reg(&reg_addr)) return after_admission();
            loader.deadline = reg_addr; /* stash current DMA reg address */
            loader.offset = 1u;
        } else {
            reg_addr = loader.deadline;
        }
        status = target_arm_swd_ble_read32(reg_addr, &v);
        if (status != LINK_TARGET_OK) return status;
        loader.offset = 0u;
        if (v & 1u) {
            *detail = reg_addr;
            return LINK_LOADER_BAD_STATE;
        }
        return LINK_SWD_PENDING;
    }
    case UPLOAD_CODE: {
        uint32_t remaining = loader.image_bytes - loader.offset;
        uint16_t chunk = remaining > 1024u ? 1024u : (uint16_t)remaining;
        status = target_arm_swd_ble_write_sram(TARGET_LOADER_BASE + loader.offset,
                                               ((const uint8_t *)loader.image) + loader.offset,
                                               chunk);
        if (status != LINK_TARGET_OK) return status;
        loader.offset = (uint16_t)(loader.offset + chunk);
        if (loader.offset >= loader.image_bytes) {
            loader.offset = 0;
            loader.phase = CHECK_CODE;
        }
        return LINK_SWD_PENDING;
    }
    case CHECK_CODE:
        status = target_arm_swd_ble_read32(TARGET_LOADER_BASE + loader.offset, &v);
        if (status != LINK_TARGET_OK) return status;
        if (v != loader.image[loader.offset / 4u]) {
            *detail = TARGET_LOADER_BASE + loader.offset;
            return LINK_LOADER_BAD_STATE;
        }
        loader.offset += 4u;
        if (loader.offset >= loader.image_bytes) {
            loader.offset = 0;
            loader.phase = SET_VTOR;
        }
        return LINK_SWD_PENDING;
    case SET_VTOR:
        return write_next(VTOR, TARGET_LOADER_BASE,
                          loader.op == TARGET_OP_WRITE ? UPLOAD_DATA : SET_MB0);
    case UPLOAD_DATA:
        status = target_arm_swd_ble_write_sram(TARGET_LOADER_DATA, loader.data, loader.count);
        if (status != LINK_TARGET_OK) return status;
        loader.phase = SET_MB0;
        return LINK_SWD_PENDING;
    case SET_MB0:
        return write_next(TARGET_LOADER_MAILBOX, loader.mb_cmd, SET_MB1);
    case SET_MB1:
        return write_next(TARGET_LOADER_MAILBOX + 4u, loader.mb_offset, SET_MB2);
    case SET_MB2:
        return write_next(TARGET_LOADER_MAILBOX + 8u, loader.mb_flash_size, SET_MB3);
    case SET_MB3:
        return write_next(TARGET_LOADER_MAILBOX + 12u, loader.mb_checksum, REG_VALUE);
    case REG_VALUE:
        return write_next(DCRDR, reg_value_for(loader.reg), REG_SELECT);
    case REG_SELECT:
        status = write_next(DCRSR, REG_WRITE | selectors[loader.reg], REG_READY);
        if (loader.phase == REG_READY) loader.deadline = link_ble_now_ms() + REG_MS;
        return status;
    case REG_READY:
    case READ_PC_READY:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!healthy(v) || !halted(v)) return LINK_LOADER_BAD_STATE;
        if (!(v & S_REGRDY)) return expired() ? LINK_SWD_TIMEOUT : LINK_SWD_PENDING;
        if (loader.phase == READ_PC_READY) loader.phase = READ_PC;
        else if (++loader.reg == sizeof(selectors)) loader.phase = START_CORE;
        else loader.phase = REG_VALUE;
        return LINK_SWD_PENDING;
    case START_CORE:
        loader.may_run = 1;
        status = write_next(DHCSR, DBG_KEY | C_DEBUGEN, WAIT_CORE);
        if (loader.phase == WAIT_CORE) loader.deadline = link_ble_now_ms() + loader.run_ms;
        return status;
    case WAIT_CORE:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!healthy(v) || !(v & C_DEBUGEN)) return LINK_LOADER_BAD_STATE;
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
        if (v != loader.stop_addr) {
            *detail = v;
            return LINK_LOADER_BAD_STATE;
        }
        loader.phase = READ_RESULT;
        return LINK_SWD_PENDING;
    case READ_RESULT: {
        unsigned completed, code;
        uint32_t expected_tag = loader.op == TARGET_OP_ERASE ? TARGET_RESULT_ERASE_TAG :
                                loader.op == TARGET_OP_VERIFY ? TARGET_RESULT_VERIFY_TAG :
                                TARGET_RESULT_WRITE_TAG;
        status = target_arm_swd_ble_read32(TARGET_LOADER_MAILBOX, &v);
        if (status != LINK_TARGET_OK) return status;
        completed = v & 0x1ffu;
        code = (v >> 9) & 0x7fu;
        if ((v & TARGET_RESULT_MASK) != expected_tag ||
            completed > loader.count ||
            (completed & 1u) || code > TARGET_LOADER_PROTECTED)
            return LINK_LOADER_BAD_STATE;
        if (code) {
            *detail += completed;
            return code == TARGET_LOADER_BUSY_TIMEOUT ? LINK_SWD_TIMEOUT : LINK_LOADER_FLASH_ERROR;
        }
        if (completed != loader.count) return LINK_LOADER_BAD_STATE;
        loader.phase = READ_LOCK;
        return LINK_SWD_PENDING;
    }
    case READ_LOCK: {
        uint32_t cr_addr = is_f4() ? F4_FLASH_CR : F1_FLASH_CR;
        uint32_t lock_bit = is_f4() ? F4_FLASH_LOCK : F1_FLASH_LOCK;
        status = target_arm_swd_ble_read32(cr_addr, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!(v & lock_bit)) return LINK_LOADER_FLASH_ERROR;
        loader.phase = (loader.op == TARGET_OP_VERIFY) ? READ_CRC : CONFIRM_HALT;
        return LINK_SWD_PENDING;
    }
    case READ_CRC:
        status = target_arm_swd_ble_read32(TARGET_MAILBOX_CRC, &loader.crc);
        if (status != LINK_TARGET_OK) return status;
        loader.phase = CONFIRM_HALT;
        return LINK_SWD_PENDING;
    case CONFIRM_HALT:
        status = target_arm_swd_ble_read32(DHCSR, &v);
        if (status != LINK_TARGET_OK) return status;
        if (!healthy(v) || !halted(v)) return LINK_LOADER_BAD_STATE;
        loader.may_run = 0;
        loader.loaded = 1;
        loader.loaded_family = loader.target->desc->family;
        if (loader.op == TARGET_OP_VERIFY && crc) *crc = loader.crc;
        return LINK_TARGET_OK;
    default:
        return LINK_LOADER_BAD_STATE;
    }
}

uint8_t link_target_loader_stop(void)
{
    loader.checked = 0;
    loader.loaded = 0;
    loader.loaded_family = 0;
    if (!loader.may_run) return 0;
    loader.phase = STOP_CORE;
    return 1;
}

link_target_status_t link_target_loader_stop_poll(void)
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
