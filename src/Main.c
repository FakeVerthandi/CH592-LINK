#include "CH59x_common.h"

#include "link_board.h"
#include "link_protocol.h"
#include "link_target.h"
#include "link_usb.h"
#if LINK_BLE_ENABLE
#include "HAL.h"
#include "link_ble.h"
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];
#endif

static void link_main_loop(void)
{
    while (1) {
#if LINK_BLE_ENABLE
        TMOS_SystemProcess();
        link_ble_poll();
#endif
        link_usb_poll();
        link_protocol_poll();
        link_target_poll();
    }
}

int main(void)
{
#if (defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif

    SetSysClock(CLK_SOURCE_PLL_60MHz);

#ifdef DEBUG
    GPIOA_SetBits(bTXD1);
    GPIOA_ModeCfg(bTXD1, GPIO_ModeOut_PP_5mA);
    UART1_DefInit();
    PRINT("CH592 LINK V1 boot\r\n");
#endif

    link_board_init();
    link_protocol_init();
    link_target_init();
#if LINK_BLE_ENABLE
    CH59x_BLEInit();
    HAL_Init();
    GAPRole_PeripheralInit();
    link_ble_init();
#endif
    link_usb_init();

    link_main_loop();
}
