#include "link_ble_programmer.h"
#include "link_config.h"
#include "link_ble.h"
#include "link_session.h"
#include "link_target_db.h"
#include "link_target_loader.h"
#include "target_arm_swd.h"
#include <string.h>

#define DHCSR          0xe000edf0u
#define DEMCR          0xe000edfcu
#define AIRCR          0xe000ed0cu
#define DBGMCU_ID      0xe0042000u

#define F1_FLASH_SR    0x4002200cu
#define F1_FLASH_CR    0x40022010u
#define F1_FLASH_BSY   1u
#define F1_FLASH_LOCK  0x80u

#define F4_FLASH_SR    0x40023c0cu
#define F4_FLASH_CR    0x40023c10u
#define F4_FLASH_BSY   (1u << 16)
#define F4_FLASH_LOCK  (1u << 31)

enum { CMD_INFO = 1, CMD_ATTACH, CMD_ERASE, CMD_WRITE, CMD_READ,
       CMD_RUN, CMD_RELEASE, CMD_ABORT, CMD_DIAG, CMD_CAPS = 12, CMD_PAGE_CRC, CMD_WRITE_PROFILE };
#define CAP_PAGE_CRC      1u
#define CAP_MULTI_TARGET  2u
#define CAP_LOADER_ERASE  4u

enum { RES_OK, RES_PARAM, RES_BUSY, RES_DETACHED, RES_TARGET,
       RES_SWD, RES_FLASH, RES_TIMEOUT, RES_ABORTED };

typedef enum {
    ST_IDLE, ST_ATTACH, ST_HALT, ST_HALT_POLL, ST_DEVICE, ST_SIZE,
#if LINK_S9_FAST_CLOCK
    ST_CHECK_CLOCK_PROFILE,
#endif
    ST_READ,
    ST_LOADER, ST_LOADER_STOP,
    ST_CLEAN_POLL, ST_CLEAN_LOCK, ST_CLEAN_VERIFY,
    ST_RUN_DEMCR_READ, ST_RUN_DEMCR_WRITE, ST_RUN_RESET,
    ST_RUN_WAIT, ST_RUN_RESUME, ST_CANCEL_DRAIN
} programmer_state_t;

static struct {
    programmer_state_t state;
    uint8_t cmd, ready, attached, valid_target, detach_after, suppress;
    uint8_t status;
    uint16_t count, offset, response_len;
    uint32_t address, deadline, detail, dpid, device, demcr;
    const link_target_chip_desc_t *pending_desc;
    link_target_active_t target;
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

static uint8_t target_is_f4(void)
{
    return p.target.desc && p.target.desc->family == LINK_FAMILY_STM32F4;
}

static void fill_attach_response(void)
{
    uint16_t erase_unit = p.target.desc->erase_model == LINK_ERASE_UNIFORM_PAGE
                          ? p.target.desc->page_bytes : 0u;
    put32(p.buffer.response + 6, p.dpid);
    put32(p.buffer.response + 10, p.device);
    p.buffer.response[14] = p.target.desc->family;
    p.buffer.response[15] = p.target.desc->erase_model;
    p.buffer.response[16] = p.target.sector_count;
    p.buffer.response[17] = 0u;
    put32(p.buffer.response + 18, p.target.flash_base);
    put16(p.buffer.response + 22, p.target.flash_kb);
    put16(p.buffer.response + 24, p.target.sram_kb);
    put16(p.buffer.response + 26, erase_unit);
    p.response_len = 28;
}

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
    link_target_loader_reset();
    target_arm_swd_ble_attach_cancel();
    target_arm_swd_ble_mem_reset();
    target_arm_swd_ble_set_flash_bounds(LINK_TARGET_FLASH_BASE, 65536u);
    target_arm_swd_set_clock(250000u);
    target_arm_swd_disconnect();
    link_session_release(LINK_OWNER_BLE);
    p.attached = p.valid_target = 0;
    p.pending_desc = 0;
    memset(&p.target, 0, sizeof(p.target));
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
        deadline(target_is_f4() ? 5000u : 500u);
        p.state = ST_CLEAN_POLL;
    } else finish();
}

static void fail(uint8_t status, uint32_t detail)
{
    record_error(status, detail);
    p.response_len = 6;
    if (p.state == ST_LOADER) {
        if (link_target_loader_stop()) p.state = ST_LOADER_STOP;
        else {
            p.detach_after = 1;
            finish();
        }
        return;
    }
    if (p.state == ST_READ) {
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
    target_arm_swd_ble_set_flash_bounds(LINK_TARGET_FLASH_BASE, 65536u);
    link_target_loader_reset();
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
        (cmd > CMD_DIAG && cmd != CMD_CAPS && cmd != CMD_PAGE_CRC &&
         !(LINK_S9_FAST_CLOCK && cmd == CMD_WRITE_PROFILE))) {
        p.status = RES_PARAM; finish(); return 1;
    }
    if ((cmd != CMD_ERASE && cmd != CMD_WRITE && cmd != CMD_READ &&
         cmd != CMD_PAGE_CRC && cmd != CMD_WRITE_PROFILE) && len != 1) {
        p.status = RES_PARAM; finish(); return 1;
    }
    if (cmd == CMD_INFO) {
        const uint8_t version[] = {0, 4, 2, 2};
        uint16_t max_frame = LINK_BLE_TWO_WRITES ? LINK_BLE_WIDE_FRAME : 20u;
        memcpy(p.buffer.response + 6, version, 4);
        put16(p.buffer.response + 10, 256u);
        put16(p.buffer.response + 12, LINK_TARGET_VERIFY_CHUNK);
        put16(p.buffer.response + 14, max_frame);
        p.response_len = 16; finish(); return 1;
    }
    if (cmd == CMD_CAPS) {
        p.buffer.response[6] = CAP_PAGE_CRC | CAP_MULTI_TARGET | CAP_LOADER_ERASE;
        p.response_len = 7; finish(); return 1;
    }
    if (cmd == CMD_DIAG) {
        memcpy(p.buffer.response + 6, failure_diag, sizeof(failure_diag));
        p.response_len = 6 + sizeof(failure_diag); finish(); return 1;
    }
    if (cmd == CMD_ATTACH) {
        if (!link_session_claim(LINK_OWNER_BLE)) {
            p.status = RES_BUSY; finish(); return 1;
        }
        if (p.attached && p.target.desc) {
            fill_attach_response();
            finish();
        } else {
            memset(failure_diag, 0, sizeof(failure_diag));
            p.pending_desc = 0;
            target_arm_swd_ble_attach_cancel();
            target_arm_swd_ble_mem_reset();
            deadline(2000); p.state = ST_ATTACH;
        }
        return 1;
    }
    if (cmd == CMD_ABORT && link_session_owner() != LINK_OWNER_BLE) {
        finish(); return 1;
    }
    if (!p.attached || !p.target.desc || link_session_owner() != LINK_OWNER_BLE) {
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
        p.buffer.response[6] = len == 3 ? LINK_BLE_WIDE_FRAME : 64u;
        p.state = ST_CHECK_CLOCK_PROFILE;
        return 1;
    }
#endif
    if (cmd == CMD_PAGE_CRC) {
        if (len != 5) { p.status = RES_PARAM; finish(); return 1; }
        p.address = get32(body + 1);
        if (!link_target_db_range_ok(&p.target, p.address, LINK_TARGET_VERIFY_CHUNK) ||
            (p.address & (LINK_TARGET_VERIFY_CHUNK - 1u))) {
            p.status = RES_PARAM; finish(); return 1;
        }
        link_target_loader_verify_begin(&p.target, p.address);
        p.state = ST_LOADER;
        return 1;
    }
    if (cmd == CMD_ERASE) {
        uint32_t unit_bytes = 0;
        uint8_t sector_num = 0;
        if (len != 5) { p.status = RES_PARAM; finish(); return 1; }
        p.address = get32(body + 1);
        if (!link_target_db_erase_unit(&p.target, p.address, &unit_bytes, &sector_num)) {
            p.status = RES_PARAM; finish(); return 1;
        }
        link_target_loader_erase_begin(&p.target, p.address, unit_bytes, sector_num);
        p.state = ST_LOADER;
        return 1;
    }
    if (cmd == CMD_WRITE) {
        if (len < 7 || len > 261 || ((len - 5u) & 1u)) {
            p.status = RES_PARAM; finish(); return 1;
        }
        p.address = get32(body + 1);
        p.count = (uint16_t)(len - 5u);
        if (!link_target_db_write_ok(&p.target, p.address, p.count)) {
            p.status = RES_PARAM; finish(); return 1;
        }
        memcpy(p.buffer.data, body + 5, p.count);
        link_target_loader_write_begin(&p.target, p.address, p.buffer.data, p.count);
        p.state = ST_LOADER;
        return 1;
    }
    if (cmd == CMD_READ) {
        if (len != 7) { p.status = RES_PARAM; finish(); return 1; }
        p.address = get32(body + 1);
        p.count = get16(body + 5);
        if (!p.count || p.count > 256u || !link_target_db_range_ok(&p.target, p.address, p.count)) {
            p.status = RES_PARAM; finish(); return 1;
        }
        p.response_len = (uint16_t)(6u + p.count);
        p.state = ST_READ;
        return 1;
    }
    p.status = RES_PARAM; finish(); return 1;
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
        p.pending_desc = link_target_db_find((uint16_t)(p.device & 0xfffu));
        if (!p.pending_desc) fail(RES_TARGET, p.device);
        else p.state = ST_SIZE;
        break;
    case ST_SIZE: {
        uint32_t reg_addr = p.pending_desc->flash_size_reg;
        uint32_t aligned_addr = reg_addr & ~3u;
        uint16_t flash_kb;
        if (!swd_ok(target_arm_swd_ble_read32(aligned_addr, &value), reg_addr)) break;
        flash_kb = (uint16_t)((reg_addr & 2u) ? (value >> 16) : value);
        if (!link_target_db_init_active(p.pending_desc, flash_kb, &p.target)) {
            fail(RES_TARGET, ((uint32_t)p.pending_desc->dev_id << 16) | flash_kb);
            break;
        }
        target_arm_swd_ble_set_flash_bounds(p.target.flash_base, p.target.flash_bytes);
        p.attached = p.valid_target = 1;
        fill_attach_response();
        finish();
        break;
    }
    case ST_READ:
        if (swd_ok(target_arm_swd_ble_read_flash(p.address, p.buffer.response + 6, p.count), p.address))
            finish();
        break;
    case ST_LOADER: {
        uint32_t detail, crc = 0;
        s = link_target_loader_poll(&detail, &crc);
        if (s == LINK_LOADER_FLASH_ERROR) fail(RES_FLASH, detail);
        else if (s == LINK_LOADER_BAD_STATE) fail(RES_TARGET, detail);
        else if (swd_ok(s, detail)) {
            if (p.cmd == CMD_PAGE_CRC) {
                put32(p.buffer.response + 6, crc);
                p.response_len = 10;
            }
            finish();
        }
        break;
    }
    case ST_LOADER_STOP:
        s = link_target_loader_stop_poll();
        if (s == LINK_SWD_PENDING) break;
        if (s == LINK_TARGET_OK && (p.cmd == CMD_PAGE_CRC || p.status == RES_TARGET)) {
            p.detach_after = 1; finish();
        } else if (s == LINK_TARGET_OK) cleanup(1);
        else {
            record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, DHCSR);
            p.detach_after = 1; finish();
        }
        break;
    case ST_CLEAN_POLL: {
        uint32_t sr_addr = target_is_f4() ? F4_FLASH_SR : F1_FLASH_SR;
        uint32_t bsy_bit = target_is_f4() ? F4_FLASH_BSY : F1_FLASH_BSY;
        s = target_arm_swd_ble_read32(sr_addr, &value);
        if (s == LINK_SWD_PENDING) break;
        if (s != LINK_TARGET_OK || value == 0xffffffffu) {
            record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, sr_addr);
            p.detach_after = 1; finish();
        }
        else if (!(value & bsy_bit)) p.state = ST_CLEAN_LOCK;
        else if (expired()) { record_error(RES_TIMEOUT, sr_addr); p.detach_after = 1; finish(); }
        break;
    }
    case ST_CLEAN_LOCK: {
        uint32_t cr_addr = target_is_f4() ? F4_FLASH_CR : F1_FLASH_CR;
        uint32_t lock_bit = target_is_f4() ? F4_FLASH_LOCK : F1_FLASH_LOCK;
        s = target_arm_swd_ble_write32(cr_addr, lock_bit);
        if (s == LINK_SWD_PENDING) break;
        if (s != LINK_TARGET_OK) { record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, cr_addr); p.detach_after = 1; finish(); }
        else p.state = ST_CLEAN_VERIFY;
        break;
    }
    case ST_CLEAN_VERIFY: {
        uint32_t cr_addr = target_is_f4() ? F4_FLASH_CR : F1_FLASH_CR;
        uint32_t lock_bit = target_is_f4() ? F4_FLASH_LOCK : F1_FLASH_LOCK;
        s = target_arm_swd_ble_read32(cr_addr, &value);
        if (s == LINK_SWD_PENDING) break;
        if (s != LINK_TARGET_OK || !(value & lock_bit)) {
            record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : s != LINK_TARGET_OK ? RES_SWD : RES_FLASH, cr_addr);
            p.detach_after = 1; finish();
        } else if (p.cmd == CMD_RUN && !p.suppress && p.status == RES_OK) p.state = ST_RUN_DEMCR_READ;
        else finish();
        break;
    }
    case ST_RUN_DEMCR_READ:
        if (swd_ok(target_arm_swd_ble_read32(DEMCR, &p.demcr), DEMCR)) p.state = ST_RUN_DEMCR_WRITE;
        break;
    case ST_RUN_DEMCR_WRITE:
        if (swd_ok(target_arm_swd_ble_write32(DEMCR, p.demcr & ~1u), DEMCR)) p.state = ST_RUN_RESET;
        break;
    case ST_RUN_RESET:
        if (swd_ok(target_arm_swd_ble_write32(AIRCR, 0x05fa0004u), AIRCR)) {
            p.valid_target = 0; deadline(20); p.state = ST_RUN_WAIT;
        }
        break;
    case ST_RUN_WAIT:
        if (expired()) p.state = ST_RUN_RESUME;
        break;
    case ST_CANCEL_DRAIN:
        s = target_arm_swd_ble_mem_cancel_poll();
        if (s == LINK_SWD_PENDING) break;
        if (link_target_loader_stop()) {
            if (s != LINK_TARGET_OK) record_error(s == LINK_SWD_TIMEOUT ? RES_TIMEOUT : RES_SWD, p.address);
            p.state = ST_LOADER_STOP;
            break;
        }
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
