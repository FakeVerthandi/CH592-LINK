#include "link_config.h"
#include "link_perf.h"
#include "CH59x_common.h"

#include "link_board.h"
#include "link_swd_delay.h"
#include "target_arm_swd.h"

#if LINK_BLE_ENABLE
#include "link_ble.h"
#include "link_target_loader_layout.h"
#if LINK_BLE_SWD_WORD_IO
#include "link_session.h"
#endif
#define LINK_SWD_IRQ_SAVE(p) (*(p)=0u)
#define LINK_SWD_LOCAL
#define LINK_SWD_IRQ_RESTORE(v) ((void)(v))
#else
#define LINK_SWD_IRQ_SAVE(p) SYS_DisableAllIrq(p)
#define LINK_SWD_LOCAL static
#define LINK_SWD_IRQ_RESTORE(v) SYS_RecoverIrq(v)
#endif

#define ARM_SWD_ACK_OK     0x01
#define ARM_SWD_ACK_WAIT   0x02
#define ARM_SWD_ACK_FAULT  0x04
#define ARM_SWD_DP_ABORT_CLR_STKCMP   0x00000002u
#define ARM_SWD_DP_ABORT_CLR_STKERR   0x00000004u
#define ARM_SWD_DP_ABORT_CLR_WDATAERR 0x00000008u
#define ARM_SWD_DP_ABORT_CLR_ORUNERR  0x00000010u
#define ARM_SWD_DP_ABORT_CLR_ALL \
    (ARM_SWD_DP_ABORT_CLR_STKCMP | ARM_SWD_DP_ABORT_CLR_STKERR | \
     ARM_SWD_DP_ABORT_CLR_WDATAERR | ARM_SWD_DP_ABORT_CLR_ORUNERR)
#define ARM_SWD_DP_ABORT_DAP_AND_CLR_ALL  0x0000001Fu
#define ARM_SWD_DP_CTRL_STAT      0x04
#define ARM_SWD_DP_SELECT         0x08
#define ARM_SWD_DP_RDBUFF         0x0C
#define ARM_SWD_DP_SELECT_APBANKSEL_MASK 0x000000F0u
#define ARM_SWD_DP_POWER_REQ      0x50000000u
#define ARM_SWD_DP_POWER_ACK      0xA0000000u
#define ARM_SWD_DP_CTRL_STICKYORUN 0x00000002u
#define ARM_SWD_DP_CTRL_STICKYCMP  0x00000010u
#define ARM_SWD_DP_CTRL_STICKYERR  0x00000020u
#define ARM_SWD_DP_CTRL_WDATAERR   0x00000080u
#define ARM_SWD_DP_CTRL_STICKY_MASK \
    (ARM_SWD_DP_CTRL_STICKYORUN | ARM_SWD_DP_CTRL_STICKYCMP | \
     ARM_SWD_DP_CTRL_STICKYERR | ARM_SWD_DP_CTRL_WDATAERR)
#define ARM_SWD_AP_CSW            0x00
#define ARM_SWD_AP_TAR            0x04
#define ARM_SWD_AP_BASE           0x08
#define ARM_SWD_AP_DRW            0x0C
#define ARM_SWD_AP_CSW_SIZE_MASK       0x00000007u
#define ARM_SWD_AP_CSW_SIZE_16         0x00000001u
#define ARM_SWD_AP_CSW_SIZE_32         0x00000002u
#define ARM_SWD_AP_CSW_ADDRINC_MASK    0x00000030u
#define ARM_SWD_AP_CSW_ADDRINC_SINGLE  0x00000010u
#define ARM_SWD_AP_CSW_ADDRINC_PACKED  0x00000020u
#define ARM_SWD_AP_CSW_VERIFY_MASK     0x00000077u
#define ARM_SWD_MEM_AP_CSW_32     0x23000052u
#define ARM_SWD_MEM_AP_CSW_16     0x23000051u
#define ARM_SWD_MEM_AP_BANK0      0x00000000u
#define ARM_SWD_MEM_AP_BANK1      0x00000010u
#define ARM_SWD_MEM_AP_BANKF      0x000000F0u
#define ARM_SWD_MEM_AP_BASE_FALLBACK 0xE00FF003u
#define ARM_SWD_MEM_AP_IDR_FALLBACK  0x14770011u
#define ARM_SWD_CORTEX_M3_ROM_BASE   0xE00FF000u
#define ARM_SWD_CORTEX_M3_ROM_END    0xE0100000u
#define ARM_SWD_CORTEX_M3_ID_OFFSET_BASE 0x00000FD0u
#define ARM_SWD_CORTEX_M3_ID_OFFSET_END  0x00001000u
#define ARM_SWD_CORTEX_M3_CPUID       0xE000ED00u
#define ARM_SWD_CORTEX_M3_CPUID_VALUE 0x411FC231u
#define STM32F1_FLASH_BASE        0x08000000u
#define STM32F103C8_FLASH_SIZE    (64u * 1024u)
#define STM32F103C8_FLASH_END     (STM32F1_FLASH_BASE + STM32F103C8_FLASH_SIZE)
#define STM32F1_FLASH_PAGE_SIZE   1024u
#define STM32F1_FLASH_KEYR        0x40022004u
#define STM32F1_FLASH_SR          0x4002200Cu
#define STM32F1_FLASH_CR          0x40022010u
#define STM32F1_FLASH_AR          0x40022014u
#define STM32F1_FLASH_KEY1        0x45670123u
#define STM32F1_FLASH_KEY2        0xCDEF89ABu
#define STM32F1_FLASH_SR_BSY      0x00000001u
#define STM32F1_FLASH_SR_EOP      0x00000020u
#define STM32F1_FLASH_SR_ERRS     0x00000014u
#define STM32F1_FLASH_CR_PG       0x00000001u
#define STM32F1_FLASH_CR_PER      0x00000002u
#define STM32F1_FLASH_CR_STRT     0x00000040u
#define STM32F1_FLASH_CR_LOCK     0x00000080u
#define ARM_SWD_READ_RETRIES        2
#define ARM_SWD_CONNECT_SETTLE_MS   20
#define ARM_SWD_RESET_HOLD_MS       20
#define ARM_SWD_RESET_SETTLE_MS     50
#define ARM_SWD_LINE_RESET_CYCLES   64
#define ARM_SWD_IDLE_CYCLES         4
#define ARM_SWD_POST_ABORT_IDLE_CYCLES 32
#define ARM_SWD_ATTACH_RESET_HOLD_MS    5
#define ARM_SWD_ATTACH_RELEASE_DELAY_MS 15
#define ARM_SWD_POST_CONNECT_DELAY_MS   5
#define ARM_SWD_MAX_TURNAROUND_CYCLES   4
/* Include 0 as a deliberate phase probe: stable 0x04 on DPIDR/ABORT can be an OK ACK sampled one bit late. */
#define ARM_SWD_MIN_TURNAROUND_CYCLES   0
#define ARM_SWD_MAX_WRITE_DATA_TURNAROUND_CYCLES 2

#define ARM_SWD_ATTACH_PLAIN        0
#define ARM_SWD_ATTACH_UNDER_RESET  1

#define ARM_SWD_SYNC_FULL           0
#define ARM_SWD_SYNC_LINE_RESET     1

#define ARM_SWD_SAMPLE_LATE         0
#define ARM_SWD_SAMPLE_EARLY        1

#define ARM_SWD_DAP_TRANSFER_APNDP       0x01
#define ARM_SWD_DAP_TRANSFER_RNW         0x02
#define ARM_SWD_DAP_TRANSFER_A2          0x04
#define ARM_SWD_DAP_TRANSFER_A3          0x08

typedef struct {
    uint8_t delay_us;
    uint8_t sync_mode;
    uint8_t sample_mode;
} arm_swd_strategy_t;

static const arm_swd_strategy_t arm_swd_strategies[] = {
    { 2,  ARM_SWD_SYNC_FULL,       ARM_SWD_SAMPLE_EARLY },
    { 5,  ARM_SWD_SYNC_FULL,       ARM_SWD_SAMPLE_EARLY },
    { 10, ARM_SWD_SYNC_FULL,       ARM_SWD_SAMPLE_EARLY },
    { 20, ARM_SWD_SYNC_FULL,       ARM_SWD_SAMPLE_EARLY },
    { 5,  ARM_SWD_SYNC_FULL,       ARM_SWD_SAMPLE_LATE  },
    { 10, ARM_SWD_SYNC_FULL,       ARM_SWD_SAMPLE_LATE  },
    { 5,  ARM_SWD_SYNC_LINE_RESET, ARM_SWD_SAMPLE_EARLY },
    { 10, ARM_SWD_SYNC_LINE_RESET, ARM_SWD_SAMPLE_EARLY }
};

static uint8_t arm_swd_last_ack;
static uint8_t arm_swd_last_attempts;
static uint8_t arm_swd_last_dio_read_high;
static uint8_t arm_swd_last_dio_read_low;
static uint8_t arm_swd_last_dio_read_release;
static uint8_t arm_swd_last_strategy;
static uint8_t arm_swd_last_turnaround_cycles;
static uint8_t arm_swd_last_attach_mode;
static uint8_t arm_swd_last_ack_window;
static uint8_t arm_swd_last_mem_stage;
static uint32_t arm_swd_last_mem_detail;
static uint8_t arm_swd_half_cycle_delay_us;
static uint8_t arm_swd_sample_mode;
static uint8_t arm_swd_sync_mode;
static uint8_t arm_swd_turnaround_cycles;
static uint8_t arm_swd_write_data_turnaround_cycles;
/* DAP mode is opt-in: legacy private-protocol phase scans remain unchanged. */
static uint8_t arm_swd_dap_calibration_enabled;
static uint8_t arm_swd_dap_write_phase_valid;
static uint8_t arm_swd_dap_write_phase;
static uint8_t arm_swd_dap_power_ready;
static uint8_t arm_swd_dap_select_valid;
static uint8_t arm_swd_dap_csw_valid;
static uint8_t arm_swd_dp_ready;
static uint8_t arm_swd_pins;
static uint8_t arm_swd_last_dp_port;
static uint8_t arm_swd_last_dp_rnw;
static uint8_t arm_swd_last_dp_addr;
static uint8_t arm_swd_last_dp_status;
static uint8_t arm_swd_last_dp_ack;
static uint8_t arm_swd_last_dp_request;
static uint32_t arm_swd_dp_select;
static uint32_t arm_swd_mem_ap_tar;
static uint32_t arm_swd_mem_ap_csw;
static uint32_t arm_swd_last_dp_data;
static uint32_t arm_swd_last_ap_read_tar;
static uint8_t arm_swd_synthetic_rdbuff_valid;
static uint32_t arm_swd_synthetic_rdbuff_data;
static uint8_t arm_swd_last_ap_post_addr;
static uint8_t arm_swd_last_ap_post_status;
static uint8_t arm_swd_last_ap_post_ack;
static uint8_t arm_swd_last_ap_post_request;
static uint32_t arm_swd_last_ap_post_data;

typedef enum {
    ARM_SWD_MEM_STAGE_NONE = 0,
    ARM_SWD_MEM_STAGE_READ_ID = 1,
    ARM_SWD_MEM_STAGE_ABORT = 2,
    ARM_SWD_MEM_STAGE_POWER_REQ = 3,
    ARM_SWD_MEM_STAGE_POWER_ACK = 4,
    ARM_SWD_MEM_STAGE_SELECT = 5,
    ARM_SWD_MEM_STAGE_CSW = 6,
    ARM_SWD_MEM_STAGE_TAR = 7,
    ARM_SWD_MEM_STAGE_DRW = 8,
    ARM_SWD_MEM_STAGE_RDBUFF = 9,
    ARM_SWD_MEM_STAGE_FLASH_UNLOCK = 10,
    ARM_SWD_MEM_STAGE_FLASH_WAIT = 11,
    ARM_SWD_MEM_STAGE_FLASH_CR = 12,
    ARM_SWD_MEM_STAGE_FLASH_AR = 13,
    ARM_SWD_MEM_STAGE_FLASH_WRITE16 = 14
} arm_swd_mem_stage_t;

LINK_SWD_LOCAL link_target_status_t target_arm_swd_write16(uint32_t addr, uint16_t value);
static void arm_swd_dap_invalidate_context(void)
{
    arm_swd_dap_power_ready = 0;
    arm_swd_dap_select_valid = 0;
    arm_swd_dap_csw_valid = 0;
}

static uint8_t arm_swd_make_request(uint8_t ap_ndp, uint8_t rnw, uint8_t addr);
static link_target_status_t arm_swd_dap_write_ap_with_retry(uint8_t addr, uint32_t value);
static uint8_t arm_swd_dap_bank1_data_addr(uint8_t ap_addr, uint32_t *addr);
static link_target_status_t arm_swd_dap_bank1_read32(uint8_t ap_addr, uint32_t *value);
static link_target_status_t arm_swd_dap_bank1_write32(uint8_t ap_addr, uint32_t value);

static void arm_swd_set_mem_diag(uint8_t stage, uint32_t detail)
{
    arm_swd_last_mem_stage = stage;
    arm_swd_last_mem_detail = detail;
}

static void arm_swd_set_transfer_diag(uint8_t ap_ndp, uint8_t rnw, uint8_t addr,
                                      uint8_t ack, link_target_status_t status,
                                      uint32_t data)
{
    if ((status != LINK_TARGET_OK) && arm_swd_dap_calibration_enabled) {
        arm_swd_dap_invalidate_context();
    }
    arm_swd_last_dp_port = ap_ndp;
    arm_swd_last_dp_rnw = rnw;
    arm_swd_last_dp_addr = (uint8_t)(addr & 0x0Cu);
    arm_swd_last_dp_status = (uint8_t)status;
    arm_swd_last_dp_ack = ack;
    arm_swd_last_dp_request = arm_swd_make_request(ap_ndp, rnw, addr);
    arm_swd_last_dp_data = data;
}

static void arm_swd_capture_ap_post_diag(uint8_t addr)
{
    arm_swd_last_ap_post_addr = (uint8_t)(addr & 0x0Cu);
    arm_swd_last_ap_post_status = arm_swd_last_dp_status;
    arm_swd_last_ap_post_ack = arm_swd_last_dp_ack;
    arm_swd_last_ap_post_request = arm_swd_last_dp_request;
    arm_swd_last_ap_post_data = arm_swd_last_dp_data;
}

static uint8_t arm_swd_ack_rank(uint8_t ack)
{
    if (ack == ARM_SWD_ACK_OK) {
        return 4;
    }
    if (ack == ARM_SWD_ACK_WAIT) {
        return 3;
    }
    if (ack == ARM_SWD_ACK_FAULT) {
        return 2;
    }
    if (ack != 0x07) {
        return 1;
    }
    return 0;
}

LINK_SWD_RAM_CODE static void arm_swd_delay_half_cycle(void)
{
    link_swd_half_delay(arm_swd_half_cycle_delay_us);
}

LINK_SWD_RAM_CODE static void arm_swd_clk_low(void)
{
    link_board_swd_clk_set(0);
}

LINK_SWD_RAM_CODE static void arm_swd_clk_high(void)
{
    link_board_swd_clk_set(1);
}

LINK_SWD_RAM_CODE static void arm_swd_dio_force_high(void)
{
    link_board_swd_dio_force_high();
}

LINK_SWD_RAM_CODE static void arm_swd_dio_drive_high(void)
{
    link_board_swd_dio_drive_high();
}

LINK_SWD_RAM_CODE static void arm_swd_dio_drive_low(void)
{
    link_board_swd_dio_drive_low();
}

LINK_SWD_RAM_CODE static void arm_swd_dio_release(void)
{
    link_board_swd_dio_release();
}

LINK_SWD_RAM_CODE static void arm_swd_write_bit(uint8_t bit_val)
{
    if (bit_val) {
        arm_swd_dio_drive_high();
    } else {
        arm_swd_dio_drive_low();
    }

    arm_swd_delay_half_cycle();
    arm_swd_clk_high();
    arm_swd_delay_half_cycle();
    arm_swd_clk_low();
}

LINK_SWD_RAM_CODE static uint8_t arm_swd_read_bit(void)
{
    uint8_t bit_val;

    if (arm_swd_sample_mode == ARM_SWD_SAMPLE_EARLY) {
        arm_swd_delay_half_cycle();
        arm_swd_clk_high();
        bit_val = link_board_swd_dio_get();
        arm_swd_delay_half_cycle();
    } else {
        arm_swd_delay_half_cycle();
        arm_swd_clk_high();
        arm_swd_delay_half_cycle();
        bit_val = link_board_swd_dio_get();
    }
    arm_swd_clk_low();
    return bit_val;
}

LINK_SWD_RAM_CODE static void arm_swd_write_bits(uint32_t value, uint8_t bit_count)
{
#if LINK_BLE_ENABLE && LINK_BLE_SWD_WORD_IO
    link_board_swd_write_bits_fast(value, bit_count, arm_swd_half_cycle_delay_us);
#else
    uint8_t bit_index;

    for (bit_index = 0; bit_index < bit_count; bit_index++) {
        arm_swd_write_bit((uint8_t)(value & 0x01));
        value >>= 1;
    }
#endif
}

LINK_SWD_RAM_CODE static uint32_t arm_swd_read_bits(uint8_t bit_count)
{
    uint8_t bit_index;
    uint32_t value = 0;

    for (bit_index = 0; bit_index < bit_count; bit_index++) {
        value |= ((uint32_t)arm_swd_read_bit() << bit_index);
    }

    return value;
}

LINK_SWD_RAM_CODE static void arm_swd_turnaround_clock_cycles(uint8_t cycle_count)
{
    uint8_t cycle_index;

    for (cycle_index = 0; cycle_index < cycle_count; cycle_index++) {
        arm_swd_delay_half_cycle();
        arm_swd_clk_high();
        arm_swd_delay_half_cycle();
        arm_swd_clk_low();
    }
}

LINK_SWD_RAM_CODE static void arm_swd_turnaround_to_target(void)
{
    arm_swd_dio_release();
    arm_swd_turnaround_clock_cycles(arm_swd_turnaround_cycles);
}

LINK_SWD_RAM_CODE static void arm_swd_turnaround_to_host(void)
{
    arm_swd_dio_release();
    arm_swd_turnaround_clock_cycles(arm_swd_turnaround_cycles);
}

LINK_SWD_RAM_CODE static void arm_swd_turnaround_to_write_data(void)
{
    arm_swd_dio_release();
    arm_swd_turnaround_clock_cycles(arm_swd_write_data_turnaround_cycles);
}

static void arm_swd_line_reset(void)
{
    uint8_t cycle_index;

    arm_swd_dio_release();
    for (cycle_index = 0; cycle_index < ARM_SWD_LINE_RESET_CYCLES; cycle_index++) {
        arm_swd_write_bit(1);
    }
}

static void arm_swd_jtag_to_swd(void)
{
    arm_swd_dio_release();
    arm_swd_write_bits(0xE79Eu, 16);
}

static void arm_swd_idle_cycles(uint8_t cycle_count)
{
    uint8_t cycle_index;

    for (cycle_index = 0; cycle_index < cycle_count; cycle_index++) {
        arm_swd_write_bit(1);  /* HIGH idle - non-standard but may be required by some targets */
    }
}

LINK_SWD_RAM_CODE static void arm_swd_idle_low_cycles(uint8_t cycle_count)
{
#if LINK_BLE_ENABLE && LINK_BLE_SWD_WORD_IO
    link_board_swd_idle_low_fast(cycle_count, arm_swd_half_cycle_delay_us);
#else
    uint8_t cycle_index;

    for (cycle_index = 0; cycle_index < cycle_count; cycle_index++) {
        arm_swd_write_bit(0);
    }
#endif
}

static void arm_swd_capture_line_diag(void)
{
    arm_swd_dio_force_high();
    arm_swd_delay_half_cycle();
    arm_swd_last_dio_read_high = link_board_swd_dio_get();

    arm_swd_dio_drive_low();
    arm_swd_delay_half_cycle();
    arm_swd_last_dio_read_low = link_board_swd_dio_get();

    arm_swd_dio_release();
    arm_swd_delay_half_cycle();
    arm_swd_last_dio_read_release = link_board_swd_dio_get();
}

static void arm_swd_sync_sequence(void)
{
    arm_swd_line_reset();
    if (arm_swd_sync_mode == ARM_SWD_SYNC_FULL) {
        arm_swd_jtag_to_swd();
        arm_swd_line_reset();
    }
#if LINK_BLE_ENABLE
    /* Arm DebugPortSetup: leave line reset with >=2 SWDIO LOW clocks before
     * the first DPIDR request. HIGH clocks keep the SW-DP in line reset and
     * the first request can be lost (ACK=111). Do not depend on recovery. */
    arm_swd_idle_low_cycles(ARM_SWD_IDLE_CYCLES);
#else
    /* Preserve the already validated USB-only image and recovery path. */
    arm_swd_idle_cycles(ARM_SWD_IDLE_CYCLES);
#endif
}

static void arm_swd_sync_under_reset(void)
{
    link_board_target_nrst_set(0);
    DelayMs(ARM_SWD_ATTACH_RESET_HOLD_MS);

    /* Drive the SWD wakeup sequence while the target is held in reset. */
    arm_swd_line_reset();
    if (arm_swd_sync_mode == ARM_SWD_SYNC_FULL) {
        arm_swd_jtag_to_swd();
        arm_swd_line_reset();
    }
    arm_swd_idle_cycles(ARM_SWD_IDLE_CYCLES);

    link_board_target_nrst_set(1);
    DelayMs(ARM_SWD_ATTACH_RELEASE_DELAY_MS);
    arm_swd_idle_low_cycles(ARM_SWD_IDLE_CYCLES);
}

static void arm_swd_sync_without_reset(void)
{
    arm_swd_sync_sequence();
    DelayMs(ARM_SWD_POST_CONNECT_DELAY_MS);
}

static void arm_swd_attach_sequence(uint8_t attach_mode)
{
    if (attach_mode == ARM_SWD_ATTACH_UNDER_RESET) {
        arm_swd_sync_under_reset();
    } else {
        arm_swd_sync_without_reset();
    }
}

static void arm_swd_apply_strategy(uint8_t strategy_index)
{
    arm_swd_half_cycle_delay_us = arm_swd_strategies[strategy_index].delay_us;
    arm_swd_sample_mode = arm_swd_strategies[strategy_index].sample_mode;
    arm_swd_sync_mode = arm_swd_strategies[strategy_index].sync_mode;
    arm_swd_last_strategy = strategy_index;
}

static uint8_t arm_swd_make_request(uint8_t ap_ndp, uint8_t rnw, uint8_t addr)
{
    uint8_t request = 0x81;
    uint8_t a2 = (addr >> 2) & 0x01;
    uint8_t a3 = (addr >> 3) & 0x01;
    uint8_t parity = (uint8_t)(ap_ndp ^ rnw ^ a2 ^ a3);

    request |= (ap_ndp << 1);
    request |= (rnw << 2);
    request |= (a2 << 3);
    request |= (a3 << 4);
    request |= (parity << 5);
    return request;
}

#if LINK_BLE_ENABLE && LINK_BLE_SWD_WORD_IO
/* Dispatch fast word IO for both BLE and USB once debug power is up. */
static uint8_t arm_swd_use_word_io(void)
{
    return arm_swd_dap_power_ready;
}

LINK_SWD_RAM_CODE static uint32_t arm_swd_read_payload32(void)
{
    if (arm_swd_sample_mode == ARM_SWD_SAMPLE_EARLY && arm_swd_use_word_io())
        return link_board_swd_read_word_early(arm_swd_half_cycle_delay_us);
    return arm_swd_read_bits(32);
}

LINK_SWD_RAM_CODE static void arm_swd_write_payload32(uint32_t value, uint8_t ap_drw)
{
    (void)ap_drw;
    if (arm_swd_dap_power_ready) {
        uint8_t owner = link_session_owner();
        if (owner == LINK_OWNER_BLE ||
            (LINK_USB_DRW_WORD_IO && owner == LINK_OWNER_USB && arm_swd_dap_write_phase_valid)) {
            link_board_swd_write_word(value, arm_swd_half_cycle_delay_us);
            return;
        }
    }
    arm_swd_write_bits(value, 32);
}
#else
#define arm_swd_read_payload32() arm_swd_read_bits(32)
#define arm_swd_write_payload32(value, ap_drw) arm_swd_write_bits((value), 32)
#endif

LINK_SWD_RAM_CODE static link_target_status_t arm_swd_read_dp_once(uint8_t addr, uint32_t *value, uint8_t *raw_ack)
{
    uint32_t irq_status = 0;
    uint8_t ack;
    uint32_t data_val;
    uint8_t parity;
    link_target_status_t status;

    if (value == 0) {
        return LINK_TARGET_IO_ERROR;
    }

    LINK_SWD_IRQ_SAVE(&irq_status);

    arm_swd_dio_release();
    arm_swd_write_bits(arm_swd_make_request(0, 1, addr), 8);
    arm_swd_turnaround_to_target();

    /* Read exactly 3 ACK bits as required by ARM SWD spec. */
    ack = (uint8_t)arm_swd_read_bits(3);
    arm_swd_last_ack_window = ack;
    if (raw_ack != 0) {
        *raw_ack = ack;
    }

    if (ack == ARM_SWD_ACK_OK) {
        data_val = arm_swd_read_payload32();
        parity = (uint8_t)arm_swd_read_bit();
        arm_swd_turnaround_to_host();
        /* Idle cycles MUST be LOW (0). HIGH would look like START bits to the target. */
        arm_swd_idle_low_cycles(8);
        arm_swd_dio_release();
        LINK_SWD_IRQ_RESTORE(irq_status);

        if ((__builtin_parity(data_val) & 0x01) != parity) {
            link_perf_note_swd(0, 1, ack, 1u);
            arm_swd_set_transfer_diag(0, 1, addr, ack, LINK_TARGET_PARITY, data_val);
            return LINK_TARGET_PARITY;
        }

        *value = data_val;
        link_perf_note_swd(0, 1, ack, 0u);
        arm_swd_set_transfer_diag(0, 1, addr, ack, LINK_TARGET_OK, data_val);
        return LINK_TARGET_OK;
    }

    /* WAIT/FAULT on a read: ARM SWD spec says the data phase is not driven
     * by the target. We do NOT clock out dummy data bits. Just turnaround
     * back to host so the bus is ready for the next transaction. */

    arm_swd_turnaround_to_host();
    arm_swd_idle_low_cycles(8);
    arm_swd_dio_release();
    LINK_SWD_IRQ_RESTORE(irq_status);

    if (ack == ARM_SWD_ACK_WAIT) {
        status = LINK_TARGET_WAIT;
    } else if (ack == ARM_SWD_ACK_FAULT) {
        status = LINK_TARGET_FAULT;
    } else {
        status = LINK_TARGET_IO_ERROR;
    }
    link_perf_note_swd(0, 1, ack, (status == LINK_TARGET_IO_ERROR) || (status == LINK_TARGET_PARITY));
    arm_swd_set_transfer_diag(0, 1, addr, ack, status, 0);
    return status;
}

LINK_SWD_RAM_CODE static link_target_status_t arm_swd_write_dp_once(uint8_t addr, uint32_t value, uint8_t *raw_ack)
{
    uint32_t irq_status = 0;
    uint8_t ack;
    uint8_t parity;
    link_target_status_t status;

    if (arm_swd_dap_write_phase_valid) {
        arm_swd_write_data_turnaround_cycles = arm_swd_dap_write_phase;
    }
    LINK_SWD_IRQ_SAVE(&irq_status);

    arm_swd_dio_release();
    arm_swd_write_bits(arm_swd_make_request(0, 0, addr), 8);
    arm_swd_turnaround_to_target();

    ack = (uint8_t)arm_swd_read_bits(3);
    arm_swd_last_ack_window = ack;  /* Track ACK for write operations too */
    if (raw_ack != 0) {
        *raw_ack = ack;
    }

    if (ack == ARM_SWD_ACK_OK) {
        arm_swd_turnaround_to_write_data();
        arm_swd_write_payload32(value, 0);
        parity = (uint8_t)(__builtin_parity(value) & 0x01);
        arm_swd_write_bit(parity);
        arm_swd_idle_low_cycles(8);
        arm_swd_dio_release();
        LINK_SWD_IRQ_RESTORE(irq_status);
        link_perf_note_swd(0, 0, ack, 0u);
        arm_swd_set_transfer_diag(0, 0, addr, ack, LINK_TARGET_OK, value);
        return LINK_TARGET_OK;
    }

    /* ABORT register (addr=0x00) is special: even if ACK=FAULT,
     * we must complete the write transaction (send data + parity)
     * to avoid bus protocol errors. Per ARM IHI0031, ABORT is
     * always accessible and the write data phase must be sent. */
    if (addr == 0x00 && ack == ARM_SWD_ACK_FAULT) {
        arm_swd_turnaround_to_write_data();
        arm_swd_write_bits(value, 32);
        parity = (uint8_t)(__builtin_parity(value) & 0x01);
        arm_swd_write_bit(parity);
        arm_swd_idle_low_cycles(8);
        arm_swd_dio_release();
        LINK_SWD_IRQ_RESTORE(irq_status);
        link_perf_note_swd(0, 0, ack, 0u);
        arm_swd_set_transfer_diag(0, 0, addr, ack, LINK_TARGET_FAULT, value);
        return LINK_TARGET_FAULT;
    }

    arm_swd_turnaround_to_host();
    arm_swd_idle_low_cycles(8);
    arm_swd_dio_release();
    LINK_SWD_IRQ_RESTORE(irq_status);

    if (ack == ARM_SWD_ACK_WAIT) {
        status = LINK_TARGET_WAIT;
    } else if (ack == ARM_SWD_ACK_FAULT) {
        status = LINK_TARGET_FAULT;
    } else {
        status = LINK_TARGET_IO_ERROR;
    }
    link_perf_note_swd(0, 0, ack, (status == LINK_TARGET_IO_ERROR) || (status == LINK_TARGET_PARITY));
    arm_swd_set_transfer_diag(0, 0, addr, ack, status, value);
    return status;
}

static link_target_status_t arm_swd_dap_write_dp_with_retry(uint8_t addr, uint32_t value)
{
    uint8_t write_turnaround;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    if ((addr == ARM_SWD_DP_CTRL_STAT) && ((value & ARM_SWD_DP_POWER_REQ) == ARM_SWD_DP_POWER_REQ)) {
        LINK_PERF_INC(dp_power_req_calls);
    }

    if (arm_swd_dap_calibration_enabled) {
        uint8_t retry;

        if (!arm_swd_dap_write_phase_valid) {
            return LINK_TARGET_IO_ERROR;
        }
        arm_swd_write_data_turnaround_cycles = arm_swd_dap_write_phase;
        status = arm_swd_write_dp_once(addr, value, 0);
        if (status != LINK_TARGET_OK) {
            return status;
        }
        if (addr == ARM_SWD_DP_SELECT) {
            if ((arm_swd_dp_select ^ value) & 0xFF000000u) {
                arm_swd_dap_csw_valid = 0;
            }
            arm_swd_dp_select = value;
            arm_swd_dap_select_valid = 1;
        } else if (addr == ARM_SWD_DP_CTRL_STAT) {
            if ((value & ARM_SWD_DP_POWER_REQ) != ARM_SWD_DP_POWER_REQ) {
                arm_swd_dap_invalidate_context();
                return LINK_TARGET_OK;
            }
            for (retry = 0; retry < 5u; retry++) {
                uint32_t ctrl_stat = 0;

                LINK_PERF_INC(dp_power_req_polls);
                status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, 0);
                if (status != LINK_TARGET_OK) {
                    return status;
                }
                if ((ctrl_stat & ARM_SWD_DP_POWER_ACK) == ARM_SWD_DP_POWER_ACK) {
                    arm_swd_dap_power_ready = 1;
                    return LINK_TARGET_OK;
                }
                DelayUs(200);
            }
            arm_swd_dap_invalidate_context();
            return LINK_TARGET_WAIT;
        }
        return LINK_TARGET_OK;
    }

    for (write_turnaround = 0; write_turnaround <= ARM_SWD_MAX_WRITE_DATA_TURNAROUND_CYCLES; write_turnaround++) {
        uint8_t raw_ack = 0;

        arm_swd_write_data_turnaround_cycles = write_turnaround;
        status = arm_swd_write_dp_once(addr, value, &raw_ack);
        if ((addr == 0x00u) && (status == LINK_TARGET_FAULT)) {
            return status;
        }
        if (status != LINK_TARGET_OK) {
            (void)arm_swd_write_dp_once(0x00, ARM_SWD_DP_ABORT_DAP_AND_CLR_ALL, 0);
            arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
            continue;
        }

        if ((addr == ARM_SWD_DP_CTRL_STAT) && ((value & ARM_SWD_DP_POWER_REQ) == ARM_SWD_DP_POWER_REQ)) {
            uint8_t retry;

            for (retry = 0; retry < 5u; retry++) {
                uint32_t ctrl_stat = 0;
                link_target_status_t read_status;

                LINK_PERF_INC(dp_power_req_polls);
                read_status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, 0);
                if ((read_status == LINK_TARGET_OK) &&
                    ((ctrl_stat & ARM_SWD_DP_POWER_ACK) == ARM_SWD_DP_POWER_ACK)) {
                    return LINK_TARGET_OK;
                }
                DelayUs(200);
            }
            (void)arm_swd_write_dp_once(0x00, ARM_SWD_DP_ABORT_DAP_AND_CLR_ALL, 0);
            arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
            continue;
        }

        if (addr == ARM_SWD_DP_SELECT) {
            arm_swd_dp_select = value;
        }
        return LINK_TARGET_OK;
    }

    return status;
}

LINK_SWD_RAM_CODE static link_target_status_t arm_swd_read_ap_once(uint8_t addr, uint32_t *value, uint8_t *raw_ack)
{
    uint32_t irq_status = 0;
    uint8_t ack;
    uint32_t data_val;
    uint8_t parity;
    link_target_status_t status;

    if (value == 0) {
        return LINK_TARGET_IO_ERROR;
    }

    LINK_SWD_IRQ_SAVE(&irq_status);

    arm_swd_dio_release();
    arm_swd_write_bits(arm_swd_make_request(1, 1, addr), 8);
    arm_swd_turnaround_to_target();

    ack = (uint8_t)arm_swd_read_bits(3);
    arm_swd_last_ack_window = ack;
    if (raw_ack != 0) {
        *raw_ack = ack;
    }

    if (ack == ARM_SWD_ACK_OK) {
        data_val = arm_swd_read_payload32();
        parity = (uint8_t)arm_swd_read_bit();
        arm_swd_turnaround_to_host();
        arm_swd_idle_low_cycles(8);
        arm_swd_dio_release();
        LINK_SWD_IRQ_RESTORE(irq_status);

        if ((__builtin_parity(data_val) & 0x01) != parity) {
            link_perf_note_swd(1, 1, ack, 1u);
            arm_swd_set_transfer_diag(1, 1, addr, ack, LINK_TARGET_PARITY, data_val);
            return LINK_TARGET_PARITY;
        }

        *value = data_val;
        link_perf_note_swd(1, 1, ack, 0u);
        arm_swd_set_transfer_diag(1, 1, addr, ack, LINK_TARGET_OK, data_val);
        return LINK_TARGET_OK;
    }

    arm_swd_turnaround_to_host();
    arm_swd_idle_low_cycles(8);
    arm_swd_dio_release();
    LINK_SWD_IRQ_RESTORE(irq_status);

    if (ack == ARM_SWD_ACK_WAIT) {
        status = LINK_TARGET_WAIT;
    } else if (ack == ARM_SWD_ACK_FAULT) {
        status = LINK_TARGET_FAULT;
    } else {
        status = LINK_TARGET_IO_ERROR;
    }
    link_perf_note_swd(1, 1, ack, (status == LINK_TARGET_IO_ERROR) || (status == LINK_TARGET_PARITY));
    arm_swd_set_transfer_diag(1, 1, addr, ack, status, 0);
    return status;
}

#if LINK_SWD_SYNTHETIC_M3_IDS
static uint8_t arm_swd_rom_table_read_fallback(uint32_t addr, uint32_t *value)
{
    if ((value == 0) || (addr < ARM_SWD_CORTEX_M3_ROM_BASE) || (addr >= ARM_SWD_CORTEX_M3_ROM_END) ||
        ((addr & 0x03u) != 0)) {
        return 0;
    }

    switch (addr - ARM_SWD_CORTEX_M3_ROM_BASE) {
    case 0x000u:
        *value = 0xFFF0F003u; /* SCS */
        return 1;
    case 0x004u:
        *value = 0xFFF02003u; /* DWT */
        return 1;
    case 0x008u:
        *value = 0xFFF03003u; /* FPB */
        return 1;
    case 0x00Cu:
        *value = 0xFFF01003u; /* ITM */
        return 1;
    case 0x010u:
        *value = 0xFFF41003u; /* TPIU */
        return 1;
    case 0x014u:
        *value = 0x00000000u;
        return 1;
    case 0xFCCu:
        *value = 0x00000001u; /* MEMTYPE: system memory present */
        return 1;
    case 0xFD0u:
        *value = 0x00000004u; /* PID4 */
        return 1;
    case 0xFD4u:
    case 0xFD8u:
    case 0xFDCu:
        *value = 0x00000000u; /* PID5..PID7 */
        return 1;
    case 0xFE0u:
        *value = 0x000000C3u; /* PID0 */
        return 1;
    case 0xFE4u:
        *value = 0x00000000u; /* PID1: Keil STM32F1 CheckID expects JEP106 0x20 */
        return 1;
    case 0xFE8u:
        *value = 0x00000002u; /* PID2: ((PID2[2:0] << 4) | PID1[7:4]) == 0x20 */
        return 1;
    case 0xFECu:
        *value = 0x00000000u; /* PID3 */
        return 1;
    case 0xFF0u:
        *value = 0x0000000Du; /* CID0 */
        return 1;
    case 0xFF4u:
        *value = 0x00000010u; /* CID1 */
        return 1;
    case 0xFF8u:
        *value = 0x00000005u; /* CID2 */
        return 1;
    case 0xFFCu:
        *value = 0x000000B1u; /* CID3 */
        return 1;
    default:
        return 0;
    }
}

static uint8_t arm_swd_component_id_value(uint32_t offset, uint8_t pid0, uint8_t pid1,
                                          uint8_t pid2, uint8_t pid3, uint8_t cid1,
                                          uint32_t *value)
{
    if (value == 0) {
        return 0;
    }

    switch (offset) {
    case 0x000u:
        *value = 0x00000004u; /* PID4 */
        return 1;
    case 0x004u:
    case 0x008u:
    case 0x00Cu:
        *value = 0x00000000u; /* PID5..PID7 */
        return 1;
    case 0x010u:
        *value = pid0;
        return 1;
    case 0x014u:
        *value = pid1;
        return 1;
    case 0x018u:
        *value = pid2;
        return 1;
    case 0x01Cu:
        *value = pid3;
        return 1;
    case 0x020u:
        *value = 0x0000000Du; /* CID0 */
        return 1;
    case 0x024u:
        *value = cid1;
        return 1;
    case 0x028u:
        *value = 0x00000005u; /* CID2 */
        return 1;
    case 0x02Cu:
        *value = 0x000000B1u; /* CID3 */
        return 1;
    default:
        return 0;
    }
}

static uint8_t arm_swd_rom_component_id_read_fallback(uint32_t addr, uint32_t *value)
{
    uint32_t base = addr & 0xFFFFF000u;
    uint32_t offset = addr & 0x00000FFFu;

    if ((value == 0) || ((addr & 0x03u) != 0) ||
        (offset < ARM_SWD_CORTEX_M3_ID_OFFSET_BASE) ||
        (offset >= ARM_SWD_CORTEX_M3_ID_OFFSET_END)) {
        return 0;
    }

    offset -= ARM_SWD_CORTEX_M3_ID_OFFSET_BASE;

    switch (base) {
    case 0xE0000000u: /* ITM */
        return arm_swd_component_id_value(offset, 0x01u, 0xB0u, 0x2Bu, 0x00u, 0x90u, value);
    case 0xE0001000u: /* DWT */
        return arm_swd_component_id_value(offset, 0x02u, 0xB0u, 0x3Bu, 0x00u, 0x90u, value);
    case 0xE0002000u: /* FPB */
        return arm_swd_component_id_value(offset, 0x03u, 0xB0u, 0x2Bu, 0x00u, 0x90u, value);
    case 0xE000E000u: /* SCS */
        return arm_swd_component_id_value(offset, 0x00u, 0xB0u, 0x0Bu, 0x00u, 0xE0u, value);
    case 0xE0040000u: /* TPIU */
        return arm_swd_component_id_value(offset, 0x21u, 0xB9u, 0x2Bu, 0x00u, 0x90u, value);
    default:
        return 0;
    }
}

static uint8_t arm_swd_scs_register_read_fallback(uint32_t addr, uint32_t *value)
{
    if (value == 0) {
        return 0;
    }

    if (addr == ARM_SWD_CORTEX_M3_CPUID) {
        *value = ARM_SWD_CORTEX_M3_CPUID_VALUE;
        return 1;
    }

    return 0;
}
#endif

LINK_SWD_RAM_CODE static link_target_status_t arm_swd_write_ap_once(uint8_t addr, uint32_t value, uint8_t *raw_ack)
{
    uint32_t irq_status = 0;
    uint8_t ack;
    uint8_t parity;
    link_target_status_t status;

    if (arm_swd_dap_write_phase_valid) {
        arm_swd_write_data_turnaround_cycles = arm_swd_dap_write_phase;
    }
    LINK_SWD_IRQ_SAVE(&irq_status);

    arm_swd_dio_release();
    arm_swd_write_bits(arm_swd_make_request(1, 0, addr), 8);
    arm_swd_turnaround_to_target();

    ack = (uint8_t)arm_swd_read_bits(3);
    arm_swd_last_ack_window = ack;
    if (raw_ack != 0) {
        *raw_ack = ack;
    }

    if (ack == ARM_SWD_ACK_OK) {
        arm_swd_turnaround_to_write_data();
        arm_swd_write_payload32(value, addr == ARM_SWD_AP_DRW);
        parity = (uint8_t)(__builtin_parity(value) & 0x01);
        arm_swd_write_bit(parity);
        arm_swd_idle_low_cycles(8);
        arm_swd_dio_release();
        LINK_SWD_IRQ_RESTORE(irq_status);
        link_perf_note_swd(1, 0, ack, 0u);
        arm_swd_set_transfer_diag(1, 0, addr, ack, LINK_TARGET_OK, value);
        return LINK_TARGET_OK;
    }

    arm_swd_turnaround_to_host();
    arm_swd_idle_low_cycles(8);
    arm_swd_dio_release();
    LINK_SWD_IRQ_RESTORE(irq_status);

    if (ack == ARM_SWD_ACK_WAIT) {
        status = LINK_TARGET_WAIT;
    } else if (ack == ARM_SWD_ACK_FAULT) {
        status = LINK_TARGET_FAULT;
    } else {
        status = LINK_TARGET_IO_ERROR;
    }
    link_perf_note_swd(1, 0, ack, (status == LINK_TARGET_IO_ERROR) || (status == LINK_TARGET_PARITY));
    arm_swd_set_transfer_diag(1, 0, addr, ack, status, value);
    return status;
}

static link_target_status_t arm_swd_select_ap_bank(uint32_t select_value)
{
    link_target_status_t status;

    if (arm_swd_dap_write_phase_valid && arm_swd_dap_select_valid &&
        (arm_swd_dp_select == select_value)) {
        return LINK_TARGET_OK;
    }
    status = arm_swd_write_dp_once(ARM_SWD_DP_SELECT, select_value, 0);
    if (status == LINK_TARGET_OK) {
        if ((arm_swd_dp_select ^ select_value) & 0xFF000000u) {
            arm_swd_dap_csw_valid = 0;
        }
        arm_swd_dp_select = select_value;
        arm_swd_dap_select_valid = arm_swd_dap_write_phase_valid;
    }
    return status;
}

static link_target_status_t arm_swd_read_ap_reg(uint8_t addr, uint32_t *value)
{
    uint32_t posted = 0;
    link_target_status_t status;
    uint8_t stage = ARM_SWD_MEM_STAGE_DRW;
    uint8_t bank0 = ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0);

    if (addr == ARM_SWD_AP_CSW) {
        stage = ARM_SWD_MEM_STAGE_CSW;
    } else if (addr == ARM_SWD_AP_TAR) {
        stage = ARM_SWD_MEM_STAGE_TAR;
    }

    if (bank0 && (addr == ARM_SWD_AP_CSW) &&
        ((arm_swd_mem_ap_csw & (ARM_SWD_AP_CSW_ADDRINC_MASK | ARM_SWD_AP_CSW_SIZE_MASK)) !=
         (ARM_SWD_AP_CSW_ADDRINC_SINGLE | ARM_SWD_AP_CSW_SIZE_32))) {
        *value = arm_swd_mem_ap_csw;
        arm_swd_set_transfer_diag(1, 1, ARM_SWD_AP_CSW, ARM_SWD_ACK_OK, LINK_TARGET_OK, *value);
        return LINK_TARGET_OK;
    }

#if LINK_SWD_SYNTHETIC_M3_IDS
    if (bank0 && (addr == ARM_SWD_AP_DRW) && arm_swd_rom_table_read_fallback(arm_swd_mem_ap_tar, value)) {
        arm_swd_mem_ap_tar += 4u;
        return LINK_TARGET_OK;
    }
    if (bank0 && (addr == ARM_SWD_AP_DRW) && arm_swd_rom_component_id_read_fallback(arm_swd_mem_ap_tar, value)) {
        arm_swd_mem_ap_tar += 4u;
        return LINK_TARGET_OK;
    }
    if (bank0 && (addr == ARM_SWD_AP_DRW) && arm_swd_scs_register_read_fallback(arm_swd_mem_ap_tar, value)) {
        arm_swd_mem_ap_tar += 4u;
        return LINK_TARGET_OK;
    }
#endif

    status = arm_swd_read_ap_once(addr, &posted, 0);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(stage, addr);
        return status;
    }

    status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, value, 0);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF, addr);
#if LINK_SWD_SYNTHETIC_M3_IDS
    } else if ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANKF) {
        if ((addr == ARM_SWD_AP_BASE) && (*value == 0u)) {
            *value = ARM_SWD_MEM_AP_BASE_FALLBACK;
        } else if ((addr == ARM_SWD_AP_DRW) && (*value == 0u)) {
            *value = ARM_SWD_MEM_AP_IDR_FALLBACK;
        }
#endif
    }
    return status;
}

static link_target_status_t arm_swd_write_ap_reg(uint8_t addr, uint32_t value)
{
    link_target_status_t status;

    status = arm_swd_write_ap_once(addr, value, 0);
    if ((status == LINK_TARGET_OK) &&
        ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0) &&
        (addr == ARM_SWD_AP_TAR)) {
        arm_swd_mem_ap_tar = value;
    }
    if ((status == LINK_TARGET_OK) &&
        ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0) &&
        (addr == ARM_SWD_AP_CSW)) {
        arm_swd_mem_ap_csw = value;
        arm_swd_dap_csw_valid = arm_swd_dap_write_phase_valid;
    }
    return status;
}

/* Only DRW is completed here. CSW/TAR writes must not drain RDBUFF. */
static link_target_status_t arm_swd_dap_check_drw_write(uint8_t drain_rdbuff)
{
    uint32_t value = 0;
    link_target_status_t status;

    if (drain_rdbuff) {
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &value, 0);
        if (status != LINK_TARGET_OK) {
            return status;
        }
    }
    status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &value, 0);
    if (status != LINK_TARGET_OK) {
        return status;
    }
    if (value & ARM_SWD_DP_CTRL_STICKY_MASK) {
        arm_swd_dap_invalidate_context();
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_DRW, value);
        return LINK_TARGET_FAULT;
    }
    return LINK_TARGET_OK;
}

static uint8_t arm_swd_dap_is_bank0_csw(uint8_t addr)
{
    return (addr == ARM_SWD_AP_CSW) &&
           ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == 0u);
}

static uint32_t arm_swd_dap_normalize_csw(uint32_t value)
{
    if ((value & (ARM_SWD_AP_CSW_ADDRINC_MASK | ARM_SWD_AP_CSW_SIZE_MASK)) ==
        (ARM_SWD_AP_CSW_ADDRINC_PACKED | ARM_SWD_AP_CSW_SIZE_16)) {
        value = (value & ~ARM_SWD_AP_CSW_ADDRINC_MASK) | ARM_SWD_AP_CSW_ADDRINC_SINGLE;
    }
    return value;
}

static uint8_t arm_swd_dap_can_use_mem32_write(void)
{
    return ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0) &&
           ((arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_SIZE_MASK) == ARM_SWD_AP_CSW_SIZE_32) &&
           ((arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_ADDRINC_MASK) == ARM_SWD_AP_CSW_ADDRINC_SINGLE);
}

static link_target_status_t arm_swd_dap_write_drw_mem32(uint32_t addr, uint32_t value)
{
    link_target_status_t status;

    status = target_arm_swd_write32(addr, value);
    if (status == LINK_TARGET_OK) {
        arm_swd_mem_ap_tar = addr + 4u;
        arm_swd_mem_ap_csw = ARM_SWD_MEM_AP_CSW_32;
    }
    return status;
}

static uint8_t arm_swd_dap_bank1_data_addr(uint8_t ap_addr, uint32_t *addr)
{
    if (((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) != ARM_SWD_MEM_AP_BANK1) ||
        ((arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_SIZE_MASK) != ARM_SWD_AP_CSW_SIZE_32)) {
        return 0;
    }

    if (addr != 0) {
        *addr = (arm_swd_mem_ap_tar & ~0x0Fu) | (uint32_t)(ap_addr & 0x0Cu);
    }
    return 1;
}

static link_target_status_t arm_swd_dap_restore_mem_ap_context(uint32_t select,
                                                               uint32_t tar,
                                                               uint32_t csw)
{
    link_target_status_t status;

    status = arm_swd_select_ap_bank(ARM_SWD_MEM_AP_BANK0);
    if (status != LINK_TARGET_OK) {
        return status;
    }
    status = arm_swd_dap_write_ap_with_retry(ARM_SWD_AP_CSW, csw);
    if (status != LINK_TARGET_OK) {
        return status;
    }
    status = arm_swd_dap_write_ap_with_retry(ARM_SWD_AP_TAR, tar);
    if (status != LINK_TARGET_OK) {
        return status;
    }
    status = arm_swd_select_ap_bank(select);
    if (status == LINK_TARGET_OK) {
        arm_swd_dp_select = select;
        arm_swd_mem_ap_tar = tar;
        arm_swd_mem_ap_csw = csw;
    }
    return status;
}

static link_target_status_t arm_swd_dap_bank1_read32(uint8_t ap_addr, uint32_t *value)
{
#if LINK_USB_USE_CHERRYUSB
    uint32_t posted;
    link_target_status_t status;

    if ((value == 0) || !arm_swd_dap_bank1_data_addr(ap_addr, 0)) {
        return LINK_TARGET_BAD_PARAM;
    }
    /* Native BDn leaves SELECT, CSW and TAR untouched. Real target readback
     * has verified all four BD registers after write-phase calibration. */
    status = arm_swd_read_ap_once(ap_addr, &posted, 0);
    arm_swd_capture_ap_post_diag(ap_addr);
    if (status == LINK_TARGET_OK) {
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, value, 0);
    }
    return status;
#else
    uint32_t saved_select = arm_swd_dp_select;
    uint32_t saved_tar = arm_swd_mem_ap_tar;
    uint32_t saved_csw = arm_swd_mem_ap_csw;
    uint32_t mem_addr;
    link_target_status_t access_status;
    link_target_status_t restore_status;

    if ((value == 0) || !arm_swd_dap_bank1_data_addr(ap_addr, &mem_addr)) {
        return LINK_TARGET_BAD_PARAM;
    }

    access_status = target_arm_swd_read32(mem_addr, value);
    restore_status = arm_swd_dap_restore_mem_ap_context(saved_select, saved_tar, saved_csw);
    return (access_status == LINK_TARGET_OK) ? restore_status : access_status;
#endif
}

static link_target_status_t arm_swd_dap_bank1_write32(uint8_t ap_addr, uint32_t value)
{
#if LINK_USB_USE_CHERRYUSB
    uint32_t posted;
    link_target_status_t status;

    if (!arm_swd_dap_bank1_data_addr(ap_addr, 0)) {
        return LINK_TARGET_BAD_PARAM;
    }
    status = arm_swd_write_ap_once(ap_addr, value, 0);
    if (status == LINK_TARGET_OK) {
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &posted, 0);
    }
    /* The DAP write caller retains its CTRL/STAT sticky-error check. */
    return status;
#else
    uint32_t saved_select = arm_swd_dp_select;
    uint32_t saved_tar = arm_swd_mem_ap_tar;
    uint32_t saved_csw = arm_swd_mem_ap_csw;
    uint32_t mem_addr;
    link_target_status_t access_status;
    link_target_status_t restore_status;

    if (!arm_swd_dap_bank1_data_addr(ap_addr, &mem_addr)) {
        return LINK_TARGET_BAD_PARAM;
    }

    access_status = target_arm_swd_write32(mem_addr, value);
    restore_status = arm_swd_dap_restore_mem_ap_context(saved_select, saved_tar, saved_csw);
    return (access_status == LINK_TARGET_OK) ? restore_status : access_status;
#endif
}

static link_target_status_t arm_swd_dap_write_ap_with_retry(uint8_t addr, uint32_t value)
{
    uint8_t write_turnaround;
    link_target_status_t status = LINK_TARGET_IO_ERROR;
    uint8_t bank0 = ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0);
    uint8_t verify_csw = arm_swd_dap_is_bank0_csw(addr);
    uint32_t write_value = verify_csw ? arm_swd_dap_normalize_csw(value) : value;

    if (arm_swd_dap_calibration_enabled && !arm_swd_dap_write_phase_valid) {
        return LINK_TARGET_IO_ERROR;
    }
    if ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK1) {
        status = arm_swd_dap_bank1_write32(addr, write_value);
        if ((status == LINK_TARGET_OK) && arm_swd_dap_write_phase_valid) {
            status = arm_swd_dap_check_drw_write(0);
        }
        return status;
    }

    if (arm_swd_dap_write_phase_valid) {
        uint32_t write_addr = arm_swd_mem_ap_tar;
        uint32_t size = arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_SIZE_MASK;
        uint32_t inc = arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_ADDRINC_MASK;
        uint8_t used_mem_write = 0;

        arm_swd_write_data_turnaround_cycles = arm_swd_dap_write_phase;
        if (bank0 && (addr == ARM_SWD_AP_DRW) && arm_swd_dap_can_use_mem32_write()) {
            status = arm_swd_dap_write_drw_mem32(write_addr, write_value);
            used_mem_write = 1;
        } else if (bank0 && (addr == ARM_SWD_AP_DRW) &&
                   (size == ARM_SWD_AP_CSW_SIZE_16) && (inc == ARM_SWD_AP_CSW_ADDRINC_SINGLE)) {
            uint16_t halfword = (uint16_t)(write_value >> ((write_addr & 2u) * 8u));

            status = target_arm_swd_write16(write_addr, halfword);
            used_mem_write = 1;
        } else {
            status = arm_swd_write_ap_once(addr, write_value, 0);
        }
        if (status != LINK_TARGET_OK) {
            return status;
        }
        if (bank0 && (addr == ARM_SWD_AP_TAR)) {
            arm_swd_mem_ap_tar = write_value;
        } else if (bank0 && (addr == ARM_SWD_AP_CSW)) {
            arm_swd_mem_ap_csw = write_value;
            arm_swd_dap_csw_valid = 1;
        } else if (bank0 && (addr == ARM_SWD_AP_DRW)) {
            status = arm_swd_dap_check_drw_write(!used_mem_write);
            if (status != LINK_TARGET_OK) {
                arm_swd_mem_ap_tar = write_addr;
                return status;
            }
            if (inc == ARM_SWD_AP_CSW_ADDRINC_SINGLE) {
                arm_swd_mem_ap_tar = write_addr + (1u << size);
            }
        }
        return LINK_TARGET_OK;
    }

    if (bank0 && (addr == ARM_SWD_AP_DRW) && arm_swd_dap_can_use_mem32_write()) {
        uint32_t write_addr = arm_swd_mem_ap_tar;

        for (write_turnaround = 0; write_turnaround <= ARM_SWD_MAX_WRITE_DATA_TURNAROUND_CYCLES; write_turnaround++) {
            arm_swd_write_data_turnaround_cycles = write_turnaround;
            arm_swd_mem_ap_tar = write_addr;
            status = arm_swd_dap_write_drw_mem32(write_addr, write_value);
            if (status == LINK_TARGET_OK) {
                return LINK_TARGET_OK;
            }
            arm_swd_mem_ap_tar = write_addr;
            (void)arm_swd_dap_write_dp_with_retry(0x00, ARM_SWD_DP_ABORT_CLR_ALL);
            arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
        }
        return status;
    }

    for (write_turnaround = 0; write_turnaround <= ARM_SWD_MAX_WRITE_DATA_TURNAROUND_CYCLES; write_turnaround++) {
        uint8_t raw_ack = 0;

        arm_swd_write_data_turnaround_cycles = write_turnaround;
        status = arm_swd_write_ap_once(addr, write_value, &raw_ack);
        if (status == LINK_TARGET_OK) {
            if (bank0 && (addr == ARM_SWD_AP_TAR)) {
                arm_swd_mem_ap_tar = write_value;
            }
            if (bank0 && (addr == ARM_SWD_AP_CSW)) {
                arm_swd_mem_ap_csw = write_value;
            }
            if (bank0 && (addr == ARM_SWD_AP_DRW)) {
                uint32_t size = arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_SIZE_MASK;
                uint32_t inc = arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_ADDRINC_MASK;

                if (inc == ARM_SWD_AP_CSW_ADDRINC_SINGLE) {
                    if (size == ARM_SWD_AP_CSW_SIZE_32) {
                        arm_swd_mem_ap_tar += 4u;
                    } else if (size == ARM_SWD_AP_CSW_SIZE_16) {
                        arm_swd_mem_ap_tar += 2u;
                    }
                }
            }
            return LINK_TARGET_OK;
        }

        (void)arm_swd_write_dp_once(0x00, ARM_SWD_DP_ABORT_DAP_AND_CLR_ALL, 0);
        arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
    }

    return status;
}

static link_target_status_t arm_swd_power_up_debug(void)
{
    uint8_t retry;
    uint8_t ack = 0;
    uint32_t ctrl_stat = 0;
    link_target_status_t status;

    LINK_PERF_INC(powerup_calls);

    if (arm_swd_dap_write_phase_valid) {
        arm_swd_write_data_turnaround_cycles = arm_swd_dap_write_phase;
        if (arm_swd_dap_power_ready) {
            return LINK_TARGET_OK;
        }
    }
    status = arm_swd_write_dp_once(0x00, ARM_SWD_DP_ABORT_CLR_ALL, &ack);
    if ((status != LINK_TARGET_OK) && (status != LINK_TARGET_FAULT)) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_ABORT, ack);
        return status;
    }
    arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);

    ack = 0;
    status = arm_swd_write_dp_once(ARM_SWD_DP_SELECT, 0x00000000u, &ack);
    if (status == LINK_TARGET_FAULT) {
        uint8_t abort_ack = 0;

        arm_swd_write_dp_once(0x00, ARM_SWD_DP_ABORT_DAP_AND_CLR_ALL, &abort_ack);
        arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
        ack = 0;
        status = arm_swd_write_dp_once(ARM_SWD_DP_SELECT, 0x00000000u, &ack);
    }
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_SELECT, ack);
        return status;
    }
    if (arm_swd_dap_write_phase_valid) {
        arm_swd_dp_select = 0;
        arm_swd_dap_select_valid = 1;
    }
    arm_swd_idle_low_cycles(8);

    ack = 0;
    status = arm_swd_write_dp_once(ARM_SWD_DP_CTRL_STAT, ARM_SWD_DP_POWER_REQ, &ack);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_POWER_REQ, ack);
        return status;
    }

    for (retry = 0; retry < 20; retry++) {
        LINK_PERF_INC(powerup_polls);
        status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, 0);
        if (status != LINK_TARGET_OK) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_POWER_ACK, status);
            return status;
        }
        if ((ctrl_stat & ARM_SWD_DP_POWER_ACK) == ARM_SWD_DP_POWER_ACK) {
            arm_swd_dp_ready = 1;
            arm_swd_dap_power_ready = arm_swd_dap_write_phase_valid;
            return LINK_TARGET_OK;
        }
        DelayUs(200);
    }

    arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_POWER_ACK, ctrl_stat >> 24);
    return LINK_TARGET_WAIT;
}

static link_target_status_t arm_swd_mem_ap_prepare(void)
{
    link_target_status_t status;

    status = arm_swd_power_up_debug();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = arm_swd_select_ap_bank(ARM_SWD_MEM_AP_BANK0);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_SELECT, status);
        return status;
    }

    status = arm_swd_write_ap_reg(ARM_SWD_AP_CSW, ARM_SWD_MEM_AP_CSW_32);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_CSW, status);
    }
    return status;
}

static link_target_status_t arm_swd_mem_ap_prepare_size(uint32_t csw)
{
    link_target_status_t status;

    status = arm_swd_power_up_debug();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = arm_swd_select_ap_bank(ARM_SWD_MEM_AP_BANK0);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_SELECT, status);
        return status;
    }

    if (arm_swd_dap_write_phase_valid && arm_swd_dap_csw_valid &&
        (arm_swd_mem_ap_csw == csw)) {
        return LINK_TARGET_OK;
    }
    status = arm_swd_write_ap_reg(ARM_SWD_AP_CSW, csw);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_CSW, status);
    }
    return status;
}

static link_target_status_t arm_swd_abort_then_read_id(uint32_t *idcode, uint32_t abort_value, uint8_t *abort_ack, uint8_t *read_ack)
{
    link_target_status_t status;
    uint8_t retry_count;

    *abort_ack = 0;
    *read_ack = 0;

    arm_swd_write_dp_once(0x00, abort_value, abort_ack);
    arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);

    status = arm_swd_read_dp_once(0x00, idcode, read_ack);
    for (retry_count = 0; (status != LINK_TARGET_OK) && (retry_count < 3); retry_count++) {
        uint8_t retry_ack = 0;

        arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
        status = arm_swd_read_dp_once(0x00, idcode, &retry_ack);
        if (arm_swd_ack_rank(retry_ack) >= arm_swd_ack_rank(*read_ack)) {
            *read_ack = retry_ack;
        }
    }

    return status;
}

link_target_status_t target_arm_swd_dap_calibrate_write_phase(void)
{
    static const uint32_t csw_probe[] = { ARM_SWD_MEM_AP_CSW_16, ARM_SWD_MEM_AP_CSW_32 };
    uint8_t phase;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    arm_swd_dap_calibration_enabled = 1;
    arm_swd_dap_write_phase_valid = 0;
    arm_swd_dap_invalidate_context();
    arm_swd_synthetic_rdbuff_valid = 0;
    for (phase = 0; phase <= ARM_SWD_MAX_WRITE_DATA_TURNAROUND_CYCLES; phase++) {
        uint8_t probe;
        uint8_t retry;
        uint32_t ctrl_stat = 0;
        uint32_t actual_csw = 0;
        uint32_t actual_tar = 0;
        uint32_t posted = 0;

        if (phase != 0u) {
            LINK_PERF_INC(write_phase_retries);
        }
        arm_swd_write_data_turnaround_cycles = phase;
        status = arm_swd_write_dp_once(0x00, ARM_SWD_DP_ABORT_CLR_ALL, 0);
        if ((status != LINK_TARGET_OK) && (status != LINK_TARGET_FAULT)) {
            continue;
        }
        arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
        status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, 0);
        if ((status != LINK_TARGET_OK) || (ctrl_stat & ARM_SWD_DP_CTRL_STICKY_MASK)) {
            status = LINK_TARGET_FAULT;
            continue;
        }
        status = arm_swd_write_dp_once(ARM_SWD_DP_SELECT, 0, 0);
        if (status != LINK_TARGET_OK) {
            continue;
        }
        status = arm_swd_write_dp_once(ARM_SWD_DP_CTRL_STAT, ARM_SWD_DP_POWER_REQ, 0);
        if (status != LINK_TARGET_OK) {
            continue;
        }
        for (retry = 0; retry < 20u; retry++) {
            status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, 0);
            if ((status != LINK_TARGET_OK) || (ctrl_stat & ARM_SWD_DP_CTRL_STICKY_MASK) ||
                ((ctrl_stat & ARM_SWD_DP_POWER_ACK) == ARM_SWD_DP_POWER_ACK)) {
                break;
            }
            DelayUs(200);
        }
        if ((status != LINK_TARGET_OK) || (ctrl_stat & ARM_SWD_DP_CTRL_STICKY_MASK) ||
            ((ctrl_stat & ARM_SWD_DP_POWER_ACK) != ARM_SWD_DP_POWER_ACK)) {
            status = LINK_TARGET_FAULT;
            continue;
        }
        for (probe = 0; probe < sizeof(csw_probe) / sizeof(csw_probe[0]); probe++) {
            status = arm_swd_write_ap_once(ARM_SWD_AP_CSW, csw_probe[probe], 0);
            if (status != LINK_TARGET_OK) {
                break;
            }
            status = arm_swd_read_ap_once(ARM_SWD_AP_CSW, &posted, 0);
            if (status != LINK_TARGET_OK) {
                break;
            }
            status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &actual_csw, 0);
            if (status != LINK_TARGET_OK) {
                break;
            }
            status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, 0);
            if ((status != LINK_TARGET_OK) || (ctrl_stat & ARM_SWD_DP_CTRL_STICKY_MASK) ||
                ((actual_csw & 0x37u) != (csw_probe[probe] & 0x37u))) {
                status = LINK_TARGET_FAULT;
                break;
            }
        }
        if (status != LINK_TARGET_OK) {
            continue;
        }
        status = arm_swd_read_ap_once(ARM_SWD_AP_TAR, &posted, 0);
        if (status != LINK_TARGET_OK) {
            continue;
        }
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &actual_tar, 0);
        if (status != LINK_TARGET_OK) {
            continue;
        }
        status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, 0);
        if ((status != LINK_TARGET_OK) || (ctrl_stat & ARM_SWD_DP_CTRL_STICKY_MASK)) {
            status = LINK_TARGET_FAULT;
            continue;
        }
        arm_swd_dap_write_phase = phase;
        arm_swd_dap_write_phase_valid = 1;
        arm_swd_dap_power_ready = 1;
        arm_swd_dap_select_valid = 1;
        arm_swd_dap_csw_valid = 1;
        arm_swd_dp_select = 0;
        arm_swd_mem_ap_csw = actual_csw;
        arm_swd_mem_ap_tar = actual_tar;
        arm_swd_dp_ready = 1;
        return LINK_TARGET_OK;
    }
    arm_swd_dap_invalidate_context();
    return status;
}

void target_arm_swd_init(void)
{
    arm_swd_last_ack = 0;
    arm_swd_last_attempts = 0;
    arm_swd_last_dio_read_high = 0;
    arm_swd_last_dio_read_low = 0;
    arm_swd_last_dio_read_release = 0;
    arm_swd_last_strategy = 0;
    arm_swd_last_turnaround_cycles = 0;
    arm_swd_last_attach_mode = 0;
    arm_swd_last_ack_window = 0;
    arm_swd_last_mem_stage = 0;
    arm_swd_last_mem_detail = 0;
    arm_swd_half_cycle_delay_us = arm_swd_strategies[0].delay_us;
    arm_swd_sample_mode = arm_swd_strategies[0].sample_mode;
    arm_swd_sync_mode = arm_swd_strategies[0].sync_mode;
    arm_swd_turnaround_cycles = 1;
    arm_swd_write_data_turnaround_cycles = 0;
    arm_swd_dap_calibration_enabled = 0;
    arm_swd_dap_write_phase_valid = 0;
    arm_swd_dap_write_phase = 0;
    arm_swd_dap_invalidate_context();
    arm_swd_dp_ready = 0;
    arm_swd_pins = 0x80;
    arm_swd_last_dp_port = 0;
    arm_swd_last_dp_rnw = 0;
    arm_swd_last_dp_addr = 0;
    arm_swd_last_dp_status = 0;
    arm_swd_last_dp_ack = 0;
    arm_swd_last_dp_request = 0;
    arm_swd_dp_select = 0;
    arm_swd_mem_ap_tar = 0;
    arm_swd_mem_ap_csw = 0;
    arm_swd_last_dp_data = 0;
    arm_swd_synthetic_rdbuff_valid = 0;
    arm_swd_synthetic_rdbuff_data = 0;
    arm_swd_last_ap_post_addr = 0;
    arm_swd_last_ap_post_status = 0;
    arm_swd_last_ap_post_ack = 0;
    arm_swd_last_ap_post_request = 0;
    arm_swd_last_ap_post_data = 0;
    arm_swd_clk_low();
    arm_swd_dio_release();
    link_board_target_nrst_set(1);
}

link_target_status_t target_arm_swd_connect(void)
{
    target_arm_swd_init();
    link_board_target_nrst_set(0);
    DelayMs(2);
    link_board_target_nrst_set(1);
    DelayMs(ARM_SWD_CONNECT_SETTLE_MS);
    arm_swd_capture_line_diag();
    arm_swd_sync_sequence();
    return LINK_TARGET_OK;
}

void target_arm_swd_disconnect(void)
{
    arm_swd_dap_calibration_enabled = 0;
    arm_swd_dap_write_phase_valid = 0;
    arm_swd_dap_invalidate_context();
    arm_swd_dp_ready = 0;
    arm_swd_dp_select = 0;
    arm_swd_mem_ap_tar = 0;
    arm_swd_mem_ap_csw = 0;
    arm_swd_last_dp_port = 0;
    arm_swd_last_dp_rnw = 0;
    arm_swd_last_dp_addr = 0;
    arm_swd_last_dp_status = 0;
    arm_swd_last_dp_ack = 0;
    arm_swd_last_dp_request = 0;
    arm_swd_last_dp_data = 0;
    arm_swd_synthetic_rdbuff_valid = 0;
    arm_swd_synthetic_rdbuff_data = 0;
    arm_swd_last_ap_post_addr = 0;
    arm_swd_last_ap_post_status = 0;
    arm_swd_last_ap_post_ack = 0;
    arm_swd_last_ap_post_request = 0;
    arm_swd_last_ap_post_data = 0;
    arm_swd_clk_low();
    arm_swd_dio_release();
}

link_target_status_t target_arm_swd_reset(void)
{
    arm_swd_dap_invalidate_context();
    link_board_target_nrst_set(0);
    DelayMs(ARM_SWD_RESET_HOLD_MS);
    link_board_target_nrst_set(1);
    DelayMs(ARM_SWD_RESET_SETTLE_MS);
    return LINK_TARGET_OK;
}

void target_arm_swd_set_clock(uint32_t clock_hz)
{
#if LINK_S9_FAST_CLOCK
    if (clock_hz >= 1000000u) {
        arm_swd_half_cycle_delay_us = clock_hz >= 4000000u ? LINK_SWD_PAD_5MHZ : LINK_SWD_PAD_2MHZ;
        return;
    }
#endif
#if LINK_USB_USE_CHERRYUSB
    if (clock_hz >= 500000u) {
        arm_swd_half_cycle_delay_us = 1;
        return;
    }
#endif
    if (clock_hz >= 250000u) {
        arm_swd_half_cycle_delay_us = 2;
    } else if (clock_hz >= 100000u) {
        arm_swd_half_cycle_delay_us = 5;
    } else if (clock_hz >= 50000u) {
        arm_swd_half_cycle_delay_us = 10;
    } else {
        arm_swd_half_cycle_delay_us = 20;
    }
}

void target_arm_swd_configure(uint8_t config)
{
    uint8_t turnaround = (uint8_t)(config & 0x03u);

    if (turnaround > ARM_SWD_MAX_TURNAROUND_CYCLES) {
        turnaround = ARM_SWD_MAX_TURNAROUND_CYCLES;
    }
    if (arm_swd_dap_calibration_enabled && (turnaround != arm_swd_turnaround_cycles)) {
        /* Recalibration is explicit: never disturb an in-flight posted read here. */
        arm_swd_dap_write_phase_valid = 0;
        arm_swd_dap_invalidate_context();
    }
    arm_swd_turnaround_cycles = turnaround;
    arm_swd_last_turnaround_cycles = turnaround;
}

void target_arm_swd_swj_sequence(uint16_t bit_count, const uint8_t *data)
{
    uint16_t bit_index;

    if ((data == 0) || (bit_count == 0)) {
        return;
    }

    arm_swd_dap_invalidate_context();
    for (bit_index = 0; bit_index < bit_count; bit_index++) {
        uint8_t bit_val = (uint8_t)((data[bit_index >> 3] >> (bit_index & 0x07u)) & 0x01u);
        arm_swd_write_bit(bit_val);
    }
    arm_swd_dio_release();
}

void target_arm_swd_swd_sequence(uint8_t output, uint16_t bit_count, const uint8_t *out_data, uint8_t *in_data)
{
    uint16_t bit_index;
    uint16_t byte_count;

    if (bit_count == 0) {
        return;
    }

    arm_swd_dap_invalidate_context();
    byte_count = (uint16_t)((bit_count + 7u) >> 3);
    if (!output && (in_data != 0)) {
        uint16_t byte_index;

        for (byte_index = 0; byte_index < byte_count; byte_index++) {
            in_data[byte_index] = 0;
        }
    }

    if (output) {
        if (out_data == 0) {
            return;
        }
        for (bit_index = 0; bit_index < bit_count; bit_index++) {
            uint8_t bit_val = (uint8_t)((out_data[bit_index >> 3] >> (bit_index & 0x07u)) & 0x01u);
            arm_swd_write_bit(bit_val);
        }
    } else {
        if (in_data == 0) {
            return;
        }
        arm_swd_dio_release();
        for (bit_index = 0; bit_index < bit_count; bit_index++) {
            uint8_t bit_val = arm_swd_read_bit();
            if (bit_val) {
                in_data[bit_index >> 3] |= (uint8_t)(1u << (bit_index & 0x07u));
            }
        }
    }
}

uint8_t target_arm_swd_swj_pins(uint8_t pin_out, uint8_t pin_select)
{
    if (pin_select & 0x01u) {
        link_board_swd_clk_set(pin_out & 0x01u);
        if (pin_out & 0x01u) {
            arm_swd_pins |= 0x01u;
        } else {
            arm_swd_pins &= (uint8_t)~0x01u;
        }
    }

    if (pin_select & 0x02u) {
        if (pin_out & 0x02u) {
            arm_swd_dio_drive_high();
            arm_swd_pins |= 0x02u;
        } else {
            arm_swd_dio_drive_low();
            arm_swd_pins &= (uint8_t)~0x02u;
        }
    }

    if (pin_select & 0x80u) {
        arm_swd_dap_invalidate_context();
        link_board_target_nrst_set(pin_out & 0x80u);
        if (pin_out & 0x80u) {
            arm_swd_pins |= 0x80u;
        } else {
            arm_swd_pins &= (uint8_t)~0x80u;
        }
    }

    if (link_board_swd_dio_get()) {
        arm_swd_pins |= 0x02u;
    } else {
        arm_swd_pins &= (uint8_t)~0x02u;
    }

    return arm_swd_pins;
}

link_target_status_t target_arm_swd_dap_transfer(uint8_t request, uint32_t write_data, uint32_t *read_data)
{
    uint8_t addr = (uint8_t)(request & (ARM_SWD_DAP_TRANSFER_A2 | ARM_SWD_DAP_TRANSFER_A3));

    if (arm_swd_dap_calibration_enabled && !arm_swd_dap_write_phase_valid) {
        return LINK_TARGET_IO_ERROR;
    }
    if ((request & ARM_SWD_DAP_TRANSFER_RNW) && (read_data == 0)) {
        return LINK_TARGET_BAD_PARAM;
    }

    if (request & ARM_SWD_DAP_TRANSFER_APNDP) {
        if (request & ARM_SWD_DAP_TRANSFER_RNW) {
            return arm_swd_read_ap_reg(addr, read_data);
        }

        return arm_swd_dap_write_ap_with_retry(addr, write_data);
    }

    if (request & ARM_SWD_DAP_TRANSFER_RNW) {
        return arm_swd_read_dp_once(addr, read_data, 0);
    }

    return arm_swd_dap_write_dp_with_retry(addr, write_data);
}

#if LINK_USB_USE_CHERRYUSB
static link_target_status_t arm_swd_block_write_ap_wait(uint8_t addr, uint32_t value,
                                                       uint16_t wait_retry)
{
    uint32_t retry;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    for (retry = 0; retry <= wait_retry; retry++) {
        status = arm_swd_write_ap_once(addr, value, 0);
        if (status != LINK_TARGET_WAIT) {
            break;
        }
    }
    return status;
}

static link_target_status_t arm_swd_block_read_dp_wait(uint8_t addr, uint32_t *value,
                                                      uint16_t wait_retry)
{
    uint32_t retry;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    for (retry = 0; retry <= wait_retry; retry++) {
        status = arm_swd_read_dp_once(addr, value, 0);
        if (status != LINK_TARGET_WAIT) {
            break;
        }
    }
    return status;
}
link_target_status_t target_arm_swd_dap_write_block(uint8_t request,
                                                   const uint8_t *data,
                                                   uint16_t word_count,
                                                   uint16_t wait_retry,
                                                   uint16_t *done)
{
    if ((data == 0) || (done == 0)) {
        return LINK_TARGET_BAD_PARAM;
    }
    *done = 0;
    {
        const uint32_t sram_base = 0x20000000u;
        const uint32_t sram_end = 0x20040000u; /* Common STM32F1/F3/F4 SRAM window (up to 256KiB). */
        uint32_t addr = arm_swd_mem_ap_tar;
        uint32_t last_value = 0;
        uint32_t detail = addr;
        uint8_t stage = ARM_SWD_MEM_STAGE_TAR;
        link_target_status_t status;

        /* NOT_IMPLEMENTED is returned before any wire access, so the caller
         * can retain the proven per-item path for other transfer classes. */
        if ((request != 0x0Du) || (word_count < 2u) ||
            !arm_swd_dap_write_phase_valid || !arm_swd_dap_power_ready ||
            !arm_swd_dap_select_valid || !arm_swd_dap_csw_valid ||
            (arm_swd_dp_select != 0u) ||
            (arm_swd_mem_ap_csw != ARM_SWD_MEM_AP_CSW_32) ||
            ((addr & 3u) != 0u) || (addr < sram_base) || (addr >= sram_end) ||
            ((uint32_t)word_count * 4u > sram_end - addr)) {
            return LINK_TARGET_NOT_IMPLEMENTED;
        }

        while (*done < word_count) {
            uint32_t segment_words = (1024u - (addr & 1023u)) / 4u;
            uint32_t remaining = (uint32_t)word_count - *done;
            uint32_t i;
            uint32_t ctrl_stat = 0;

            if (segment_words > remaining) {
                segment_words = remaining;
            }
            stage = ARM_SWD_MEM_STAGE_TAR;
            detail = addr;
            status = arm_swd_block_write_ap_wait(ARM_SWD_AP_TAR, addr, wait_retry);
            if (status != LINK_TARGET_OK) {
                goto failed;
            }

            for (i = 0; i < segment_words; i++) {
                uint32_t offset = (uint32_t)*done * 4u;

                last_value = ((uint32_t)data[offset]) |
                             ((uint32_t)data[offset + 1u] << 8) |
                             ((uint32_t)data[offset + 2u] << 16) |
                             ((uint32_t)data[offset + 3u] << 24);
                stage = ARM_SWD_MEM_STAGE_DRW;
                detail = addr;
                status = arm_swd_block_write_ap_wait(ARM_SWD_AP_DRW, last_value, wait_retry);
                if (status != LINK_TARGET_OK) {
                    goto failed;
                }
                (*done)++;
                addr += 4u;
                arm_swd_mem_ap_tar = addr;
            }

            /* Complete this segment before changing TAR at a 1KiB wrap.
             * ACK counts are preserved even if final completion fails. */
            stage = ARM_SWD_MEM_STAGE_RDBUFF;
            detail = addr;
            status = arm_swd_block_read_dp_wait(ARM_SWD_DP_RDBUFF, &ctrl_stat, wait_retry);
            if (status != LINK_TARGET_OK) {
                goto failed;
            }
            stage = ARM_SWD_MEM_STAGE_DRW;
            status = arm_swd_block_read_dp_wait(ARM_SWD_DP_CTRL_STAT, &ctrl_stat, wait_retry);
            if (status != LINK_TARGET_OK) {
                goto failed;
            }
            if ((ctrl_stat & ARM_SWD_DP_CTRL_STICKY_MASK) != 0u) {
                detail = ctrl_stat;
                status = LINK_TARGET_FAULT;
                goto failed;
            }
        }
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, last_value);
        return LINK_TARGET_OK;

failed:
        arm_swd_dap_invalidate_context();
        arm_swd_set_mem_diag(stage, detail);
        return status;
    }
}
static link_target_status_t arm_swd_block_read_ap_wait(uint32_t *value,
                                                       uint16_t wait_retry)
{
    uint32_t retry;
    link_target_status_t status = LINK_TARGET_IO_ERROR;

    for (retry = 0; retry <= wait_retry; retry++) {
        status = arm_swd_read_ap_once(ARM_SWD_AP_DRW, value, 0);
        if (status != LINK_TARGET_WAIT) {
            break;
        }
    }
    arm_swd_capture_ap_post_diag(ARM_SWD_AP_DRW);
    return status;
}

link_target_status_t target_arm_swd_dap_read_block(uint8_t request,
                                                  uint8_t *data,
                                                  uint16_t word_count,
                                                  uint16_t wait_retry,
                                                  uint16_t *done)
{
    uint32_t addr = arm_swd_mem_ap_tar;
    uint32_t region_end = 0;
    uint32_t detail = addr;
    uint8_t stage = ARM_SWD_MEM_STAGE_TAR;
    link_target_status_t status;

    if ((data == 0) || (done == 0)) {
        return LINK_TARGET_BAD_PARAM;
    }
    *done = 0;
    if ((addr >= 0x20000000u) && (addr < 0x20040000u)) {
        region_end = 0x20040000u;
    } else if ((addr >= 0x08000000u) && (addr < 0x08200000u)) {
        region_end = 0x08200000u;
    }
    /* Ordinary STM32 SRAM/Flash memory enters this path. Bank1, peripherals
     * and incomplete contexts keep the existing per-item path. */
    if ((request != 0x0Fu) || (word_count < 2u) ||
        !arm_swd_dap_write_phase_valid || !arm_swd_dap_power_ready ||
        !arm_swd_dap_select_valid || !arm_swd_dap_csw_valid ||
        arm_swd_synthetic_rdbuff_valid || (arm_swd_dp_select != 0u) ||
        (arm_swd_mem_ap_csw != ARM_SWD_MEM_AP_CSW_32) ||
        ((addr & 3u) != 0u) || (region_end == 0u) ||
        ((uint32_t)word_count * 4u > region_end - addr)) {
        return LINK_TARGET_NOT_IMPLEMENTED;
    }

    while (*done < word_count) {
        uint32_t segment_words = (1024u - (addr & 1023u)) / 4u;
        uint32_t remaining = (uint32_t)word_count - *done;
        uint32_t i;
        uint32_t value = 0;

        if (segment_words > remaining) {
            segment_words = remaining;
        }
        stage = ARM_SWD_MEM_STAGE_TAR;
        detail = addr;
        status = arm_swd_block_write_ap_wait(ARM_SWD_AP_TAR, addr, wait_retry);
        if (status != LINK_TARGET_OK) {
            goto failed;
        }
        for (i = 0; i < segment_words; i++) {
            stage = ARM_SWD_MEM_STAGE_DRW;
            detail = addr;
            arm_swd_last_ap_read_tar = addr;
            status = arm_swd_block_read_ap_wait(&value, wait_retry);
            if (status != LINK_TARGET_OK) {
                goto failed;
            }
            addr += 4u;
            arm_swd_mem_ap_tar = addr;
            if (i != 0u) {
                uint32_t offset = (uint32_t)*done * 4u;

                /* Each AP read returns the preceding word. The first response
                 * belongs to the old pipeline and must be discarded. */
                data[offset] = (uint8_t)value;
                data[offset + 1u] = (uint8_t)(value >> 8);
                data[offset + 2u] = (uint8_t)(value >> 16);
                data[offset + 3u] = (uint8_t)(value >> 24);
                (*done)++;
            }
        }
        stage = ARM_SWD_MEM_STAGE_RDBUFF;
        detail = arm_swd_last_ap_read_tar;
        status = arm_swd_block_read_dp_wait(ARM_SWD_DP_RDBUFF, &value, wait_retry);
        if (status != LINK_TARGET_OK) {
            goto failed;
        }
        {
            uint32_t offset = (uint32_t)*done * 4u;

            data[offset] = (uint8_t)value;
            data[offset + 1u] = (uint8_t)(value >> 8);
            data[offset + 2u] = (uint8_t)(value >> 16);
            data[offset + 3u] = (uint8_t)(value >> 24);
            (*done)++;
        }
        /* Complete the posted read before changing TAR at a 1KiB boundary. */
        status = arm_swd_block_read_dp_wait(ARM_SWD_DP_CTRL_STAT, &value, wait_retry);
        if (status != LINK_TARGET_OK) {
            goto failed;
        }
        if ((value & ARM_SWD_DP_CTRL_STICKY_MASK) != 0u) {
            detail = value;
            status = LINK_TARGET_FAULT;
            goto failed;
        }
    }
    arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, arm_swd_last_ap_read_tar);
    return LINK_TARGET_OK;

failed:
    arm_swd_dap_invalidate_context();
    arm_swd_set_mem_diag(stage, detail);
    return status;
}

#endif

static uint8_t arm_swd_try_synthetic_drw_read(uint8_t ap_addr, uint32_t *value)
{
#if LINK_SWD_SYNTHETIC_M3_IDS
    /* Synthetic reads are a narrow Keil enumeration aid for ROM table/component
     * ID/CPUID only. They must not be treated as proof that real AHB access works. */
    if ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANKF) {
        if (ap_addr == ARM_SWD_AP_BASE) {
            *value = ARM_SWD_MEM_AP_BASE_FALLBACK;
            return 1;
        }
        if (ap_addr == ARM_SWD_AP_DRW) {
            *value = ARM_SWD_MEM_AP_IDR_FALLBACK;
            return 1;
        }
        return 0;
    }
#endif

#if !LINK_USB_USE_CHERRYUSB
    if (arm_swd_dap_bank1_data_addr(ap_addr, 0)) {
        return arm_swd_dap_bank1_read32(ap_addr, value) == LINK_TARGET_OK;
    }
#endif

    if ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) != ARM_SWD_MEM_AP_BANK0) {
        return 0;
    }

    if ((ap_addr == ARM_SWD_AP_CSW) &&
        ((arm_swd_mem_ap_csw & (ARM_SWD_AP_CSW_ADDRINC_MASK | ARM_SWD_AP_CSW_SIZE_MASK)) !=
         (ARM_SWD_AP_CSW_ADDRINC_SINGLE | ARM_SWD_AP_CSW_SIZE_32))) {
        *value = arm_swd_mem_ap_csw;
        return 1;
    }

#if LINK_SWD_SYNTHETIC_M3_IDS
    if (ap_addr != ARM_SWD_AP_DRW) {
        return 0;
    }

    if (arm_swd_rom_table_read_fallback(arm_swd_mem_ap_tar, value) ||
        arm_swd_rom_component_id_read_fallback(arm_swd_mem_ap_tar, value) ||
        arm_swd_scs_register_read_fallback(arm_swd_mem_ap_tar, value)) {
        return 1;
    }
#endif

    return 0;
}

link_target_status_t target_arm_swd_dap_ap_read_post(uint8_t addr, uint32_t *posted_data)
{
    uint8_t ap_addr = (uint8_t)(addr & (ARM_SWD_DAP_TRANSFER_A2 | ARM_SWD_DAP_TRANSFER_A3));
    uint8_t had_synthetic = arm_swd_synthetic_rdbuff_valid;
    uint32_t synthetic_data = arm_swd_synthetic_rdbuff_data;
    uint8_t current_is_synthetic;
    uint8_t real_mem32_drw_read;
    uint32_t current_synthetic_data = 0;
    link_target_status_t status;

    if (arm_swd_dap_calibration_enabled && !arm_swd_dap_write_phase_valid) {
        return LINK_TARGET_IO_ERROR;
    }
#if LINK_USB_USE_CHERRYUSB
    if (arm_swd_dap_bank1_data_addr(ap_addr, 0)) {
        /* Completed native reads use the existing software posted buffer.
         * Propagate failure instead of issuing a second access to hide it. */
        status = arm_swd_dap_bank1_read32(ap_addr, &current_synthetic_data);
        if (status != LINK_TARGET_OK) {
            return status;
        }
        current_is_synthetic = 1;
    } else {
        current_is_synthetic = arm_swd_try_synthetic_drw_read(ap_addr, &current_synthetic_data);
    }
#else
    current_is_synthetic = arm_swd_try_synthetic_drw_read(ap_addr, &current_synthetic_data);
#endif
    real_mem32_drw_read = (((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0) &&
                           (ap_addr == ARM_SWD_AP_DRW) &&
                           ((arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_SIZE_MASK) == ARM_SWD_AP_CSW_SIZE_32) &&
                           ((arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_ADDRINC_MASK) == ARM_SWD_AP_CSW_ADDRINC_SINGLE));

    if (had_synthetic && (posted_data != 0)) {
        *posted_data = synthetic_data;
    } else if (current_is_synthetic && (posted_data != 0)) {
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, posted_data, 0);
        if (status != LINK_TARGET_OK) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF, ARM_SWD_DP_RDBUFF);
            arm_swd_synthetic_rdbuff_valid = 0;
            return status;
        }
    } else if (posted_data != 0) {
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, posted_data, 0);
        if (status != LINK_TARGET_OK) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF, ARM_SWD_DP_RDBUFF);
            return status;
        }
    }

    if (current_is_synthetic) {
        arm_swd_synthetic_rdbuff_data = current_synthetic_data;
        if (((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0) &&
            (ap_addr == ARM_SWD_AP_DRW)) {
            arm_swd_mem_ap_tar += 4u;
        }
        arm_swd_synthetic_rdbuff_valid = 1;
        arm_swd_set_transfer_diag(1, 1, ap_addr, ARM_SWD_ACK_OK, LINK_TARGET_OK, current_synthetic_data);
        return LINK_TARGET_OK;
    }

    arm_swd_synthetic_rdbuff_valid = 0;
    if (real_mem32_drw_read) {
        uint32_t read_tar = arm_swd_mem_ap_tar;
        link_target_status_t prep_status;

        (void)arm_swd_dap_write_dp_with_retry(0x00, ARM_SWD_DP_ABORT_CLR_STKCMP);
        arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
        prep_status = arm_swd_dap_write_ap_with_retry(ARM_SWD_AP_TAR, read_tar);
        if (prep_status != LINK_TARGET_OK) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_TAR, read_tar);
            return prep_status;
        }
    }
    if (real_mem32_drw_read) {
        arm_swd_last_ap_read_tar = arm_swd_mem_ap_tar;
    }
    status = arm_swd_read_ap_once(ap_addr, &arm_swd_last_dp_data, 0);
    arm_swd_capture_ap_post_diag(ap_addr);
    if (status != LINK_TARGET_OK) {
        uint8_t stage = ARM_SWD_MEM_STAGE_DRW;

        if (ap_addr == ARM_SWD_AP_CSW) {
            stage = ARM_SWD_MEM_STAGE_CSW;
        } else if (ap_addr == ARM_SWD_AP_TAR) {
            stage = ARM_SWD_MEM_STAGE_TAR;
        }
        arm_swd_set_mem_diag(stage, ap_addr);
    } else if (real_mem32_drw_read) {
        arm_swd_mem_ap_tar += 4u;
    }
    return status;
}

link_target_status_t target_arm_swd_dap_read_rdbuff(uint32_t *read_data)
{
    link_target_status_t status;
    uint8_t retry_mem_read = 0;
    uint32_t retry_tar = arm_swd_last_ap_read_tar;

    if (read_data == 0) {
        return LINK_TARGET_BAD_PARAM;
    }

    if (arm_swd_dap_calibration_enabled && !arm_swd_dap_write_phase_valid) {
        return LINK_TARGET_IO_ERROR;
    }
    if (arm_swd_synthetic_rdbuff_valid) {
        arm_swd_synthetic_rdbuff_valid = 0;
        *read_data = arm_swd_synthetic_rdbuff_data;
        arm_swd_set_transfer_diag(0, 1, ARM_SWD_DP_RDBUFF, ARM_SWD_ACK_OK, LINK_TARGET_OK, *read_data);
        return LINK_TARGET_OK;
    }

    status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, read_data, 0);
    if ((status != LINK_TARGET_OK) &&
        ((arm_swd_dp_select & ARM_SWD_DP_SELECT_APBANKSEL_MASK) == ARM_SWD_MEM_AP_BANK0) &&
        ((arm_swd_mem_ap_csw & ARM_SWD_AP_CSW_SIZE_MASK) == ARM_SWD_AP_CSW_SIZE_32)) {
        LINK_PERF_INC(rdbuff_recoveries);
        (void)arm_swd_dap_write_dp_with_retry(0x00, ARM_SWD_DP_ABORT_CLR_STKCMP);
        arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
        if (arm_swd_dap_write_ap_with_retry(ARM_SWD_AP_TAR, retry_tar) == LINK_TARGET_OK) {
            uint32_t posted = 0;

            arm_swd_mem_ap_tar = retry_tar;
            status = arm_swd_read_ap_once(ARM_SWD_AP_DRW, &posted, 0);
            arm_swd_capture_ap_post_diag(ARM_SWD_AP_DRW);
            if (status == LINK_TARGET_OK) {
                status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, read_data, 0);
                retry_mem_read = 1;
            }
        }
    }
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF, ARM_SWD_DP_RDBUFF);
    } else if (retry_mem_read) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, *read_data);
    }
    return status;
}

link_target_status_t target_arm_swd_read_idcode(uint32_t *idcode)
{
    uint8_t strategy_index;
    uint8_t turnaround_cycles;
    uint8_t attach_mode;
    uint8_t retry_index;
    uint8_t best_ack = 0;
    uint8_t best_first_ack = 0;
    uint8_t best_abort_ack = 0;
    uint8_t best_read_ack = 0;
    uint8_t best_strategy = 0;
    uint8_t best_attach_mode = 0;
    uint8_t best_turnaround_cycles = 1;
    uint8_t best_ack_window = 0;
    uint8_t best_read_rank = 0;
    uint8_t best_abort_rank = 0;
    uint8_t best_first_rank = 0;
    link_target_status_t last_status = LINK_TARGET_IO_ERROR;

    if (idcode == 0) {
        return LINK_TARGET_IO_ERROR;
    }

    arm_swd_last_ack = 0;
    arm_swd_last_attempts = 0;
    arm_swd_turnaround_cycles = 1;
    arm_swd_last_turnaround_cycles = 1;
    arm_swd_last_attach_mode = 0;
    arm_swd_capture_line_diag();

    for (strategy_index = 0; strategy_index < (sizeof(arm_swd_strategies) / sizeof(arm_swd_strategies[0])); strategy_index++) {
        for (turnaround_cycles = ARM_SWD_MIN_TURNAROUND_CYCLES; turnaround_cycles <= ARM_SWD_MAX_TURNAROUND_CYCLES; turnaround_cycles++) {
            for (attach_mode = ARM_SWD_ATTACH_PLAIN; attach_mode <= ARM_SWD_ATTACH_UNDER_RESET; attach_mode++) {
                for (retry_index = 0; retry_index < ARM_SWD_READ_RETRIES; retry_index++) {
                    uint8_t first_ack = 0;
                    uint8_t abort_ack = 0;
                    uint8_t read_ack = 0;
                    uint8_t attempt_ack;
                    uint8_t attempt_rank;
                    uint32_t first_idcode = 0;

                    arm_swd_apply_strategy(strategy_index);
                    arm_swd_turnaround_cycles = turnaround_cycles;
                    arm_swd_last_turnaround_cycles = turnaround_cycles;
                    arm_swd_last_attach_mode = attach_mode;
                    arm_swd_attach_sequence(attach_mode);

                    last_status = arm_swd_read_dp_once(0x00, &first_idcode, &first_ack);
                    arm_swd_last_attempts++;
                    if (last_status == LINK_TARGET_OK) {
                        *idcode = first_idcode;
                        arm_swd_dp_ready = 1;
                        arm_swd_last_ack = first_ack;
                        arm_swd_last_dio_read_high = first_ack;
                        arm_swd_last_dio_read_low = first_ack;
                        return LINK_TARGET_OK;
                    }

                    last_status = arm_swd_abort_then_read_id(idcode, ARM_SWD_DP_ABORT_CLR_ALL, &abort_ack, &read_ack);
                    if (last_status != LINK_TARGET_OK) {
                        uint8_t dap_abort_ack = 0;
                        uint8_t dap_read_ack = 0;

                        last_status = arm_swd_abort_then_read_id(idcode, ARM_SWD_DP_ABORT_DAP_AND_CLR_ALL,
                                                                 &dap_abort_ack, &dap_read_ack);
                        if (arm_swd_ack_rank(dap_read_ack) >= arm_swd_ack_rank(read_ack)) {
                            abort_ack = dap_abort_ack;
                            read_ack = dap_read_ack;
                        }
                    }

                    if (last_status != LINK_TARGET_OK) {
                        uint8_t select_ack = 0;
                        uint8_t select_read_ack = 0;

                        arm_swd_write_dp_once(0x08, 0x00000000u, &select_ack);
                        arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
                        last_status = arm_swd_read_dp_once(0x00, idcode, &select_read_ack);
                        if (arm_swd_ack_rank(select_read_ack) >= arm_swd_ack_rank(read_ack)) {
                            abort_ack = select_ack;
                            read_ack = select_read_ack;
                        }
                    }

                    attempt_ack = read_ack;
                    attempt_rank = arm_swd_ack_rank(read_ack);
                    if ((attempt_rank > best_read_rank) ||
                        ((attempt_rank == best_read_rank) && (arm_swd_ack_rank(abort_ack) > best_abort_rank)) ||
                        ((attempt_rank == best_read_rank) && (arm_swd_ack_rank(abort_ack) == best_abort_rank) &&
                         (arm_swd_ack_rank(first_ack) > best_first_rank))) {
                        best_read_rank = attempt_rank;
                        best_abort_rank = arm_swd_ack_rank(abort_ack);
                        best_first_rank = arm_swd_ack_rank(first_ack);
                        best_ack = attempt_ack;
                        best_first_ack = first_ack;
                        best_abort_ack = abort_ack;
                        best_read_ack = read_ack;
                        best_strategy = strategy_index;
                        best_attach_mode = attach_mode;
                        best_turnaround_cycles = turnaround_cycles;
                        best_ack_window = read_ack;
                    }

                    arm_swd_last_dio_read_high = first_ack;
                    arm_swd_last_dio_read_low = abort_ack;
                    arm_swd_last_dio_read_release = read_ack;

                    if (last_status == LINK_TARGET_OK) {
                        arm_swd_dp_ready = 1;
                        arm_swd_last_ack = read_ack;
                        arm_swd_last_strategy = strategy_index;
                        arm_swd_last_attach_mode = attach_mode;
                        arm_swd_last_turnaround_cycles = turnaround_cycles;
                        return LINK_TARGET_OK;
                    }

                    DelayUs(200);
                }
            }
        }
    }

    arm_swd_last_ack = best_ack;
    arm_swd_last_dio_read_high = best_first_ack;
    arm_swd_last_dio_read_low = best_abort_ack;
    arm_swd_last_dio_read_release = best_read_ack;
    arm_swd_last_strategy = best_strategy;
    arm_swd_last_attach_mode = best_attach_mode;
    arm_swd_last_turnaround_cycles = best_turnaround_cycles;
    arm_swd_last_ack_window = best_ack_window;

    return last_status;
}

static link_target_status_t arm_swd_mem_ap_prepare_with_retry(uint32_t csw)
{
    link_target_status_t status;
    uint8_t write_turnaround;

    if (arm_swd_dap_calibration_enabled) {
        if (!arm_swd_dap_write_phase_valid) {
            return LINK_TARGET_IO_ERROR;
        }
        arm_swd_write_data_turnaround_cycles = arm_swd_dap_write_phase;
        return arm_swd_mem_ap_prepare_size(csw);
    }
    for (write_turnaround = 0; write_turnaround <= ARM_SWD_MAX_WRITE_DATA_TURNAROUND_CYCLES; write_turnaround++) {
        arm_swd_write_data_turnaround_cycles = write_turnaround;
        status = arm_swd_mem_ap_prepare_size(csw);
        if (status == LINK_TARGET_OK) {
            return LINK_TARGET_OK;
        }
    }

    return status;
}

static link_target_status_t arm_swd_ensure_dp_ready(void)
{
    uint32_t idcode;

    if (arm_swd_dp_ready) {
        return LINK_TARGET_OK;
    }

    arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_READ_ID, 0);
#if LINK_BLE_ENABLE
    if (arm_swd_dap_calibration_enabled) return LINK_TARGET_NOT_CONNECTED;
#endif
    return target_arm_swd_read_idcode(&idcode);
}

#if LINK_BLE_ENABLE
static link_target_status_t arm_swd_ble_checked(link_target_status_t status)
{
    uint32_t ctrl;
    if (status != LINK_TARGET_OK) return status;
    status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &ctrl, 0);
    if (status == LINK_TARGET_OK && (ctrl & ARM_SWD_DP_CTRL_STICKY_MASK)) return LINK_TARGET_FAULT;
    return status;
}
#else
#define arm_swd_ble_checked(s) (s)
#endif

static link_target_status_t arm_swd_read32_prepared(uint32_t addr, uint32_t *value)
{
    link_target_status_t status;

    status = arm_swd_write_ap_reg(ARM_SWD_AP_TAR, addr);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_TAR, addr);
        return status;
    }

    status = arm_swd_read_ap_reg(ARM_SWD_AP_DRW, value);
    if (status == LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, *value);
    }
    return arm_swd_ble_checked(status);
}

static link_target_status_t arm_swd_write32_prepared(uint32_t addr, uint32_t value)
{
    uint32_t posted;
    link_target_status_t status;

    status = arm_swd_write_ap_reg(ARM_SWD_AP_TAR, addr);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_TAR, addr);
        return status;
    }

    status = arm_swd_write_ap_reg(ARM_SWD_AP_DRW, value);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_DRW, addr);
        return status;
    }

    status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &posted, 0);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF, status);
    } else {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, value);
    }
    return arm_swd_ble_checked(status);
}

static link_target_status_t arm_swd_write16_prepared(uint32_t addr, uint16_t value)
{
    uint32_t posted;
    uint32_t drw_value;
    link_target_status_t status;

    status = arm_swd_write_ap_reg(ARM_SWD_AP_TAR, addr);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_TAR, addr);
        return status;
    }

    drw_value = (uint32_t)value;
    if ((addr & 0x02u) != 0) {
        drw_value <<= 16;
    }

    status = arm_swd_write_ap_reg(ARM_SWD_AP_DRW, drw_value);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_WRITE16, addr);
        return status;
    }

    status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &posted, 0);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF, status);
    } else {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, value);
    }
    return arm_swd_ble_checked(status);
}

link_target_status_t target_arm_swd_read32_block(uint32_t addr, uint32_t *values, uint8_t word_count)
{
    uint8_t word_index;
    link_target_status_t status;

    if ((values == 0) || (word_count == 0) || (word_count > 15) || ((addr & 0x03u) != 0)) {
        return LINK_TARGET_BAD_PARAM;
    }

    status = arm_swd_ensure_dp_ready();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = arm_swd_mem_ap_prepare_with_retry(ARM_SWD_MEM_AP_CSW_32);
    if (status != LINK_TARGET_OK) {
        return status;
    }

    if (word_count == 1) {
        return arm_swd_read32_prepared(addr, &values[0]);
    }

    status = arm_swd_write_ap_reg(ARM_SWD_AP_TAR, addr);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_TAR, addr);
        return status;
    }

    for (word_index = 0; word_index < word_count; word_index++) {
        status = arm_swd_read_ap_reg(ARM_SWD_AP_DRW, &values[word_index]);
        if (status != LINK_TARGET_OK) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_DRW, addr + ((uint32_t)word_index * 4u));
            return status;
        }
    }

    arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, values[word_count - 1]);
    return LINK_TARGET_OK;
}

link_target_status_t target_arm_swd_read32(uint32_t addr, uint32_t *value)
{
    return target_arm_swd_read32_block(addr, value, 1);
}

link_target_status_t target_arm_swd_write32(uint32_t addr, uint32_t value)
{
    link_target_status_t status;

    if ((addr & 0x03u) != 0) {
        return LINK_TARGET_BAD_PARAM;
    }

#if LINK_USB_USE_CHERRYUSB
    /* The DAP session already owns a calibrated, powered AP0/CSW32.
     * Keep TAR -> DRW -> RDBUFF; omit redundant connect/prepare calls.
     * The DAP caller still checks CTRL/STAT after this write completes. */
    if (arm_swd_dap_write_phase_valid && arm_swd_dap_power_ready &&
        arm_swd_dap_select_valid && arm_swd_dap_csw_valid &&
        (arm_swd_dp_select == 0u) &&
        (arm_swd_mem_ap_csw == ARM_SWD_MEM_AP_CSW_32)) {
        return arm_swd_write32_prepared(addr, value);
    }
#endif

    status = arm_swd_ensure_dp_ready();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = arm_swd_mem_ap_prepare_with_retry(ARM_SWD_MEM_AP_CSW_32);
    if (status != LINK_TARGET_OK) {
        return status;
    }

    return arm_swd_write32_prepared(addr, value);
}

LINK_SWD_LOCAL link_target_status_t target_arm_swd_write16(uint32_t addr, uint16_t value)
{
    link_target_status_t status;

    if ((addr & 0x01u) != 0) {
        return LINK_TARGET_BAD_PARAM;
    }

    status = arm_swd_ensure_dp_ready();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = arm_swd_mem_ap_prepare_with_retry(ARM_SWD_MEM_AP_CSW_16);
    if (status != LINK_TARGET_OK) {
        return status;
    }

    return arm_swd_write16_prepared(addr, value);
}

static link_target_status_t stm32f1_flash_wait(void)
{
    uint16_t retry;
    uint32_t sr = 0;
    link_target_status_t status;

    for (retry = 0; retry < 5000; retry++) {
        status = target_arm_swd_read32(STM32F1_FLASH_SR, &sr);
        if (status != LINK_TARGET_OK) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_WAIT, status);
            return status;
        }

        if ((sr & STM32F1_FLASH_SR_BSY) == 0) {
            if ((sr & STM32F1_FLASH_SR_ERRS) != 0) {
                arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_WAIT, sr);
                (void)target_arm_swd_write32(STM32F1_FLASH_SR, STM32F1_FLASH_SR_EOP | STM32F1_FLASH_SR_ERRS);
                return LINK_TARGET_FAULT;
            }
            return LINK_TARGET_OK;
        }

        DelayUs(100);
    }

    arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_WAIT, sr);
    return LINK_TARGET_WAIT;
}

static link_target_status_t stm32f1_flash_unlock(void)
{
    uint32_t cr = 0;
    link_target_status_t status;

    status = stm32f1_flash_wait();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = target_arm_swd_read32(STM32F1_FLASH_CR, &cr);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_UNLOCK, status);
        return status;
    }

    if ((cr & STM32F1_FLASH_CR_LOCK) == 0) {
        return LINK_TARGET_OK;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_KEYR, STM32F1_FLASH_KEY1);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_UNLOCK, 1);
        return status;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_KEYR, STM32F1_FLASH_KEY2);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_UNLOCK, 2);
        return status;
    }

    status = target_arm_swd_read32(STM32F1_FLASH_CR, &cr);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_UNLOCK, status);
        return status;
    }
    if ((cr & STM32F1_FLASH_CR_LOCK) != 0) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_UNLOCK, cr);
        return LINK_TARGET_FAULT;
    }

    return LINK_TARGET_OK;
}

link_target_status_t target_arm_swd_flash_erase_page(uint32_t addr)
{
    link_target_status_t status;

    if ((addr < STM32F1_FLASH_BASE) || (addr >= STM32F103C8_FLASH_END) ||
        ((addr % STM32F1_FLASH_PAGE_SIZE) != 0)) {
        return LINK_TARGET_BAD_PARAM;
    }

    status = stm32f1_flash_unlock();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_SR, STM32F1_FLASH_SR_EOP | STM32F1_FLASH_SR_ERRS);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_WAIT, status);
        return status;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_CR, STM32F1_FLASH_CR_PER);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_CR, status);
        return status;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_AR, addr);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_AR, status);
        return status;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_CR, STM32F1_FLASH_CR_PER | STM32F1_FLASH_CR_STRT);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_CR, status);
        return status;
    }

    status = stm32f1_flash_wait();
    (void)target_arm_swd_write32(STM32F1_FLASH_CR, 0);
    return status;
}

link_target_status_t target_arm_swd_flash_write(uint32_t addr, const uint8_t *data, uint8_t len)
{
    uint8_t offset;
    link_target_status_t status;

    if ((data == 0) || (len == 0) || ((len & 0x01u) != 0) || ((addr & 0x01u) != 0) ||
        (addr < STM32F1_FLASH_BASE) || (addr >= STM32F103C8_FLASH_END) ||
        (((uint32_t)addr + len) > STM32F103C8_FLASH_END)) {
        return LINK_TARGET_BAD_PARAM;
    }

    status = stm32f1_flash_unlock();
    if (status != LINK_TARGET_OK) {
        return status;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_SR, STM32F1_FLASH_SR_EOP | STM32F1_FLASH_SR_ERRS);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_WAIT, status);
        return status;
    }

    status = target_arm_swd_write32(STM32F1_FLASH_CR, STM32F1_FLASH_CR_PG);
    if (status != LINK_TARGET_OK) {
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_FLASH_CR, status);
        return status;
    }

    for (offset = 0; offset < len; offset += 2) {
        uint16_t halfword = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);

        status = target_arm_swd_write16(addr + offset, halfword);
        if (status != LINK_TARGET_OK) {
            (void)target_arm_swd_write32(STM32F1_FLASH_CR, 0);
            return status;
        }

        status = stm32f1_flash_wait();
        if (status != LINK_TARGET_OK) {
            (void)target_arm_swd_write32(STM32F1_FLASH_CR, 0);
            return status;
        }
    }

    (void)target_arm_swd_write32(STM32F1_FLASH_CR, 0);
    return LINK_TARGET_OK;
}

void target_arm_swd_get_diag(link_target_diag_t *diag)
{
    if (diag == 0) {
        return;
    }

    diag->raw_ack = arm_swd_last_ack;
    diag->attempts = arm_swd_last_attempts;
    diag->dio_read_high = arm_swd_last_dio_read_high;
    diag->dio_read_low = arm_swd_last_dio_read_low;
    diag->dio_read_release = arm_swd_last_dio_read_release;
    diag->strategy = (uint8_t)(arm_swd_last_strategy | (arm_swd_last_attach_mode << 4));
    diag->turnaround = arm_swd_last_turnaround_cycles;
    diag->ack_window = arm_swd_last_ack_window;
    if (arm_swd_last_mem_stage != 0) {
        diag->dio_read_high = arm_swd_last_mem_stage;
        diag->dio_read_low = (uint8_t)(arm_swd_last_mem_detail & 0xFF);
        diag->dio_read_release = arm_swd_last_ack_window;
    }
}

void target_arm_swd_get_debug_diag(target_arm_swd_debug_diag_t *diag)
{
    if (diag == 0) {
        return;
    }

    diag->mem_stage = arm_swd_last_mem_stage;
    diag->ack_window = arm_swd_last_ack_window;
    diag->turnaround = arm_swd_turnaround_cycles;
    diag->write_turnaround = arm_swd_write_data_turnaround_cycles;
    diag->dp_port = arm_swd_last_dp_port;
    diag->dp_rnw = arm_swd_last_dp_rnw;
    diag->dp_addr = arm_swd_last_dp_addr;
    diag->dp_status = arm_swd_last_dp_status;
    diag->dp_ack = arm_swd_last_dp_ack;
    diag->dp_request = arm_swd_last_dp_request;
    diag->mem_detail = arm_swd_last_mem_detail;
    diag->dp_select = arm_swd_dp_select;
    diag->mem_ap_tar = arm_swd_mem_ap_tar;
    diag->mem_ap_csw = arm_swd_mem_ap_csw;
    diag->dp_data = arm_swd_last_dp_data;
    diag->ap_post_addr = arm_swd_last_ap_post_addr;
    diag->ap_post_status = arm_swd_last_ap_post_status;
    diag->ap_post_ack = arm_swd_last_ap_post_ack;
    diag->ap_post_request = arm_swd_last_ap_post_request;
    diag->ap_post_data = arm_swd_last_ap_post_data;
}

#if LINK_BLE_ENABLE
/* The fixed, physically verified wire phases are shared by BLE and the USB
 * queue in this build. Progress yields between operations; ACK WAIT retries
 * only the unaccepted transaction, never a successfully accepted write. */
static uint8_t ble_attach_step;
static uint32_t ble_attach_due;
static target_arm_swd_ble_attach_diag_t ble_attach_diag;
void target_arm_swd_ble_attach_cancel(void) { ble_attach_step = 0; }
void target_arm_swd_ble_get_attach_diag(target_arm_swd_ble_attach_diag_t *diag)
{
    if (diag) *diag = ble_attach_diag;
}
link_target_status_t target_arm_swd_ble_attach(uint32_t *idcode)
{
    uint32_t value = 0;
    link_target_status_t status;
    if (!idcode) return LINK_TARGET_BAD_PARAM;
    if (ble_attach_step == 0) {
        target_arm_swd_init();
        target_arm_swd_set_clock(250000u);
        arm_swd_turnaround_cycles = arm_swd_last_turnaround_cycles = 0;
        arm_swd_write_data_turnaround_cycles = 2;
        arm_swd_dap_write_phase = 2;
        arm_swd_dap_write_phase_valid = 1;
        arm_swd_dap_calibration_enabled = 1;
        ble_attach_diag.idcode = ble_attach_diag.ctrl_stat = ble_attach_diag.csw = 0;
        ble_attach_diag.step = 1;
        ble_attach_diag.status = LINK_TARGET_WAIT;
        arm_swd_sync_sequence();
        /* Match the stable plain attach settle time without blocking TMOS. */
        ble_attach_due = link_ble_now_ms() + ARM_SWD_POST_CONNECT_DELAY_MS;
        ble_attach_step = 1;
        return LINK_TARGET_WAIT;
    }
    if (ble_attach_step == 1) {
        if ((int32_t)(link_ble_now_ms() - ble_attach_due) < 0) return LINK_TARGET_WAIT;
        ble_attach_step = 2;
    }
    ble_attach_diag.step = ble_attach_step;
    switch (ble_attach_step) {
    case 2:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_READ_ID, 0);
        status = arm_swd_read_dp_once(0, &ble_attach_diag.idcode, 0);
        if (status == LINK_TARGET_OK) arm_swd_dp_ready = 1;
        break;
    case 3:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_ABORT, 0);
        status = arm_swd_write_dp_once(0, ARM_SWD_DP_ABORT_CLR_ALL, 0);
        if (status == LINK_TARGET_OK || status == LINK_TARGET_FAULT) {
            /* ABORT's data phase is already completed by write_dp_once even
             * on FAULT. Verify real CTRL/STAT next, as the stable USB path does. */
            arm_swd_idle_low_cycles(ARM_SWD_POST_ABORT_IDLE_CYCLES);
            status = LINK_TARGET_OK;
        }
        break;
    case 4:
        status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &value, 0);
        if (status == LINK_TARGET_OK) {
            ble_attach_diag.ctrl_stat = value;
            if (value & ARM_SWD_DP_CTRL_STICKY_MASK) status = LINK_TARGET_FAULT;
        }
        break;
    case 5:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_SELECT, 0);
        status = arm_swd_write_dp_once(ARM_SWD_DP_SELECT, 0, 0);
        break;
    case 6:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_POWER_REQ, 0);
        status = arm_swd_write_dp_once(ARM_SWD_DP_CTRL_STAT, ARM_SWD_DP_POWER_REQ, 0);
        break;
    case 7:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_POWER_ACK, 0);
        status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &value, 0);
        if (status == LINK_TARGET_OK) {
            ble_attach_diag.ctrl_stat = value;
            if (value & ARM_SWD_DP_CTRL_STICKY_MASK) status = LINK_TARGET_FAULT;
            else if ((value & ARM_SWD_DP_POWER_ACK) != ARM_SWD_DP_POWER_ACK) status = LINK_TARGET_WAIT;
        }
        break;
    case 8:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_CSW, ARM_SWD_MEM_AP_CSW_32);
        status = arm_swd_write_ap_once(ARM_SWD_AP_CSW, ARM_SWD_MEM_AP_CSW_32, 0);
        break;
    case 9:
        status = arm_swd_read_ap_once(ARM_SWD_AP_CSW, &value, 0);
        break;
    case 10:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF, ARM_SWD_AP_CSW);
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &value, 0);
        if (status == LINK_TARGET_OK) {
            ble_attach_diag.csw = value;
            if ((value & ARM_SWD_AP_CSW_VERIFY_MASK) != (ARM_SWD_MEM_AP_CSW_32 & ARM_SWD_AP_CSW_VERIFY_MASK)) status = LINK_TARGET_FAULT;
            else arm_swd_mem_ap_csw = value;
        }
        break;
    default:
        status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &value, 0);
        if (status == LINK_TARGET_OK) {
            ble_attach_diag.ctrl_stat = value;
            if (value & ARM_SWD_DP_CTRL_STICKY_MASK) status = LINK_TARGET_FAULT;
        }
        if (status == LINK_TARGET_OK) {
            arm_swd_dap_power_ready = arm_swd_dap_select_valid = arm_swd_dap_csw_valid = 1;
            arm_swd_dp_select = 0;
            *idcode = ble_attach_diag.idcode;
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, 0);
        }
        ble_attach_diag.status = status;
        if (status != LINK_TARGET_WAIT) ble_attach_step = 0;
        return status;
    }
    ble_attach_diag.status = status;
    if (status == LINK_TARGET_WAIT) return status;
    if (status != LINK_TARGET_OK) { ble_attach_step = 0; return status; }
    ble_attach_step++;
    return LINK_TARGET_WAIT;
}

/* BLE programmer memory transactions. These do not use the legacy synchronous
 * accessors: late RDBUFF WAIT must not restart an already accepted AP write. */
enum { BLE_MEM_READ32 = 1, BLE_MEM_WRITE32, BLE_MEM_WRITE16, BLE_MEM_READ_FLASH,
       BLE_MEM_WRITE_SRAM };
enum { BLE_MEM_SELECT, BLE_MEM_CSW, BLE_MEM_TAR, BLE_MEM_DRW,
       BLE_MEM_RDBUFF, BLE_MEM_STATUS };
#define BLE_MEM_FLASH_BASE_DEFAULT 0x08000000u
#define BLE_MEM_FLASH_SIZE_DEFAULT 0x00010000u
#define BLE_MEM_FLASH_SIZE_MAX     0x00200000u
#define BLE_MEM_TAR_MASK   0x3ffu

static uint32_t ble_mem_flash_base = BLE_MEM_FLASH_BASE_DEFAULT;
static uint32_t ble_mem_flash_end = BLE_MEM_FLASH_BASE_DEFAULT + BLE_MEM_FLASH_SIZE_DEFAULT;

void target_arm_swd_ble_set_flash_bounds(uint32_t base, uint32_t size)
{
    if (!size || size > BLE_MEM_FLASH_SIZE_MAX) {
        ble_mem_flash_base = BLE_MEM_FLASH_BASE_DEFAULT;
        ble_mem_flash_end = BLE_MEM_FLASH_BASE_DEFAULT + BLE_MEM_FLASH_SIZE_DEFAULT;
        return;
    }
    ble_mem_flash_base = base;
    ble_mem_flash_end = base + size;
}

static struct {
    uint8_t op, step;
    uint8_t posted;
    uint32_t addr, value, csw, result, deadline;
    uint8_t *block_data;
    const uint8_t *write_data;
    uint32_t next_word;
    uint16_t count, offset;
} ble_mem;

void target_arm_swd_ble_mem_reset(void)
{
    ble_mem.op = 0;
}

void target_arm_swd_ble_mem_cancel(void)
{
    if (ble_mem.op == BLE_MEM_READ_FLASH || ble_mem.op == BLE_MEM_WRITE_SRAM) {
        ble_mem.block_data = 0;
        ble_mem.write_data = 0;
        if (ble_mem.posted) {
            ble_mem.step = BLE_MEM_RDBUFF;
            return;
        }
    }
    /* A pending read or write already accepted by DRW drains via RDBUFF/STATUS. */
    if (ble_mem.step <= BLE_MEM_DRW) ble_mem.op = 0;
}

static void arm_swd_ble_store_flash_word(uint32_t word)
{
    uint8_t lane = (ble_mem.addr + ble_mem.offset) & 3u;

    if (!ble_mem.block_data) return;
    for (; lane < 4u && ble_mem.offset < ble_mem.count; ++lane) {
        ble_mem.block_data[ble_mem.offset++] = (uint8_t)(word >> (8u * lane));
    }
}

static link_target_status_t arm_swd_ble_mem_advance(uint32_t *read_data)
{
    uint32_t value = 0;
    link_target_status_t status;
    const uint8_t block = ble_mem.op == BLE_MEM_READ_FLASH || ble_mem.op == BLE_MEM_WRITE_SRAM;
    if ((int32_t)(link_ble_now_ms() - ble_mem.deadline) >= 0) {
        if (block) arm_swd_dap_invalidate_context();
        ble_mem.op = 0;
        return LINK_SWD_TIMEOUT;
    }
    switch (ble_mem.step) {
    case BLE_MEM_SELECT:
        if (arm_swd_dap_select_valid && arm_swd_dp_select == 0) {
            ble_mem.step++;
            return LINK_SWD_PENDING;
        }
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_SELECT, 0);
        status = arm_swd_write_dp_once(ARM_SWD_DP_SELECT, 0, 0);
        if (status == LINK_TARGET_OK) {
            arm_swd_dp_select = 0;
            arm_swd_dap_select_valid = 1;
            arm_swd_dap_csw_valid = 0;
        }
        break;
    case BLE_MEM_CSW:
        if (arm_swd_dap_csw_valid && arm_swd_mem_ap_csw == ble_mem.csw) {
            ble_mem.step++;
            return LINK_SWD_PENDING;
        }
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_CSW, ble_mem.csw);
        status = arm_swd_write_ap_once(ARM_SWD_AP_CSW, ble_mem.csw, 0);
        if (status == LINK_TARGET_OK) {
            arm_swd_mem_ap_csw = ble_mem.csw;
            arm_swd_dap_csw_valid = 1;
        }
        break;
    case BLE_MEM_TAR: {
        uint32_t addr = block ? ble_mem.next_word : ble_mem.addr;
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_TAR, addr);
        status = arm_swd_write_ap_once(ARM_SWD_AP_TAR, addr, 0);
        if (status == LINK_TARGET_OK) arm_swd_mem_ap_tar = addr;
        break;
    }
    case BLE_MEM_DRW:
        if (ble_mem.op == BLE_MEM_WRITE_SRAM) {
            unsigned off = ble_mem.next_word - ble_mem.addr;
            unsigned lane;
            value = 0xffffffffu;
            for (lane = 0; lane < 4u && off + lane < ble_mem.count; ++lane) {
                unsigned shift = 8u * lane;
                value = (value & ~(0xffu << shift)) |
                        ((uint32_t)ble_mem.write_data[off + lane] << shift);
            }
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_DRW, ble_mem.next_word);
            status = arm_swd_write_ap_once(ARM_SWD_AP_DRW, value, 0);
            if (status != LINK_TARGET_OK) break;
            /* Advance only when accepted. RDBUFF drains before CTRL/STAT
             * so checking sticky flags cannot discard pending AP writes. */
            ble_mem.posted = 1;
            ble_mem.next_word += 4u;
            if (ble_mem.next_word >= ble_mem.addr + ble_mem.count)
                ble_mem.step = BLE_MEM_RDBUFF;
            return LINK_SWD_PENDING;
        }
        if (ble_mem.op == BLE_MEM_READ_FLASH) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_DRW, ble_mem.next_word);
            status = arm_swd_read_ap_once(ARM_SWD_AP_DRW, &value, 0);
            if (status != LINK_TARGET_OK) break;

            /* The first AP read primes the pipeline. Each later AP read
             * supplies the word requested by the preceding transaction. */
            if (ble_mem.posted) arm_swd_ble_store_flash_word(value);
            ble_mem.posted = 1;
            ble_mem.next_word += 4u;
            if ((ble_mem.next_word & BLE_MEM_TAR_MASK) == 0u ||
                ble_mem.next_word >= ble_mem.addr + ble_mem.count) {
                ble_mem.step = BLE_MEM_RDBUFF;
            }
            return LINK_SWD_PENDING;
        }
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_DRW, ble_mem.addr);
        if (ble_mem.op == BLE_MEM_READ32) {
            status = arm_swd_read_ap_once(ARM_SWD_AP_DRW, &value, 0);
        } else {
            value = ble_mem.value;
            if (ble_mem.op == BLE_MEM_WRITE16 && (ble_mem.addr & 2u)) value <<= 16;
            status = arm_swd_write_ap_once(ARM_SWD_AP_DRW, value, 0);
        }
        break;
    case BLE_MEM_RDBUFF:
        arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_RDBUFF,
                             block ? ble_mem.next_word - 4u : ble_mem.addr);
        status = arm_swd_read_dp_once(ARM_SWD_DP_RDBUFF, &value, 0);
        if (status == LINK_TARGET_OK) {
            if (block) {
                if (ble_mem.op == BLE_MEM_READ_FLASH) arm_swd_ble_store_flash_word(value);
                ble_mem.posted = 0;
            } else {
                ble_mem.result = value;
            }
        }
        break;
    default:
        status = arm_swd_read_dp_once(ARM_SWD_DP_CTRL_STAT, &value, 0);
        if (status == LINK_TARGET_OK && (value & ARM_SWD_DP_CTRL_STICKY_MASK)) {
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_DRW, value);
            status = LINK_TARGET_FAULT;
        }
        if (status == LINK_TARGET_OK) {
            if (ble_mem.op == BLE_MEM_READ_FLASH &&
                ble_mem.block_data && ble_mem.offset < ble_mem.count) {
                /* TAR's carry across a 1KiB boundary is implementation defined. */
                ble_mem.step = BLE_MEM_TAR;
                return LINK_SWD_PENDING;
            }
            if (read_data) *read_data = ble_mem.result;
            arm_swd_set_mem_diag(ARM_SWD_MEM_STAGE_NONE, ble_mem.addr);
            ble_mem.op = 0;
            return LINK_TARGET_OK;
        }
        break;
    }
    if (status == LINK_TARGET_WAIT) return LINK_SWD_PENDING;
    if (status != LINK_TARGET_OK) {
        ble_mem.op = 0;
        arm_swd_dap_invalidate_context();
        return status;
    }
    ble_mem.step++;
    return LINK_SWD_PENDING;
}

link_target_status_t target_arm_swd_ble_mem_cancel_poll(void)
{
    if (!ble_mem.op) return LINK_TARGET_OK;
    /* cancel() has already removed all operations not accepted at DRW. */
    return arm_swd_ble_mem_advance(0);
}

static void arm_swd_ble_mem_start(uint8_t op, uint32_t addr, uint32_t value)
{
    ble_mem.op = op;
    ble_mem.addr = addr;
    ble_mem.value = value;
    ble_mem.csw = op == BLE_MEM_WRITE16 ? ARM_SWD_MEM_AP_CSW_16 : ARM_SWD_MEM_AP_CSW_32;
    ble_mem.result = 0;
    ble_mem.step = BLE_MEM_SELECT;
    ble_mem.deadline = link_ble_now_ms() + 500u;
}

static uint8_t arm_swd_ble_mem_ready(void)
{
    return arm_swd_dp_ready && arm_swd_dap_power_ready && arm_swd_dap_write_phase_valid;
}

static link_target_status_t arm_swd_ble_mem_start_step(uint8_t op, uint32_t addr,
                                                       uint32_t value, uint32_t *result)
{
    if ((op == BLE_MEM_READ32 && !result) ||
        (op == BLE_MEM_WRITE16 ? (addr & 1u) : (addr & 3u))) return LINK_TARGET_BAD_PARAM;
    if (!ble_mem.op) {
        if (!arm_swd_ble_mem_ready()) return LINK_TARGET_NOT_CONNECTED;
        arm_swd_ble_mem_start(op, addr, value);
    } else if (ble_mem.op != op || ble_mem.addr != addr || ble_mem.value != value) {
        return LINK_TARGET_BAD_PARAM;
    }
    return arm_swd_ble_mem_advance(result);
}

link_target_status_t target_arm_swd_ble_read_flash(uint32_t addr, uint8_t *data, uint16_t count)
{
    if (!data || !count || count > 256u ||
        addr < ble_mem_flash_base || addr >= ble_mem_flash_end ||
        count > ble_mem_flash_end - addr) return LINK_TARGET_BAD_PARAM;

    if (!ble_mem.op) {
        if (!arm_swd_ble_mem_ready()) return LINK_TARGET_NOT_CONNECTED;
        arm_swd_ble_mem_start(BLE_MEM_READ_FLASH, addr, 0);
        ble_mem.block_data = data;
        ble_mem.next_word = addr & ~3u;
        ble_mem.count = count;
        ble_mem.offset = 0;
        ble_mem.posted = 0;
    } else if (ble_mem.op != BLE_MEM_READ_FLASH || ble_mem.addr != addr ||
               ble_mem.block_data != data || ble_mem.count != count) {
        return LINK_TARGET_BAD_PARAM;
    }
    return arm_swd_ble_mem_advance(0);
}

link_target_status_t target_arm_swd_ble_write_sram(uint32_t addr, const uint8_t *data, uint16_t count)
{
    const uint32_t end = addr < TARGET_LOADER_DATA ? TARGET_LOADER_DATA : TARGET_LOADER_MAILBOX;
    if (!data || !count || (addr & 3u) || addr < TARGET_LOADER_BASE || addr >= end ||
        count > end - addr || count > BLE_MEM_TAR_MASK + 1u - (addr & BLE_MEM_TAR_MASK))
        return LINK_TARGET_BAD_PARAM;

    if (!ble_mem.op) {
        if (!arm_swd_ble_mem_ready()) return LINK_TARGET_NOT_CONNECTED;
        arm_swd_ble_mem_start(BLE_MEM_WRITE_SRAM, addr, 0);
        ble_mem.write_data = data;
        ble_mem.next_word = addr;
        ble_mem.count = count;
        ble_mem.posted = 0;
    } else if (ble_mem.op != BLE_MEM_WRITE_SRAM || ble_mem.addr != addr ||
               ble_mem.write_data != data || ble_mem.count != count) {
        return LINK_TARGET_BAD_PARAM;
    }
    return arm_swd_ble_mem_advance(0);
}

link_target_status_t target_arm_swd_ble_read32(uint32_t addr, uint32_t *value)
{
    return arm_swd_ble_mem_start_step(BLE_MEM_READ32, addr, 0, value);
}
link_target_status_t target_arm_swd_ble_write32(uint32_t addr, uint32_t value)
{
    return arm_swd_ble_mem_start_step(BLE_MEM_WRITE32, addr, value, 0);
}
link_target_status_t target_arm_swd_ble_write16(uint32_t addr, uint16_t value)
{
    return arm_swd_ble_mem_start_step(BLE_MEM_WRITE16, addr, value, 0);
}
#endif
