#include <stdint.h>
#include "link_f103_loader_layout.h"

#ifndef F103_LOADER_TEST
#define REG32(address) (*(volatile uint32_t *)(address))
#define READ_RCC_CR()    REG32(0x40021000u)
#define READ_FLASH_SR()  REG32(0x4002200cu)
#define READ_FLASH_CR()  REG32(0x40022010u)
#define READ_FLASH_OBR() REG32(0x4002201cu)
#define WRITE_KEY(v)     (REG32(0x40022004u) = (v))
#define WRITE_SR(v)      (REG32(0x4002200cu) = (v))
#define WRITE_CR(v)      (REG32(0x40022010u) = (v))
#define FLASH_HALF(off)  (*(volatile uint16_t *)(F103_FLASH_BASE + (off)))
#define FLASH_BYTE(off)  (*(const volatile uint8_t *)(F103_FLASH_BASE + (off)))
#define DATA_HALF(off)   (*(const volatile uint16_t *)(F103_LOADER_DATA + (off)))
#define WRITE_HALF(off, v) (FLASH_HALF(off) = (v))
#endif
#define BSY        0x01u
#define ERRORS     0x14u
#define EOP        0x20u
#define LOCK       0x80u
#define PG         0x01u
#define POLL_LIMIT 65536u

static uint32_t result(unsigned status, unsigned completed)
{
    return F103_RESULT_TAG | (status << 9) | completed;
}

static unsigned wait_status(unsigned mask)
{
    for (unsigned attempts = 0; attempts < POLL_LIMIT; ++attempts) {
        unsigned sr = READ_FLASH_SR();
        if (!(sr & BSY) && (!mask || (sr & mask))) return sr;
    }
    return BSY;
}

static uint32_t program_block(uint32_t request, uint32_t complement)
{
    unsigned offset = request & 0xffffu;
    unsigned count = request >> 16;
    if ((request ^ complement) != UINT32_MAX || !count || count > F103_LOADER_MAX ||
        ((offset | count) & 1u) || count > F103_FLASH_SIZE - offset ||
        count > F103_FLASH_PAGE - (offset & (F103_FLASH_PAGE - 1u)))
        return result(F103_LOADER_BAD_REQUEST, 0);
    if ((READ_RCC_CR() & 3u) != 3u) return result(F103_LOADER_CLOCK_OFF, 0);
    if (READ_FLASH_OBR() & 3u) return result(F103_LOADER_PROTECTED, 0);
    if (wait_status(0) & BSY) return result(F103_LOADER_BUSY_TIMEOUT, 0);
    if (READ_FLASH_CR() & LOCK) {
        WRITE_KEY(0x45670123u);
        WRITE_KEY(0xcdef89abu);
        if (READ_FLASH_CR() & LOCK) return result(F103_LOADER_LOCKED, 0);
    }
    WRITE_SR(EOP | ERRORS);
    WRITE_CR(PG);

    unsigned completed = 0;
    unsigned status = F103_LOADER_DONE;
    while (completed != count) {
        if (FLASH_HALF(offset + completed) != 0xffffu) {
            status = F103_LOADER_NOT_BLANK;
            break;
        }
        WRITE_HALF(offset + completed, DATA_HALF(completed));
        unsigned sr = wait_status(EOP | ERRORS);
        if (sr & BSY) {
            /* The host will confirm idle before it attempts to lock. */
            return result(F103_LOADER_BUSY_TIMEOUT, completed);
        }
        if (sr & ERRORS) {
            status = F103_LOADER_PROGRAM_ERROR;
            break;
        }
        WRITE_SR(EOP);
        completed += 2;
    }

    WRITE_CR(LOCK);
    if (!(READ_FLASH_CR() & LOCK)) status = F103_LOADER_LOCKED;
    return result(status, completed);
}

static uint32_t verify_page(uint32_t request, uint32_t complement, volatile uint32_t *value)
{
    unsigned offset = request & ~F103_REQUEST_VERIFY;
    if ((request ^ complement) != UINT32_MAX || offset > F103_FLASH_SIZE - F103_FLASH_PAGE ||
        (offset & (F103_FLASH_PAGE - 1u))) return F103_VERIFY_TAG | (F103_LOADER_BAD_REQUEST << 9);
    if (READ_FLASH_OBR() & 3u) return F103_VERIFY_TAG | (F103_LOADER_PROTECTED << 9);
    if (wait_status(0) & BSY) return F103_VERIFY_TAG | (F103_LOADER_BUSY_TIMEOUT << 9);
    if (!(READ_FLASH_CR() & LOCK)) return F103_VERIFY_TAG | (F103_LOADER_LOCKED << 9);

    /* Reflected IEEE CRC32, also used by the browser. Read the entire physical
     * Flash page, including its FF holes. No Flash register writes here. */
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < F103_FLASH_PAGE; ++i) {
        crc ^= FLASH_BYTE(offset + i);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0);
    }
    *value = crc ^ UINT32_MAX;
    return F103_VERIFY_TAG;
}

#ifndef F103_LOADER_TEST
__attribute__((used)) static void loader_work(void)
{
    volatile uint32_t *mailbox = (volatile uint32_t *)F103_LOADER_MAILBOX;
    uint32_t request = mailbox[0];
    uint32_t complement = mailbox[1];
    uint32_t status = request & F103_REQUEST_VERIFY ? verify_page(request, complement, &mailbox[2])
                                                    : program_block(request, complement);
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
    F103_LOADER_STACK, (uintptr_t)loader_entry,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception,
    (uintptr_t)loader_exception, 0, 0, 0, 0,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception, 0,
    (uintptr_t)loader_exception, (uintptr_t)loader_exception
};
#endif
