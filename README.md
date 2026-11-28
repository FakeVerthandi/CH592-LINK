# CH592-LINK

基于 WCH CH592F（RISC-V 60MHz，26KiB RAM）实现的 USB / BLE 双模 SWD 调试烧录器。USB 侧实现标准 CMSIS-DAP v2（WinUSB Bulk），免驱接入 Keil MDK 与 OpenOCD；蓝牙侧配合本地网页（Web Bluetooth）写入 Intel HEX 或 BIN，并在目标端对受影响的 Flash 区域执行校验。

`v0.5.0` 将 SWD 底层切换为 **CH592 硬件 SPI0 原生 2 线半双工 + GPIO 混合驱动**（最高支持 10MHz，无需外接限流电阻），并在 `src/link_config.h` 中保留 `CONFIG_SWD_HW_SPI` 编译宏以随时回退至软 GPIO 实现。

当前已在 **STM32F103C8T6**、**STM32F407VGT6（1MiB）** 与 **STM32F407VET6（512KiB）** 上完成 OpenOCD、Keil 与 BLE 烧录验证；另内置 STM32F3 单 Bank 支持（待实板验证），GD32F303VET6 已验证 SWD 识别。

## 接线与使用

### 硬件连接

默认模式（`CONFIG_SWD_HW_SPI = 1`，`CONFIG_SWD_HW_SPI_2WIRE = 1`）：

| CH592 引脚 | 目标 STM32 引脚 | 说明 |
| :--- | :--- | :--- |
| `PA13`（SCK0） | `SWCLK` | 硬件 SPI0 时钟输出 |
| `PA15`（MISO / SDX） | `SWDIO` | 硬件 SPI0 2 线半双工数据，无需外接电阻 |
| `GND` | `GND` | 共地连接 |

可选编译模式（修改 `src/link_config.h` 后重新编译）：
- **3 线硬件 SPI0 模式**：设 `CONFIG_SWD_HW_SPI_2WIRE = 0`，将 `PA14` 串联 `330Ω ~ 470Ω` 限流电阻后与 `PA15` 并接至目标 `SWDIO`。
- **软 GPIO 兼容模式**：设 `CONFIG_SWD_HW_SPI = 0`，引脚恢复为 `PA5 → SWCLK`、`PA4 → SWDIO`。

供电与布线注意事项（基于 CH592 官方评估板 EVT 实测）：
- 建议使用 **5cm 左右短杜邦线**，避免 `SWCLK` 与 `SWDIO` 长距离并行走线引入高频串扰。
- 请勿使用 CH592 的 `3.3V` 引脚反向为 STM32F4 等高功耗目标板供电。有线调试建议两板独立 USB 供电；纯无线 BLE 场景可由目标板 `3.3V` 为 CH592 供电，或为 CH592 独立供电。
- 在纯无线供电且使用官方 EVT 板时，为降低蓝牙射频发射对 SWD 信号的干扰，建议在网页端将 SWCLK 选择为 `2 MHz`（吞吐瓶颈位于 BLE 空口，2MHz 与 5MHz 总耗时差异小于 0.5 秒）。

### 使用步骤

1. 使用 WCHISP 工具将 `firmware/CH592-LINK-0.5.0.hex` 下载至 CH592。
2. **OpenOCD**：插入 USB 后免配置自动识别：
   ```bash
   # STM32F4（最高 10MHz）
   openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f4x.cfg \
     -c "adapter speed 10000; program firmware.hex verify reset exit"

   # STM32F1（复位默认 8MHz HSI，建议 4~5MHz）
   openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f1x.cfg \
     -c "adapter speed 5000; program firmware.hex verify reset exit"
   ```
3. **Keil MDK**：先断开网页蓝牙连接（USB 与 BLE 会话互斥），在目标工程 `Debug` 设置中选择 `CMSIS-DAP Debugger` 并将端口设为 `SW`。
4. **BLE 无线烧录**：运行 `web/start.cmd`，在 Windows Chrome 或 Edge 中打开本地 `http://localhost:...` 地址。连接 `CH592-LINK` 并识别目标后选择 `.hex` 或 `.bin` 文件。默认发送模式为 `20B`；若系统支持 `ATT MTU ≥ 142`，可切换为 `139B 有应答写 · 满块两次确认`。

网页默认由目标端逐 1KiB 块计算 CRC32 校验，也可手动勾选完整逐字节读回。擦写前会执行向量表检查、F1/F4 目标匹配检查及 `DBGMCU_ID` 复核；传输或擦写结果未知时不自动重试，可通过“读取 SWD 诊断”查看失败阶段、ACK 状态与访存地址。

## 目录结构

| 目录 | 说明 |
| :--- | :--- |
| `firmware/` | 预编译 CH592 固件（`CH592-LINK-0.5.0.hex`） |
| `src/` | CH592 固件源码（硬件 SPI0 / 软 GPIO SWD 驱动、CMSIS-DAP v2、BLE 烧录状态机、目标芯片表） |
| `target_loader/` | 运行于目标 STM32 SRAM 内的 Thumb 擦写与 CRC32 校验程序（F1/F3 与 F4） |
| `web/` | Web Bluetooth 烧录网页端及本地启动脚本（`start.cmd` / `serve.py`） |
| `tools/` | 固件构建脚本 `build_ble.py` 与目标端 loader 构建脚本 `build_target_loaders.py` |
| `test_firmware/` | STM32F103 / STM32F407 预编译测试固件（`.bin` / `.hex`） |
| `Ld/` | CH592 链接脚本（Flash 与 `.highcode` RAM 分区定义） |
| `ble_sdk/` | WCH CH59x 官方 BLE 协议栈、外设驱动与启动文件 |
| `third_party/` | CherryUSB 1.5.3 USBFS 协议栈及 CH58x/CH59x 端口适配 |

## 从源码构建

在 Windows 上安装 Python 3，准备 WCH RISC-V GCC（已验证 8.2.0）以及 Cortex-M Thumb 工具链（支持 `arm-none-eabi-gcc` 或 Clang + LLD）。将环境变量 `CH592_RISCV_BIN` 与 `CH592_THUMB_TOOLS` 分别指向对应工具的 `bin` 目录；若工具位于 `PATH` 中，或 `arm-none-eabi-gcc` 与 RISC-V GCC 位于同级工具链目录，可省略 `CH592_THUMB_TOOLS`。

在仓库根目录执行 `python tools/build_ble.py`，输出的 `.hex`、`.elf`、`.map` 与构建报告位于 `obj/ble_build/`，构建脚本会自动校验编译后保留栈空间不少于 4KiB。

## 实测数据（v0.5.0）

以下数据均在硬件 SPI0 2 线模式（5cm 短杜邦线直连、无外接电阻、零错包）条件下测得。

### OpenOCD

| 目标芯片 | 时钟 | 测试项 | 耗时 | 有效速度 |
| :--- | :---: | :--- | :---: | :---: |
| STM32F407VGT6 | 10MHz | 128 KiB 连续读（`dump_image`） | 0.99s | **128.98 KiB/s** |
| STM32F407VET6 | 10MHz | 128 KiB 连续读（`dump_image`） | 0.99s | **128.80 KiB/s** |
| STM32F407VET6 | 10MHz | 140 KiB 纯写入（预擦除） | 1.26s | **111.67 KiB/s** |
| STM32F407VGT6 | 10MHz | 140 KiB 纯写入（预擦除） | 1.27s | **110.25 KiB/s** |
| STM32F407VGT6 | 10MHz | 140 KiB 目标端校验（`verify_image`） | 0.34s | **412.65 KiB/s** |
| STM32F407VGT6 | 10MHz | 40.18 KiB 全流程（擦+写+校+复位，×3） | 2.35s ~ 2.39s | **16.8 ~ 17.1 KiB/s** |
| STM32F103C8T6 | 5MHz | 64 KiB 连续读（`dump_image`） | 0.57s | **112.33 KiB/s** |
| STM32F103C8T6 | 5MHz | 32 KiB 纯写入（预擦除） | 1.02s | **31.35 KiB/s** |
| STM32F103C8T6 | 5MHz | 40.17 KiB 全流程（三轮中位数） | 3.30s | **12.19 KiB/s** |

### Keil MDK（`UV4 -f` 命令行三轮中位数）

| 目标芯片 | 镜像大小 | 耗时 | 有效速度 |
| :--- | :---: | :---: | :---: |
| STM32F407VGT6 | 40.18 KiB | 3.67s | **10.94 KiB/s** |
| STM32F407VET6 | 40.18 KiB | 3.78s | **10.64 KiB/s** |
| STM32F103C8T6 | 40.17 KiB | 5.45s | **7.36 KiB/s** |

### BLE 无线烧录（139B 有应答写 + 目标端 1KiB CRC32）

| 目标芯片 | 供电与时钟 | 镜像大小 | 擦除 | 写入 | 校验 | 总耗时 |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| STM32F407VGT6 | 双 USB，5MHz | 40.18 KiB | 1.17s | 39.54s（1042 B/s） | 2.26s | **42.97s** |
| STM32F407VET6 | 纯无线（F407 3.3V 供 CH592），2MHz | 40.18 KiB | 1.28s | 39.82s（1035 B/s） | 2.28s | **43.38s** |
| STM32F103C8T6 | 2MHz（擦 41 页） | 40.17 KiB | 3.89s | 39.85s（1034 B/s） | 2.28s | **46.02s** |

注：
1. Keil 项为命令行 `UV4 -f` 自动化实测，包含进程启停开销；在 Keil 界面内直接执行 Download 耗时更短。
2. 受 USB 2.0 Full-Speed（12 Mbps，64B Bulk）接口带宽限制，本项目有线读取速率已接近 USB FS 协议实际吞吐上限；STM32F103 纯写入速率受限于目标 Flash 半字编程硬件耗时。

## 变更记录（v0.4.4 → v0.5.0）

- 重构 SWD 底层为 CH592 硬件 SPI0 2 线半双工 + GPIO 混合驱动（`PA13/PA15`，最高 10MHz），保留 `CONFIG_SWD_HW_SPI` 编译回退开关。
- 放宽软 GPIO 路径 5MHz 块读写时序余量，修复 `trn=0` 下遇到 `ACK_WAIT` 遗漏换向时钟的问题，块写尾补充 8 个空闲低时钟后再排空 `RDBUFF`。
- 网页端升级至 `WEB 0.5.0`，BLE SWD 诊断补充具体失败访存地址（`访存=0x...`），并更新接线提示与无线时钟建议。
- 在 `test_firmware/` 中补充 F103C8 / F407VG 预编译测试固件。

第三方组件及来源见 [THIRD_PARTY.md](THIRD_PARTY.md)。
