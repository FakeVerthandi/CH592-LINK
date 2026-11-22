#include <stdint.h>
#include "link_target_loader_layout.h"

#define REG32(address)     (*(volatile uint32_t *)(address))
#define READ_RCC_CR()      REG32(0x40023800u)
#define READ_FLASH_ACR()   REG32(0x40023c00u)
#define WRITE_ACR(v)       (REG32(0x40023c00u) = (v))
#define WRITE_KEY(v)       (REG32(0x40023c04u) = (v))
#define READ_FLASH_SR()    REG32(0x40023c0cu)
#define WRITE_SR(v)        (REG32(0x40023c0cu) = (v))
#define READ_FLASH_CR()    REG32(0x40023c10u)
#define WRITE_CR(v)        (REG32(0x40023c10u) = (v))
#define READ_FLASH_OPTCR() REG32(0x40023c14u)
#define FLASH_HALF(off)    (*(volatile uint16_t *)(TARGET_FLASH_BASE + (off)))
#define FLASH_BYTE(off)    (*(const volatile uint8_t *)(TARGET_FLASH_BASE + (off)))
#define DATA_HALF(off)     (*(const volatile uint16_t *)(TARGET_LOADER_DATA + (off)))
#define WRITE_HALF(off, v) (FLASH_HALF(off) = (v))

#define SR_EOP        (1u << 0)
#define SR_ERRORS     ((1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7))
#define SR_BSY        (1u << 16)
#define CR_PG         (1u << 0)
#define CR_SER        (1u << 1)
#define CR_SNB_SHIFT  3u
#define CR_PSIZE_X16  (1u << 8)
#define CR_STRT       (1u << 16)
#define CR_LOCK       (1u << 31)
#define ACR_ICEN      (1u << 9)
#define ACR_DCEN      (1u << 10)
#define ACR_ICRST     (1u << 11)
#define ACR_DCRST     (1u << 12)
#define POLL_WRITE    131072u
#define POLL_ERASE    20000000u

static uint32_t make_result(uint32_t tag, unsigned status, unsigned completed)
{
    return tag | (status << 9) | (completed & 0x1ffu);
}

static unsigned wait_not_busy(unsigned limit)
{
    for (unsigned attempts = 0; attempts < limit; ++attempts) {
        unsigned sr = READ_FLASH_SR();
        if (!(sr & SR_BSY)) return sr;
    }
    return SR_BSY;
}

static unsigned rdp_active(void)
{
    return ((READ_FLASH_OPTCR() >> 8) & 0xffu) != 0xaau;
}

static unsigned sector_wrp_active(unsigned snb)
{
    return (READ_FLASH_OPTCR() & (1u << (16u + snb))) == 0u;
}

static unsigned unlock_flash(void)
{
    if (READ_FLASH_CR() & CR_LOCK) {
        WRITE_KEY(0x45670123u);
        WRITE_KEY(0xcdef89abu);
        if (READ_FLASH_CR() & CR_LOCK) return 0;
    }
    return 1;
}

static uint32_t disable_and_reset_caches(void)
{
    uint32_t saved = READ_FLASH_ACR();
    uint32_t disabled = saved & ~(ACR_ICEN | ACR_DCEN);
    if (saved & (ACR_ICEN | ACR_DCEN)) {
        WRITE_ACR(disabled);
        WRITE_ACR(disabled | ACR_ICRST | ACR_DCRST);
        WRITE_ACR(disabled);
        __asm volatile("dsb\nisb" ::: "memory");
    }
    return saved;
}

static void restore_caches(uint32_t saved)
{
    if (saved & (ACR_ICEN | ACR_DCEN)) {
        WRITE_ACR(saved);
        __asm volatile("dsb\nisb" ::: "memory");
    }
}

static uint32_t erase_sector(unsigned snb, uint32_t offset, uint32_t flash_size)
{
    if (snb >= 12u || offset >= flash_size)
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_BAD_REQUEST, 0);
    if ((READ_RCC_CR() & 3u) != 3u)
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_CLOCK_OFF, 0);
    if (rdp_active() || sector_wrp_active(snb))
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_PROTECTED, 0);
    if (wait_not_busy(POLL_WRITE) & SR_BSY)
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    if (!unlock_flash())
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_LOCKED, 0);

    WRITE_SR(SR_EOP | SR_ERRORS);
    WRITE_CR(CR_SER | (snb << CR_SNB_SHIFT) | CR_PSIZE_X16);
    WRITE_CR(CR_SER | (snb << CR_SNB_SHIFT) | CR_PSIZE_X16 | CR_STRT);
    __asm volatile("dsb" ::: "memory");

    unsigned sr = wait_not_busy(POLL_ERASE);
    if (sr & SR_BSY) {
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    }

    unsigned status = TARGET_LOADER_DONE;
    if (sr & SR_ERRORS) {
        status = TARGET_LOADER_PROGRAM_ERROR;
    }
    WRITE_SR(SR_EOP | SR_ERRORS);
    WRITE_CR(CR_LOCK);
    if (!(READ_FLASH_CR() & CR_LOCK)) status = TARGET_LOADER_LOCKED;

    uint32_t saved_acr = disable_and_reset_caches();
    restore_caches(saved_acr);
    return make_result(TARGET_RESULT_ERASE_TAG, status, 0);
}

static uint32_t program_block(unsigned count, uint32_t offset, uint32_t flash_size)
{
    if (!count || count > TARGET_LOADER_MAX || ((offset | count) & 1u) ||
        offset >= flash_size || count > flash_size - offset ||
        count > TARGET_VERIFY_CHUNK - (offset & (TARGET_VERIFY_CHUNK - 1u)))
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_BAD_REQUEST, 0);
    if ((READ_RCC_CR() & 3u) != 3u)
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_CLOCK_OFF, 0);
    if (rdp_active())
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_PROTECTED, 0);
    if (wait_not_busy(POLL_WRITE) & SR_BSY)
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    if (!unlock_flash())
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_LOCKED, 0);

    uint32_t saved_acr = disable_and_reset_caches();
    WRITE_SR(SR_EOP | SR_ERRORS);
    WRITE_CR(CR_PG | CR_PSIZE_X16);

    unsigned completed = 0;
    unsigned status = TARGET_LOADER_DONE;
    while (completed != count) {
        if (FLASH_HALF(offset + completed) != 0xffffu) {
            status = TARGET_LOADER_NOT_BLANK;
            break;
        }
        WRITE_HALF(offset + completed, DATA_HALF(completed));
        __asm volatile("dsb" ::: "memory");
        unsigned sr = wait_not_busy(POLL_WRITE);
        if (sr & SR_BSY) {
            restore_caches(saved_acr);
            return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_BUSY_TIMEOUT, completed);
        }
        if (sr & SR_ERRORS) {
            status = TARGET_LOADER_PROGRAM_ERROR;
            break;
        }
        WRITE_SR(SR_EOP);
        completed += 2;
    }

    WRITE_CR(CR_LOCK);
    if (!(READ_FLASH_CR() & CR_LOCK)) status = TARGET_LOADER_LOCKED;
    restore_caches(saved_acr);
    return make_result(TARGET_RESULT_WRITE_TAG, status, completed);
}

static uint32_t verify_chunk(unsigned length, uint32_t offset,
                             uint32_t flash_size, volatile uint32_t *out_crc)
{
    if (length != TARGET_VERIFY_CHUNK || offset >= flash_size ||
        (offset & (TARGET_VERIFY_CHUNK - 1u)) || length > flash_size - offset)
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_BAD_REQUEST, 0);
    if (rdp_active())
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_PROTECTED, 0);
    if (wait_not_busy(POLL_WRITE) & SR_BSY)
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    if (!(READ_FLASH_CR() & CR_LOCK))
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_LOCKED, 0);

    uint32_t saved_acr = disable_and_reset_caches();
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= FLASH_BYTE(offset + i);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0);
    }
    restore_caches(saved_acr);
    *out_crc = crc ^ UINT32_MAX;
    return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_DONE, 0);
}

__attribute__((used)) static void loader_work(void)
{
    volatile uint32_t *mailbox = (volatile uint32_t *)TARGET_LOADER_MAILBOX;
    uint32_t cmd = mailbox[0];
    uint32_t offset = mailbox[1];
    uint32_t flash_size = mailbox[2];
    uint32_t checksum = mailbox[3];
    unsigned op = cmd >> TARGET_OP_SHIFT;
    unsigned arg0 = cmd & 0xffffu;
    uint32_t status;

    if (((cmd ^ offset ^ flash_size) ^ checksum) != UINT32_MAX ||
        !flash_size || flash_size > 0x100000u) {
        uint32_t tag = op == TARGET_OP_ERASE ? TARGET_RESULT_ERASE_TAG :
                       op == TARGET_OP_VERIFY ? TARGET_RESULT_VERIFY_TAG :
                       TARGET_RESULT_WRITE_TAG;
        status = make_result(tag, TARGET_LOADER_BAD_REQUEST, 0);
    } else if (op == TARGET_OP_WRITE) {
        status = program_block(arg0, offset, flash_size);
    } else if (op == TARGET_OP_ERASE) {
        status = erase_sector(arg0, offset, flash_size);
    } else if (op == TARGET_OP_VERIFY) {
        status = verify_chunk(arg0, offset, flash_size, &mailbox[4]);
    } else {
        status = make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_BAD_REQUEST, 0);
    }

    mailbox[0] = status;
    __asm volatile("dsb" ::: "memory");
}

__attribute__((naked, noreturn)) void loader_entry(void)
{
    __asm volatile("bl loader_work\n"
                   ".global loader_stop\nloader_stop:\n"
                   "bkpt #0\n"
                   "1: b 1b");
}

__attribute__((naked, noreturn)) void loader_exception(void)
{
    __asm volatile("bkpt #1\n1: b 1b");
}

__attribute__((used, section(".vectors"))) const uintptr_t loader_vectors[16] = {
    TARGET_LOADER_STACK, (uintptr_t)loader_entry,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception,
    (uintptr_t)loader_exception, 0, 0, 0, 0,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception, 0,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception
};
