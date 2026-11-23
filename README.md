# CH592-LINK

基于 WCH CH592 的双模调试烧录器。USB 侧实现标准 CMSIS-DAP v2（WinUSB Bulk），可直接接入 Keil 与 OpenOCD；蓝牙侧配合本地网页写入 Intel HEX 或 BIN，并校验受影响的 Flash 区域。

当前已在 STM32F103C8T6 与 STM32F407VGT6 上完成 USB（Keil / OpenOCD）与 BLE 烧录验证（另含 STM32F3 单 Bank 支持，待实板验证；GD32F303VET6 已验证 SWD 识别）。

## 接线和使用

CH592 PA5 接目标 SWCLK，PA4 接 SWDIO，两板共地。目标板按其板级要求供电。

1. 将 `firmware/CH592-LINK-0.4.4.hex` 下载到 CH592 探针。
2. 使用蓝牙时，运行 `web/start.cmd`，在 Windows Chrome 或 Edge 中打开启动器给出的 localhost 地址。连接探针后选择目标程序。默认发送模式为 20B；使用 139B 有应答写需要 ATT MTU 至少为 142。
3. 使用 Keil 时，先释放网页蓝牙连接，再在目标工程的调试设置中选择 CMSIS-DAP。
4. 使用 OpenOCD 时，可免配置自动发现设备，例如：
   ```bash
   openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f4x.cfg -c "program firmware.hex verify reset exit"
   ```

网页默认由目标端逐 1KiB 块计算 CRC32，也可手动勾选完整读回。擦写前会检查目标芯片与固件匹配性及目标端 `DBGMCU_ID`；擦写结果未知时不自动重试，需重新建立会话。

## 从源码构建

在 Windows 上安装 Python 3，准备 WCH RISC-V GCC（已验证 8.2.0）以及 Cortex-M Thumb 工具链（支持 `arm-none-eabi-gcc` 或 Clang + LLD）。将环境变量 `CH592_RISCV_BIN` 和 `CH592_THUMB_TOOLS` 分别指向对应工具的 `bin` 目录；若工具已在 PATH 中，或 `arm-none-eabi-gcc` 与 RISC-V GCC 位于同级工具链目录，可省略 `CH592_THUMB_TOOLS`。

从工程根目录运行 `python tools/build_ble.py`，生成的 HEX、ELF、MAP 和构建报告位于 `obj/ble_build/`。

## 实测（0.4.4）

| 方式 | 目标芯片 | 测试项 / 镜像 | 实测耗时 | 有效速度 |
| :--- | :--- | :--- | :---: | :---: |
| **OpenOCD** | STM32F407VGT6 | 140 KiB 纯写入（`reset init` 8MHz） | 1.55s | **90.62 KiB/s** |
| **OpenOCD** | STM32F407VGT6 | 128 KiB Flash 连续读（`dump_image`） | 1.21s | **106.08 KiB/s** |
| **OpenOCD** | STM32F407VGT6 | 140 KiB 目标端校验（`verify_image`） | 0.37s | **376.98 KiB/s** |
| **OpenOCD** | STM32F407VGT6 | 40.18 KiB 全流程（擦+写+校+复位） | 2.14s | **18.79 KiB/s** |
| **OpenOCD** | STM32F103C8T6 | 64 KiB Flash 连续读（`dump_image`） | 0.60s | **105.94 KiB/s** |
| **OpenOCD** | STM32F103C8T6 | 40.17 KiB 纯写入（5MHz） | 1.28s | **31.36 KiB/s** |
| **OpenOCD** | STM32F103C8T6 | 40.17 KiB 全流程（三轮中位数） | 3.48s | **11.54 KiB/s** |
| **Keil** | STM32F407VGT6 | 40.18 KiB 全流程（`UV4 -f` 命令行） | 3.86s ~ 4.18s | **9.6 ~ 10.4 KiB/s** |
| **Keil** | STM32F103C8T6 | 40.17 KiB 全流程（`UV4 -f` 三轮中位数） | 5.92s | **6.78 KiB/s** |
| **BLE** | STM32F407VGT6 | 40.18 KiB 全流程（139B 有应答写 + 块 CRC32） | 43.78s | **940 B/s**（写 1023 B/s） |
| **BLE** | STM32F103C8T6 | 40.17 KiB 全流程（139B 有应答写 + 块 CRC32） | 47.14s | **873 B/s**（写 1025 B/s） |

注：Keil 项为命令行 `UV4 -f` 自动化实测，包含进程启停开销；在 Keil 界面内直接点击 Download 实际耗时更短。

第三方组件及来源见 [THIRD_PARTY.md](THIRD_PARTY.md)。
