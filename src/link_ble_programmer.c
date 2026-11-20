#include "link_ble_programmer.h"
#include "link_config.h"
#include "link_ble.h"
#include "link_session.h"
#include "target_arm_swd.h"
#include <string.h>
#if LINK_BLE_SRAM_WRITE
#include "link_f103_loader.h"
#endif

#define FLASH_BASE 0x08000000u
#define FLASH_SIZE 65536u
#define FLASH_END (FLASH_BASE + FLASH_SIZE)
#define PAGE_SIZE 1024u
#define FLASH_KEYR 0x40022004u
#define FLASH_SR 0x4002200cu
#define FLASH_CR 0x40022010u
#define FLASH_AR 0x40022014u
#define FLASH_LOCK 0x80u
#define FLASH_BSY 1u
#define FLASH_ERRORS 0x14u
#define FLASH_EOP 0x20u
#define DHCSR 0xe000edf0u
#define DEMCR 0xe000edfcu
#define AIRCR 0xe000ed0cu
#define DBGMCU_ID 0xe0042000u
#define FLASH_SIZE_REG 0x1ffff7e0u

enum { CMD_INFO = 1, CMD_ATTACH, CMD_ERASE, CMD_WRITE, CMD_READ,
       CMD_RUN, CMD_RELEASE, CMD_ABORT, CMD_DIAG, CMD_CAPS = 12, CMD_PAGE_CRC, CMD_WRITE_PROFILE };
#define CAP_PAGE_CRC 1u
enum { RES_OK, RES_PARAM, RES_BUSY, RES_DETACHED, RES_TARGET,
       RES_SWD, RES_FLASH, RES_TIMEOUT, RES_ABORTED };
typedef enum {
    ST_IDLE, ST_ATTACH, ST_HALT, ST_HALT_POLL, ST_DEVICE, ST_SIZE,
#if LINK_S9_FAST_CLOCK
    ST_CHECK_CLOCK_PROFILE,
#endif
    ST_UNLOCK_BUSY, ST_UNLOCK_CR, ST_KEY1, ST_KEY2, ST_UNLOCK_VERIFY,
    ST_CLEAR, ST_ERASE_PER, ST_ERASE_AR, ST_ERASE_START,
    ST_WRITE_PG, ST_WRITE_CHECK, ST_WRITE_HALF, ST_FLASH_POLL,
    ST_WRITE_CLEAR, ST_OP_CLEAR_CR, ST_READ,
    ST_CLEAN_POLL, ST_CLEAN_LOCK, ST_CLEAN_VERIFY,
    ST_RUN_DEMCR_READ, ST_RUN_DEMCR_WRITE, ST_RUN_RESET,
    ST_RUN_WAIT, ST_RUN_RESUME, ST_CANCEL_DRAIN
#if LINK_BLE_SRAM_WRITE
    , ST_LOADER, ST_LOADER_STOP, ST_LOADER_CHECK
#endif
} programmer_state_t;

static struct {
    programmer_state_t state;
    uint8_t cmd, ready, attached, valid_target, detach_after, suppress;
    uint8_t status;
    uint16_t count, offset, response_len;
    uint32_t address, deadline, detail, dpid, device, demcr;
    /* A WRITE owns data until completion/cleanup. Only then does finish()
     * construct its response. READ/INFO/DIAG never consume WRITE data.
     * ready prevents the next command from reusing this storage too early. */
    union {
        uint8_t data[256];
        uint8_t response[262];
    } buffer;
} p;
static volatile uint8_t disconnect_pending;
/* Frozen before detach/cleanup overwrites live SWD diagnostics. Pure RAM. */
static uint8_t failure_diag[36];
static void capture_failure(link_target_status_t status);


static uint16_t get16(const uint8_t *b)
{ return (uint16_t)b[0] | ((uint16_t)b[1] << 8); }
static uint32_t get32(const uint8_t *b)
{ return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static void put16(uint8_t *b, uint16_t v)
{ b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v)
{ b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static void deadline(uint32_t ms) { p.deadline = link_ble_now_ms() + ms; }
static uint8_t expired(void)
{ return (int32_t)(link_ble_now_ms() - p.deadline) >= 0; }
static uint8_t range(uint32_t address, uint16_t count)
{ return count && address >= FLASH_BASE && address < FLASH_END &&
         count <= FLASH_END - address; }

static void capture_failure(link_target_status_t status)
{
    target_arm_swd_debug_diag_t wire;
    target_arm_swd_ble_attach_diag_t attach;
    target_arm_swd_get_debug_diag(&wire);
    target_arm_swd_ble_get_attach_diag(&attach);
    failure_diag[0] = 1;
    failure_diag[1] = (uint8_t)p.state;
    failure_diag[2] = attach.step;
    failure_diag[3] = (uint8_t)(status == LINK_TARGET_OK ? wire.dp_status : status);
    failure_diag[4] = wire.dp_ack;
    failure_diag[5] = wire.dp_port;
    failure_diag[6] = wire.dp_rnw;
    failure_diag[7] = wire.dp_addr;
    failure_diag[8] = wire.turnaround;
    failure_diag[9] = wire.write_turnaround;
    failure_diag[10] = wire.mem_stage;
    failure_diag[11] = 1;
    put32(failure_diag + 12, p.detail);
    put32(failure_diag + 16, attach.idcode);
    put32(failure_diag + 20, attach.ctrl_stat);
    put32(failure_diag + 24, attach.csw);
    put32(failure_diag + 28, wire.dp_data);
    put32(failure_diag + 32, wire.mem_detail);
}

static void detach(void)
{
#if LINK_BLE_SRAM_WRITE
    link_f103_loader_reset();
#endif
    target_arm_swd_ble_attach_cancel();
    target_arm_swd_ble_mem_reset();
    target_arm_swd_disconnect();
    link_session_release(LINK_OWNER_BLE);
    p.attached = p.valid_target = 0;
}

static void finish(void)
{
    if (p.detach_after && link_session_owner() == LINK_OWNER_BLE) detach();
    p.buffer.response[0] = p.cmd | 0x80u;
    p.buffer.response[1] = p.status;
    put32(p.buffer.response + 2, p.detail);
    if (p.status != RES_OK) p.response_len = 6;
    p.ready = !p.suppress;
    p.state = ST_IDLE;
}

/* Preserve the first failure. Cleanup never turns a failure into success. */
static void record_error(uint8_t status, uint32_t detail)
{
    if (p.status == RES_OK) {
        p.status = status; p.detail = detail;
        capture_failure(LINK_TARGET_OK);
    }
}

static void cleanup(uint8_t detach_after)
{
    p.detach_after = detach_after;
    if (p.valid_target && link_session_owner() == LINK_OWNER_BLE) {
        deadline(500);
        p.state = ST_CLEAN_POLL;
    } else finish();
}

static void fail(uint8_t status, uint32_t detail)
{
    record_error(status, detail);
    p.response_len = 6;
#if LINK_BLE_SRAM_WRITE
    if (p.state == ST_LOADER || p.state == ST_LOADER_CHECK) {
        if (link_f103_loader_stop()) p.state = ST_LOADER_STOP;
        else {
            p.detach_after = 1;
            finish();
        }
        return;
    }
#endif
    if (p.state == ST_READ) {
        /* READ did not unlock Flash. A failed posted read can leave the
         * debug pipeline uncertain, so release it and require fresh attach. */
        p.detach_after = 1;
        finish();
    } else {
        cleanup(1);
    }
}

static uint8_t swd_ok(link_target_status_t s, uint32_t addr)
{
    if (s == LINK_TARGET_OK) return 1;
    if (s == LINK_SWD_PENDING) return 0;
    if (s == LINK_SWD_TIMEOUT) {
        fail(RES_TIMEOUT, addr);
        return 0;
    }
    if (p.status == RES_OK) {
        p.status = RES_SWD; p.detail = addr;
        capture_failure(s);
    }
    fail(RES_SWD, addr);
    return 0;
}

void link_ble_programmer_init(void)
{
    memset(&p, 0, sizeof(p));
    disconnect_pending = 0;
    target_arm_swd_ble_mem_reset();
#if LINK_BLE_SRAM_WRITE
    link_f103_loader_reset();
#endif
    memset(failure_diag, 0, sizeof(failure_diag));
}

void link_ble_programmer_disconnect(void)
{
    disconnect_pending = 1;
}

uint8_t link_ble_programmer_start(const uint8_t *body, uint16_t len)
{
    uint8_t cmd;
    if (p.state != ST_IDLE || p.ready || disconnect_pending) return 0;
    p.cmd = (body && len) ? body[0] : 0;
    p.status = RES_OK; p.detail = 0; p.suppress = 0;
    p.detach_after = 0; p.response_len = 6; p.offset = 0;
    cmd = p.cmd;
    if (!body || !len || cmd < CMD_INFO ||
        (cmd > CMD_DIAG && !(LINK_BLE_SRAM_WRITE && (cmd == CMD_CAPS || cmd == CMD_PAGE_CRC)) &&
         !(LINK_S9_FAST_CLOCK && cmd == CMD_WRITE_PROFILE))) {
        p.status = RES_PARAM; finish(); return 1;
    }
    if ((cmd != CMD_ERASE && cmd != CMD_WRITE && cmd != CMD_READ && cmd != CMD_PAGE_CRC && cmd != CMD_WRITE_PROFILE) && len != 1) {
        p.status = RES_PARAM; finish(); return 1;
    }
    if (cmd == CMD_INFO) {
#if LINK_BLE_SRAM_WRITE
        const uint8_t version[] = {0, 3, LINK_BLE_TWO_WRITES ? 17 : LINK_S9_FAST_CLOCK ? 16 : LINK_USB_DRW_WORD_IO ? 15 : 14, 1};
#else
        const uint8_t version[] = {0, 3, 10, 1};
#endif
        memcpy(p.buffer.response + 6, version, 4);
        put16(p.buffer.response + 10, 256); put16(p.buffer.response + 12, PAGE_SIZE);
        put32(p.buffer.response + 14, FLASH_BASE); put32(p.buffer.response + 18, FLASH_SIZE);
        p.response_len = 22; finish(); return 1;
    }
#if LINK_BLE_SRAM_WRITE
    if (cmd == CMD_CAPS) {
        p.buffer.response[6] = CAP_PAGE_CRC;
        p.response_len = 7; finish(); return 1;
    }
#endif
    if (cmd == CMD_DIAG) {
        memcpy(p.buffer.response + 6, failure_diag, sizeof(failure_diag));
        p.response_len = 6 + sizeof(failure_diag); finish(); return 1;
    }
    if (cmd == CMD_ATTACH) {
        if (!link_session_claim(LINK_OWNER_BLE)) {
            p.status = RES_BUSY; finish(); return 1;
        }
        if (p.attached) {
            /* Already attached: return its verified identity without resync. */
            put32(p.buffer.response + 6, p.dpid); put32(p.buffer.response + 10, p.device);
            put16(p.buffer.response + 14, 64); p.response_len = 16; finish();
        } else {
            memset(failure_diag, 0, sizeof(failure_diag));
            target_arm_swd_ble_attach_cancel();
            target_arm_swd_ble_mem_reset();
            deadline(2000); p.state = ST_ATTACH;
        }
        return 1;
    }
    if (cmd == CMD_ABORT && link_session_owner() != LINK_OWNER_BLE) {
        finish(); return 1;
    }
    if (!p.attached || link_session_owner() != LINK_OWNER_BLE) {
        p.status = RES_DETACHED; finish(); return 1;
    }
    if (cmd == CMD_RELEASE || cmd == CMD_ABORT || cmd == CMD_RUN) {
        cleanup(cmd == CMD_RUN ? 0 : 1); return 1;
    }
#if LINK_S9_FAST_CLOCK
    if (cmd == CMD_WRITE_PROFILE) {
        if ((len != 2 && !(LINK_BLE_TWO_WRITES && len == 3 && body[2] == LINK_BLE_WIDE_FRAME)) ||
            (body[1] != 0 && body[1] != 2 && body[1] != 5)) {
            p.status = RES_PARAM; finish(); return 1;
        }
        target_arm_swd_set_clock(body[1] ? body[1] * 1000000u : 250000u);
        /* Older R6 clients validate this field against their 64-byte limit. */
        p.buffer.response[6] = len == 3 ? LINK_BLE_WIDE_FRAME : 64u;
        p.state = ST_CHECK_CLOCK_PROFILE;
        return 1;
    }
#endif
#if LINK_BLE_SRAM_WRITE
    if (cmd == CMD_PAGE_CRC) {
        if (len != 5) { p.status = RES_PARAM; finish(); return 1; }
        p.address = get32(body + 1);
        if (!range(p.address, PAGE_SIZE) || (p.address & (PAGE_SIZE - 1u))) {
            p.status = RES_PARAM; finish(); return 1;
        }
        link_f103_loader_verify_begin(p.address);
        p.state = ST_LOADER;
        return 1;
    }
#endif
    if ((cmd == CMD_ERASE && len != 5) ||
        (cmd == CMD_WRITE && (len < 7 || len > 261 || ((len - 5u) & 1u))) ||
        (cmd == CMD_READ && len != 7)) {
        p.status = RES_PARAM; finish(); return 1;
    }
    p.address = get32(body + 1);
    p.count = cmd == CMD_ERASE ? PAGE_SIZE :
              cmd == CMD_READ ? get16(body + 5) : len - 5;
    if (!range(p.address, p.count) ||
        (cmd == CMD_ERASE && (p.address & (PAGE_SIZE - 1u))) ||
        (cmd != CMD_ERASE && p.count > 256) ||
        (cmd == CMD_WRITE && ((p.address & 1u) ||
         (p.address & (PAGE_SIZE - 1u)) + p.count > PAGE_SIZE))) {
        p.status = RES_PARAM; finish(); return 1;
    }
    if (cmd == CMD_READ) {
        p.response_len = 6 + p.count; p.state = ST_READ;
    } else {
        if (cmd == CMD_WRITE) memcpy(p.buffer.data, body + 5, p.count);
#if LINK_BLE_SRAM_WRITE
        if (cmd == CMD_WRITE) {
            link_f103_loader_begin(p.address, p.buffer.data, p.count);
            p.state = ST_LOADER;
        } else {
            link_f103_loader_check_begin(p.address);
            p.state = ST_LOADER_CHECK;
        }
#else
        deadline(500); p.state = ST_UNLOCK_BUSY;
#endif
    }
    return 1;
}

void link_ble_programmer_poll(void)
{
    uint32_t value = 0;
    link_target_status_t s;
    if (disconnect_pending) {
        disconnect_pending = 0;
        p.ready = 0; p.suppress = 1;
        target_arm_swd_ble_attach_cancel();
        if (link_session_owner() == LINK_OWNER_BLE) {
            target_arm_swd_ble_mem_cancel();
            p.state = ST_CANCEL_DRAIN;
        }
        else p.state = ST_IDLE;
        return;
    }
    switch (p.state) {
    case ST_IDLE: return;
#if LINK_S9_FAST_CLOCK
    case ST_CHECK_CLOCK_PROFILE:
        s = target_arm_swd_ble_read32(DHCSR, &value);
        if (s == LINK_SWD_PENDING) break;
        /* S_RESET_ST is clear-on-read. A reset invalidates the cached
         * loader and CPU context even when the core has halted again. */
        if (s != LINK_TARGET_OK || !(value & (1u << 17)) || !(value & 1u) ||
            (value & ((1u << 25) | (1u << 19)))) {
            target_arm_swd_set_clock(250000u);
            p.detach_after = 1;
            p.status = s == LINK_TARGET_OK ? RES_TARGET : RES_SWD;
            p.detail = DHCSR;
        } else {
            uint8_t available = link_ble_rx_frame_limit();
            if (available < p.buffer.response[6]) p.buffer.response[6] = available;
            p.response_len = 7;
        }
        finish();
        break;
#endif
    case ST_ATTACH:
        s = target_arm_swd_ble_attach(&p.dpid);
        if (s == LINK_TARGET_WAIT) {
            if (expired()) fail(RES_TIMEOUT, 0);
        } else if (swd_ok(s, 0)) {
            if ((p.dpid & 0x0fffffffu) != 0x0ba01477u) fail(RES_TARGET, p.dpid);
            else p.state = ST_HALT;
        }
        break;
    case ST_HALT:
        if (swd_ok(target_arm_swd_ble_write32(DHCSR, 0xa05f0003u), DHCSR)) {
            deadline(1000); p.state = ST_HALT_POLL;
        }
        break;
    case ST_HALT_POLL:
        if (!swd_ok(target_arm_swd_ble_read32(DHCSR, &value), DHCSR)) break;
        if (value & (1u << 17)) p.state = ST_DEVICE;
        else if (expired()) fail(RES_TIMEOUT, DHCSR);
        break;
    case ST_DEVICE:
        if (!swd_ok(target_arm_swd_ble_read32(DBGMCU_ID, &p.device), DBGMCU_ID)) break;
        if ((p.device & 0xfffu) != 0x410u) fail(RES_TARGET, DBGMCU_ID);
        else p.state = ST_SIZE;
        break;
    case ST_SIZE:
        if (!swd_ok(target_arm_swd_ble_read32(FLASH_SIZE_REG, &value), FLASH_SIZE_REG)) break;
        if ((value & 0xffffu) != 64u) { fail(RES_TARGET, FLASH_SIZE_REG); break; }
        p.attached = p.valid_target = 1;
        put32(p.buffer.response + 6, p.dpid); put32(p.buffer.response + 10, p.device);
        put16(p.buffer.response + 14, 64); p.response_len = 16; finish();
        break;
    case ST_UNLOCK_BUSY:
        if (!swd_ok(target_arm_swd_ble_read32(FLASH_SR, &value), FLASH_SR)) break;
        if (!(value & FLASH_BSY)) p.state = ST_UNLOCK_CR;
        else if (expired()) fail(RES_TIMEOUT, p.address);
        break;
    case ST_UNLOCK_CR:
        if (swd_ok(target_arm_swd_ble_read32(FLASH_CR, &value), FLASH_CR))
            p.state = value & FLASH_LOCK ? ST_KEY1 : ST_CLEAR;
        break;
    case ST_KEY1:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_KEYR, 0x45670123u), FLASH_KEYR)) p.state = ST_KEY2;
        break;
    case ST_KEY2:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_KEYR, 0xcdef89abu), FLASH_KEYR)) p.state = ST_UNLOCK_VERIFY;
        break;
    case ST_UNLOCK_VERIFY:
        if (!swd_ok(target_arm_swd_ble_read32(FLASH_CR, &value), FLASH_CR)) break;
        if (value & FLASH_LOCK) fail(RES_FLASH, p.address);
        else p.state = ST_CLEAR;
        break;
    case ST_CLEAR:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_SR, FLASH_EOP | FLASH_ERRORS), FLASH_SR))
            p.state = p.cmd == CMD_ERASE ? ST_ERASE_PER : ST_WRITE_PG;
        break;
    case ST_ERASE_PER:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_CR, 2), FLASH_CR)) p.state = ST_ERASE_AR;
        break;
    case ST_ERASE_AR:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_AR, p.address), FLASH_AR)) p.state = ST_ERASE_START;
        break;
    case ST_ERASE_START:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_CR, 0x42), FLASH_CR)) {
            deadline(500); p.state = ST_FLASH_POLL;
        }
        break;
    case ST_WRITE_PG:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_CR, 1), FLASH_CR)) p.state = ST_WRITE_CHECK;
        break;
    case ST_WRITE_CHECK:
        if (!swd_ok(target_arm_swd_ble_read32((p.address + p.offset) & ~3u, &value), p.address + p.offset)) break;
        if (((value >> (((p.address + p.offset) & 2u) * 8u)) & 0xffffu) != 0xffffu) {
            fail(RES_FLASH, p.address + p.offset); break;
        }
        p.state = ST_WRITE_HALF;
        break;
    case ST_WRITE_HALF:
        if (swd_ok(target_arm_swd_ble_write16(p.address + p.offset, get16(p.buffer.data + p.offset)), p.address + p.offset)) {
            deadline(100); p.state = ST_FLASH_POLL;
        }
        break;
    case ST_FLASH_POLL:
        if (!swd_ok(target_arm_swd_ble_read32(FLASH_SR, &value), p.address + p.offset)) break;
        if (!(value & FLASH_BSY)) {
            if (value & FLASH_ERRORS) fail(RES_FLASH, p.address + p.offset);
            else if (value & FLASH_EOP) p.state = ST_WRITE_CLEAR;
            else if (expired()) fail(RES_TIMEOUT, p.address + p.offset);
        } else if (expired()) fail(RES_TIMEOUT, p.address + p.offset);
        break;
    case ST_WRITE_CLEAR:
        if (!swd_ok(target_arm_swd_ble_write32(FLASH_SR, FLASH_EOP | FLASH_ERRORS), FLASH_SR)) break;
        if (p.cmd == CMD_WRITE && (p.offset += 2) < p.count) p.state = ST_WRITE_CHECK;
        else p.state = ST_OP_CLEAR_CR;
        break;
    case ST_OP_CLEAR_CR:
        if (swd_ok(target_arm_swd_ble_write32(FLASH_CR, FLASH_LOCK), FLASH_CR)) finish();
        break;
    case ST_READ:
        if (swd_ok(target_arm_swd_ble_read_flash(p.address, p.buffer.response + 6, p.count), p.address))
            finish();
        break;
#if LINK_BLE_SRAM_WRITE
    case ST_LOADER:
    case ST_LOADER_CHECK: {
        uint32_t detail, crc = 0;
        s = link_f103_loader_poll(&detail, &crc);
        if (s == LINK_F103_FLASH_ERROR) fail(RES_FLASH, detail);
        else if (s == LINK_F103_BAD_STATE) fail(RES_TARGET, detail);
        else if (swd_ok(s, detail)) {
            if (p.state == ST_LOADER_CHECK) {
                deadline(500); p.state = ST_UNLOCK_BUSY;
            } else {
                if (p.cmd == CMD_PAGE_CRC) {
                    put32(p.buffer.response + 6, crc);
                    p.response_len = 10;
                }
                finish();
            }
        }
        break;
    }
    case ST_LOADER_STOP:
        s = link_f103_loader_stop_poll();
        if (s == LINK_SWD_PENDING) break;
        if (s == LINK_TARGET_OK && p.cmd == CMD_PAGE_CRC) { p.detach_after = 1; finish(); }
        else if (s == LINK_TARGET_OK) cleanup(1);
        else {
            record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, DHCSR);
            p.detach_after = 1; finish();
        }
        break;
#endif
    case ST_CLEAN_POLL:
        s = target_arm_swd_ble_read32(FLASH_SR, &value);
        if (s == LINK_SWD_PENDING) break;
        if (s != LINK_TARGET_OK) { record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, FLASH_SR); p.detach_after = 1; finish(); }
        else if (!(value & FLASH_BSY)) p.state = ST_CLEAN_LOCK;
        else if (expired()) { record_error(RES_TIMEOUT, FLASH_SR); p.detach_after = 1; finish(); }
        break;
    case ST_CLEAN_LOCK:
        s = target_arm_swd_ble_write32(FLASH_CR, FLASH_LOCK);
        if (s == LINK_SWD_PENDING) break;
        if (s != LINK_TARGET_OK) { record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, FLASH_CR); p.detach_after = 1; finish(); }
        else p.state = ST_CLEAN_VERIFY;
        break;
    case ST_CLEAN_VERIFY:
        s = target_arm_swd_ble_read32(FLASH_CR, &value);
        if (s == LINK_SWD_PENDING) break;
        if (s != LINK_TARGET_OK || !(value & FLASH_LOCK)) {
            record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : s != LINK_TARGET_OK ? RES_SWD : RES_FLASH, FLASH_CR);
            p.detach_after = 1; finish();
        } else if (p.cmd == CMD_RUN && !p.suppress && p.status == RES_OK) p.state = ST_RUN_DEMCR_READ;
        else finish();
        break;
    case ST_RUN_DEMCR_READ:
        if (swd_ok(target_arm_swd_ble_read32(DEMCR, &p.demcr), DEMCR)) p.state = ST_RUN_DEMCR_WRITE;
        break;
    case ST_RUN_DEMCR_WRITE:
        if (swd_ok(target_arm_swd_ble_write32(DEMCR, p.demcr & ~1u), DEMCR)) p.state = ST_RUN_RESET;
        break;
    case ST_RUN_RESET:
        if (swd_ok(target_arm_swd_ble_write32(AIRCR, 0x05fa0004u), AIRCR)) {
            /* Reset may release the halt; Flash is already locked. */
            p.valid_target = 0; deadline(20); p.state = ST_RUN_WAIT;
        }
        break;
    case ST_RUN_WAIT:
        if (expired()) p.state = ST_RUN_RESUME;
        break;
    case ST_CANCEL_DRAIN:
        s = target_arm_swd_ble_mem_cancel_poll();
        if (s == LINK_SWD_PENDING) break;
#if LINK_BLE_SRAM_WRITE
        if (link_f103_loader_stop()) {
            if (s != LINK_TARGET_OK) record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, p.address);
            p.state = ST_LOADER_STOP;
            break;
        }
#endif
        if (s == LINK_TARGET_OK && p.cmd == CMD_PAGE_CRC) { p.detach_after = 1; finish(); }
        else if (s == LINK_TARGET_OK) cleanup(1);
        else {
            record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, p.address);
            p.detach_after = 1; finish();
        }
        break;
    case ST_RUN_RESUME:
        s = target_arm_swd_ble_write32(DHCSR, 0xa05f0000u);
        if (s == LINK_SWD_PENDING) break;
        if (s != LINK_TARGET_OK) record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, DHCSR);
        p.detach_after = 1; finish();
        break;
    }
}

uint8_t link_ble_programmer_response(uint8_t *body, uint16_t *len)
{
    if (!p.ready || !body || !len || disconnect_pending) return 0;
    memcpy(body, p.buffer.response, p.response_len);
    *len = p.response_len; p.ready = 0;
    return 1;
}
