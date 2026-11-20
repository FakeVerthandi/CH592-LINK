#include "CH59x_common.h"

#include "link_target.h"
#include "target_arm_swd.h"

static link_target_type_t current_target = LINK_TARGET_NONE;

void link_target_init(void)
{
    current_target = LINK_TARGET_NONE;
    target_arm_swd_init();
}

void link_target_poll(void)
{
    (void)current_target;
}

link_target_status_t link_target_connect(void)
{
    link_target_status_t status = target_arm_swd_connect();

    if (status == LINK_TARGET_OK) {
        current_target = LINK_TARGET_ARM_SWD;
    }

    return status;
}

link_target_status_t link_target_reset(void)
{
    if (current_target == LINK_TARGET_NONE) {
        return LINK_TARGET_NOT_CONNECTED;
    }

    if (current_target == LINK_TARGET_ARM_SWD) {
        return target_arm_swd_reset();
    }

    return LINK_TARGET_NOT_IMPLEMENTED;
}

link_target_status_t link_target_read_id(uint32_t *idcode)
{
    if (current_target == LINK_TARGET_NONE) {
        return LINK_TARGET_NOT_CONNECTED;
    }

    if (current_target == LINK_TARGET_ARM_SWD) {
        return target_arm_swd_read_idcode(idcode);
    }

    return LINK_TARGET_NOT_IMPLEMENTED;
}

link_target_status_t link_target_read32(uint32_t addr, uint32_t *value)
{
    if (current_target == LINK_TARGET_NONE) {
        return LINK_TARGET_NOT_CONNECTED;
    }

    if (current_target == LINK_TARGET_ARM_SWD) {
        return target_arm_swd_read32(addr, value);
    }

    return LINK_TARGET_NOT_IMPLEMENTED;
}

link_target_status_t link_target_read32_block(uint32_t addr, uint32_t *values, uint8_t word_count)
{
    if (current_target == LINK_TARGET_NONE) {
        return LINK_TARGET_NOT_CONNECTED;
    }

    if (current_target == LINK_TARGET_ARM_SWD) {
        return target_arm_swd_read32_block(addr, values, word_count);
    }

    return LINK_TARGET_NOT_IMPLEMENTED;
}

link_target_status_t link_target_write32(uint32_t addr, uint32_t value)
{
    if (current_target == LINK_TARGET_NONE) {
        return LINK_TARGET_NOT_CONNECTED;
    }

    if (current_target == LINK_TARGET_ARM_SWD) {
        return target_arm_swd_write32(addr, value);
    }

    return LINK_TARGET_NOT_IMPLEMENTED;
}

link_target_status_t link_target_flash_erase_page(uint32_t addr)
{
    if (current_target == LINK_TARGET_NONE) {
        return LINK_TARGET_NOT_CONNECTED;
    }

    if (current_target == LINK_TARGET_ARM_SWD) {
        return target_arm_swd_flash_erase_page(addr);
    }

    return LINK_TARGET_NOT_IMPLEMENTED;
}

link_target_status_t link_target_flash_write(uint32_t addr, const uint8_t *data, uint8_t len)
{
    if (current_target == LINK_TARGET_NONE) {
        return LINK_TARGET_NOT_CONNECTED;
    }

    if (current_target == LINK_TARGET_ARM_SWD) {
        return target_arm_swd_flash_write(addr, data, len);
    }

    return LINK_TARGET_NOT_IMPLEMENTED;
}

void link_target_get_diag(link_target_diag_t *diag)
{
    if (diag == 0) {
        return;
    }

    diag->raw_ack = 0;
    diag->attempts = 0;
    diag->dio_read_high = 0;
    diag->dio_read_low = 0;
    diag->dio_read_release = 0;
    diag->strategy = 0;
    diag->turnaround = 0;
    diag->ack_window = 0;

    if (current_target == LINK_TARGET_ARM_SWD) {
        target_arm_swd_get_diag(diag);
    }
}
