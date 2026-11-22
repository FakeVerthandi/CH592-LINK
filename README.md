# CH592-LINK

基于 WCH CH592 的双模调试烧录器。USB 侧实现 CMSIS-DAP v2（WinUSB），可直接接入 Keil；蓝牙侧配合本地网页写入 Intel HEX 或 BIN，并校验受影响的 Flash 区域。

当前已在 STM32F103C8T6 与 STM32F407VGT6 上完成 USB 与 BLE 烧录验证（另含 STM32F3 单 Bank 支持，待实板验证；GD32F303VET6 已验证 SWD 识别）。

## 接线和使用

CH592 PA5 接目标 SWCLK，PA4 接 SWDIO，两板共地。目标板按其板级要求供电。

1. 将 `firmware/CH592-LINK-0.4.2.hex` 下载到 CH592 探针。
2. 使用蓝牙时，运行 `web/start.cmd`，在 Windows Chrome 或 Edge 中打开启动器给出的 localhost 地址。连接探针后选择目标程序。默认发送模式为 20B；使用 139B 有应答写需要 ATT MTU 至少为 142。
3. 使用 Keil 时，先释放网页蓝牙连接，再在目标工程的调试设置中选择 CMSIS-DAP。

网页默认由目标端逐 1KiB 块计算 CRC32，也可手动勾选完整读回。擦写前会检查目标芯片与固件匹配性及目标端 `DBGMCU_ID`；擦写结果未知时不自动重试，需重新建立会话。

## 从源码构建

在 Windows 上安装 Python 3，准备 WCH RISC-V GCC（已验证 8.2.0）以及 Cortex-M Thumb 工具链（支持 `arm-none-eabi-gcc` 或 Clang + LLD）。将环境变量 `CH592_RISCV_BIN` 和 `CH592_THUMB_TOOLS` 分别指向对应工具的 `bin` 目录；若工具已在 PATH 中，或 `arm-none-eabi-gcc` 与 RISC-V GCC 位于同级工具链目录，可省略 `CH592_THUMB_TOOLS`。

从工程根目录运行 `python tools/build_ble.py`，生成的 HEX、ELF、MAP 和构建报告位于 `obj/ble_build/`。

## 实测（0.4.2）

- STM32F103C8T6（41,132B 镜像）：Keil CLI（`UV4 -f`，含进程启停）三轮中位数 6.14s（6.54 KiB/s）；BLE 139B 有应答写全流程 49.20s（836 B/s），41 块 CRC32 校验通过。
- STM32F407VGT6（41,148B 镜像）：Keil CLI 三轮中位数 3.93s（10.23 KiB/s）；BLE 139B 有应答写全流程 43.78s（940 B/s），3 扇区擦除与 41 块 CRC32 校验通过。

第三方组件及来源见 [THIRD_PARTY.md](THIRD_PARTY.md)。
