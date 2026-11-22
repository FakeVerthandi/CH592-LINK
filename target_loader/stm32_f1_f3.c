#include <stdint.h>
#include "link_target_loader_layout.h"

#define REG32(address)     (*(volatile uint32_t *)(address))
#define READ_RCC_CR()      REG32(0x40021000u)
#define WRITE_KEY(v)       (REG32(0x40022004u) = (v))
#define READ_FLASH_SR()    REG32(0x4002200cu)
#define WRITE_SR(v)        (REG32(0x4002200cu) = (v))
#define READ_FLASH_CR()    REG32(0x40022010u)
#define WRITE_CR(v)        (REG32(0x40022010u) = (v))
#define WRITE_AR(v)        (REG32(0x40022014u) = (v))
#define READ_FLASH_OBR()   REG32(0x4002201cu)
#define FLASH_HALF(off)    (*(volatile uint16_t *)(TARGET_FLASH_BASE + (off)))
#define FLASH_BYTE(off)    (*(const volatile uint8_t *)(TARGET_FLASH_BASE + (off)))
#define DATA_HALF(off)     (*(const volatile uint16_t *)(TARGET_LOADER_DATA + (off)))
#define WRITE_HALF(off, v) (FLASH_HALF(off) = (v))

#define BSY          0x01u
#define ERRORS       0x14u
#define EOP          0x20u
#define PG           0x01u
#define PER          0x02u
#define STRT         0x40u
#define LOCK         0x80u
#define POLL_WRITE   65536u
#define POLL_ERASE   4000000u

static uint32_t make_result(uint32_t tag, unsigned status, unsigned completed)
{
    return tag | (status << 9) | (completed & 0x1ffu);
}

static unsigned wait_status(unsigned mask, unsigned limit)
{
    for (unsigned attempts = 0; attempts < limit; ++attempts) {
        unsigned sr = READ_FLASH_SR();
        if (!(sr & BSY) && (!mask || (sr & mask))) return sr;
    }
    return BSY;
}

static unsigned is_protected(unsigned flags)
{
    unsigned mask = (flags & TARGET_FLAG_F3_OBR) ? 7u : 3u;
    return (READ_FLASH_OBR() & mask) != 0u;
}

static unsigned unlock_flash(void)
{
    if (READ_FLASH_CR() & LOCK) {
        WRITE_KEY(0x45670123u);
        WRITE_KEY(0xcdef89abu);
        if (READ_FLASH_CR() & LOCK) return 0;
    }
    return 1;
}

static uint32_t erase_page(unsigned flags, unsigned page_bytes, uint32_t offset, uint32_t flash_size)
{
    if ((page_bytes != 1024u && page_bytes != 2048u) ||
        offset >= flash_size || (offset & (page_bytes - 1u)) ||
        page_bytes > flash_size - offset)
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_BAD_REQUEST, 0);
    if ((READ_RCC_CR() & 3u) != 3u)
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_CLOCK_OFF, 0);
    if (is_protected(flags))
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_PROTECTED, 0);
    if (wait_status(0, POLL_WRITE) & BSY)
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    if (!unlock_flash())
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_LOCKED, 0);

    WRITE_SR(EOP | ERRORS);
    WRITE_CR(PER);
    WRITE_AR(TARGET_FLASH_BASE + offset);
    WRITE_CR(PER | STRT);

    unsigned sr = wait_status(EOP | ERRORS, POLL_ERASE);
    if (sr & BSY) {
        return make_result(TARGET_RESULT_ERASE_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    }

    unsigned status = TARGET_LOADER_DONE;
    if (sr & ERRORS) {
        status = TARGET_LOADER_PROGRAM_ERROR;
    }
    WRITE_SR(EOP | ERRORS);
    WRITE_CR(LOCK);
    if (!(READ_FLASH_CR() & LOCK)) status = TARGET_LOADER_LOCKED;
    return make_result(TARGET_RESULT_ERASE_TAG, status, 0);
}

static uint32_t program_block(unsigned flags, unsigned count, uint32_t offset, uint32_t flash_size)
{
    if (!count || count > TARGET_LOADER_MAX || ((offset | count) & 1u) ||
        offset >= flash_size || count > flash_size - offset ||
        count > TARGET_VERIFY_CHUNK - (offset & (TARGET_VERIFY_CHUNK - 1u)))
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_BAD_REQUEST, 0);
    if ((READ_RCC_CR() & 3u) != 3u)
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_CLOCK_OFF, 0);
    if (is_protected(flags))
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_PROTECTED, 0);
    if (wait_status(0, POLL_WRITE) & BSY)
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    if (!unlock_flash())
        return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_LOCKED, 0);

    WRITE_SR(EOP | ERRORS);
    WRITE_CR(PG);

    unsigned completed = 0;
    unsigned status = TARGET_LOADER_DONE;
    while (completed != count) {
        if (FLASH_HALF(offset + completed) != 0xffffu) {
            status = TARGET_LOADER_NOT_BLANK;
            break;
        }
        WRITE_HALF(offset + completed, DATA_HALF(completed));
        unsigned sr = wait_status(EOP | ERRORS, POLL_WRITE);
        if (sr & BSY) {
            return make_result(TARGET_RESULT_WRITE_TAG, TARGET_LOADER_BUSY_TIMEOUT, completed);
        }
        if (sr & ERRORS) {
            status = TARGET_LOADER_PROGRAM_ERROR;
            break;
        }
        WRITE_SR(EOP);
        completed += 2;
    }

    WRITE_CR(LOCK);
    if (!(READ_FLASH_CR() & LOCK)) status = TARGET_LOADER_LOCKED;
    return make_result(TARGET_RESULT_WRITE_TAG, status, completed);
}

static uint32_t verify_chunk(unsigned flags, unsigned length, uint32_t offset,
                             uint32_t flash_size, volatile uint32_t *out_crc)
{
    if (length != TARGET_VERIFY_CHUNK || offset >= flash_size ||
        (offset & (TARGET_VERIFY_CHUNK - 1u)) || length > flash_size - offset)
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_BAD_REQUEST, 0);
    if (is_protected(flags))
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_PROTECTED, 0);
    if (wait_status(0, POLL_WRITE) & BSY)
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_BUSY_TIMEOUT, 0);
    if (!(READ_FLASH_CR() & LOCK))
        return make_result(TARGET_RESULT_VERIFY_TAG, TARGET_LOADER_LOCKED, 0);

    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= FLASH_BYTE(offset + i);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0);
    }
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
    unsigned flags = (cmd >> TARGET_FLAG_SHIFT) & 0xfffu;
    unsigned arg0 = cmd & 0xffffu;
    uint32_t status;

    if (((cmd ^ offset ^ flash_size) ^ checksum) != UINT32_MAX ||
        !flash_size || flash_size > 0x100000u) {
        uint32_t tag = op == TARGET_OP_ERASE ? TARGET_RESULT_ERASE_TAG :
                       op == TARGET_OP_VERIFY ? TARGET_RESULT_VERIFY_TAG :
                       TARGET_RESULT_WRITE_TAG;
        status = make_result(tag, TARGET_LOADER_BAD_REQUEST, 0);
    } else if (op == TARGET_OP_WRITE) {
        status = program_block(flags, arg0, offset, flash_size);
    } else if (op == TARGET_OP_ERASE) {
        status = erase_page(flags, arg0, offset, flash_size);
    } else if (op == TARGET_OP_VERIFY) {
        status = verify_chunk(flags, arg0, offset, flash_size, &mailbox[4]);
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
