# CH592-LINK

基于 WCH CH592F（RISC-V 60MHz，26KiB RAM）做的双模 SWD 调试烧录器：

- **有线 USB**：标准 CMSIS-DAP v2（WinUSB Bulk），直接进 Keil 和 OpenOCD，不用装第三方驱动。
- **无线 BLE**：配合本地网页（Web Bluetooth）烧录 `.hex` / `.bin`，在目标 STM32 的 SRAM 内运行擦写与 1KiB 块 CRC32 校验。

`v0.5.0` 把 SWD 底层改成了 **CH592 硬件 SPI0 原生 2 线半双工 + 极简 GPIO 混合驱动**（不焊电阻、不用面包板，三根短线直连），同时在代码里保留了编译开关，随时可以切回原软件 GPIO 方案做对照。

目前已上板实测通过的芯片：

- **STM32F103C8T6**：OpenOCD / Keil / BLE 全通
- **STM32F407VGT6（1MiB）**：OpenOCD（最高 10MHz）/ Keil / BLE 全通
- **STM32F407VET6（512KiB）**：OpenOCD（最高 10MHz）/ Keil / 纯无线 BLE（F407 3.3V 给 CH592 供电）全通
- 另内置 **STM32F3 单 Bank** 支持（待实板验证）；**GD32F303VET6** 已验证 SWD 识别

---

## 接线方式

### 默认接线（v0.5.0 硬件 SPI0 2 线模式）

只接三根线，建议用 **5cm 左右短杜邦线**：

| CH592 引脚 | 目标 STM32 引脚 | 说明 |
| :--- | :--- | :--- |
| `PA13`（SCK0） | `SWCLK` | 硬件 SPI0 时钟输出 |
| `PA15`（MISO / SDX） | `SWDIO` | 硬件 SPI0 2 线双向数据，无需外接电阻 |
| `GND` | `GND` | 必须可靠共地 |

可选模式（在 `src/link_config.h` 中切换编译宏）：

- **3 线模式（加限流电阻更稳妥）**：设 `CONFIG_SWD_HW_SPI_2WIRE` 为 `0`，`PA14` 串一颗 `330Ω~470Ω` 电阻后与 `PA15` 并接到目标 `SWDIO`。
- **回退软 GPIO 模式**：设 `CONFIG_SWD_HW_SPI` 为 `0`，引脚恢复为旧版 `PA5 → SWCLK`、`PA4 → SWDIO`。

### 供电与抗干扰说明（实测踩坑记录）

手头测试使用的是沁恒 CH592 官方评估板（EVT），实际联调时有几点硬件细节需要留意：

1. **线长与走线**：F407 的 SWCLK 引脚响应很快，15cm 以上的并排杜邦线在高频下容易由 `SWDIO` 串扰出多余时钟沿。换成 **5cm 短线**，或把 `SWCLK` 和 `SWDIO` 分开走，稳定性提升非常明显。
2. **不要用 CH592 的 3.3V 给 F407 反向供电**：F407 擦写 Flash 时电流较大，靠 CH592 供电会掉压断连。有线调试建议两块板各自 USB 供电；纯无线 BLE 场景可以反过来由目标板的 `3.3V` 给 CH592 供电（CH592 耗电很小），或者给 CH592 单独接充电宝。
3. **官方 EVT 板共地底噪**：CH592 EVT 板面向全功能评估，地线与退耦不如精简小板干净。部分抗干扰较弱的目标板在纯无线模式下 5MHz 偶尔受蓝牙射频发射干扰，**无线烧录建议在网页里把 SWCLK 选在 `2 MHz`**（瓶颈在蓝牙空口，2MHz 和 5MHz 总耗时只差不到 0.5 秒，稳定得多）。

---

## 快速上手

1. **烧录探针固件**：用 WCHISP 工具将 `firmware/CH592-LINK-0.5.0.hex` 下载到 CH592。
2. **OpenOCD 使用**：插入电脑 USB，免配置直接识别：
   ```bash
   # STM32F4 示例（10MHz 满速）
   openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f4x.cfg \
     -c "adapter speed 10000; program firmware.hex verify reset exit"

   # STM32F1 示例（复位后默认 8MHz HSI，建议 4~5MHz）
   openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f1x.cfg \
     -c "adapter speed 5000; program firmware.hex verify reset exit"
   ```
3. **Keil MDK 使用**：先断开网页蓝牙（USB 和 BLE 互斥），在工程 `Debug` 中选择 `CMSIS-DAP Debugger`，`Settings` 里选 `SW` 即可。
4. **BLE 无线烧录**：
   - 运行 `web/start.cmd`，用 Windows Chrome 或 Edge 打开显示的 `http://localhost:...` 地址。
   - 连接 `CH592-LINK`，识别目标芯片后拖入 `.hex` 或 `.bin` 文件（自带向量表检查和 F1/F4 防烧错拦截）。
   - 发送模式默认 `20B 保守发送`；若系统支持 ATT MTU ≥ 142，可切到 `139B 有应答写 · 满块两次确认` 提速。
   - 校验默认走目标端 1KiB 块 CRC32，也可勾选完整逐字节读回；擦写异常时不自动重试，可点“读取 SWD 诊断”查看失败阶段、ACK 及具体访存地址。

---

## 实测速度（v0.5.0 硬件 SPI0 2 线）

全部数据在 5cm 杜邦线、无电阻直连、零错包条件下测得。

### OpenOCD

| 目标芯片 | SWD 时钟 | 测试项 | 耗时 | 速度 |
| :--- | :---: | :--- | :---: | :---: |
| STM32F407VGT6 | 10 MHz | 128 KiB 整片读取（`dump_image`） | 0.99s | **128.98 KiB/s** |
| STM32F407VET6 | 10 MHz | 128 KiB 整片读取（`dump_image`） | 0.99s | **128.80 KiB/s** |
| STM32F407VET6 | 10 MHz | 140 KiB 纯写入 Flash（预擦除） | 1.26s | **111.67 KiB/s** |
| STM32F407VGT6 | 10 MHz | 140 KiB 纯写入 Flash（预擦除） | 1.27s | **110.25 KiB/s** |
| STM32F407VGT6 | 10 MHz | 140 KiB 目标端校验（`verify_image`） | 0.34s | **412.65 KiB/s** |
| STM32F407VGT6 | 10 MHz | 40.18 KiB 全流程（擦+写+校+复位，×3） | 2.35s ~ 2.39s | **16.8 ~ 17.1 KiB/s** |
| STM32F103C8T6 | 5 MHz | 64 KiB 整片读取（`dump_image`） | 0.57s | **112.33 KiB/s** |
| STM32F103C8T6 | 5 MHz | 32 KiB 纯写入 Flash（预擦除） | 1.02s | **31.35 KiB/s**（F103 硬件上限） |
| STM32F103C8T6 | 5 MHz | 40.17 KiB 全流程（擦+写+校+复位，×3 中位） | 3.30s | **12.19 KiB/s** |

### Keil MDK（`UV4 -f` 命令行三轮中位数）

| 目标芯片 | 镜像大小 | 三轮实测耗时 | 中位数速度 |
| :--- | :---: | :---: | :---: |
| STM32F407VGT6 | 40.18 KiB | 3.60s / 3.76s / 3.67s | **10.94 KiB/s** |
| STM32F407VET6 | 40.18 KiB | 3.84s / 3.73s / 3.78s | **10.64 KiB/s** |
| STM32F103C8T6 | 40.17 KiB | 7.60s / 5.46s / 5.38s | **7.36 KiB/s** |

*注：`UV4 -f` 计时包含 Keil 进程启停开销，在 IDE 界面里直接点 Download 体感会更快。*

### BLE 无线烧录（139B 有应答写 + 目标端 1KiB CRC32）

| 目标芯片 | 供电与时钟 | 镜像大小 | 擦除 | 写入 | 校验 | 总耗时 |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| STM32F407VGT6 | 双 USB，5 MHz | 40.18 KiB | 1.17s | 39.54s（1042 B/s） | 2.26s | **42.97s** |
| STM32F407VET6 | **纯无线**（F407 3.3V 供 CH592），2 MHz | 40.18 KiB | 1.28s | 39.82s（1035 B/s） | 2.28s | **43.38s** |
| STM32F103C8T6 | 2 MHz（擦 41 页） | 40.17 KiB | 3.89s | 39.85s（1034 B/s） | 2.28s | **46.02s** |

### 和同类调试器对比

在 USB Full-Speed（12 Mbps，64B Bulk）同级探针里，CH592-LINK 的有线速度已经基本摸到 USB FS 的实际瓶颈（~130 KiB/s）：

- **对比常见 DAPLink（CMSIS-DAP v1 HID / STM32F103）**：HID 读/写常见在 20~50 KiB/s，本项目 v2 Bulk + 硬件 SPI0 读 128.9 KiB/s、纯写 111.7 KiB/s，高出一截。
- **对比 ST-Link V2（USB FS）**：常见纯写 30~80 KiB/s、读 50~100 KiB/s，本项目在 F407 读和纯写上都能打平或小胜。
- **对比 USB High-Speed 探针（HS-Link / CH32V305 / J-Link OB HS / ST-Link V3）**：HS 探针有 512B Bulk 包和更高主频的优势，读取可达 300~1000+ KiB/s，接口代差摆在那里，不越级对标。
- **差异点**：单颗几块钱的 CH592F 在 26KiB RAM 里同时塞下了 USB CMSIS-DAP v2 + BLE 5.4 无线烧录栈，平时插线当高速 DAP 用，不方便接线时切网页蓝牙无线刷固件。

---

## 目录结构

| 目录 | 说明 |
| :--- | :--- |
| `firmware/` | 预编译好的 CH592 探针固件（`CH592-LINK-0.5.0.hex`），直接烧录即可 |
| `src/` | CH592 固件核心源码（硬件 SPI0 / 软 GPIO SWD 驱动、CMSIS-DAP v2、BLE 烧录状态机、目标芯片表） |
| `target_loader/` | 运行在目标 STM32 SRAM 里的 Thumb 擦写与 CRC32 校验程序（F1/F3 与 F4） |
| `web/` | 蓝牙无线烧录网页端（Web Bluetooth）及本地启动脚本 `start.cmd` / `serve.py` |
| `tools/` | 一键构建脚本 `build_ble.py` 与目标端 loader 编译脚本 `build_target_loaders.py` |
| `test_firmware/` | 实测用的 F103 / F407 示例测试固件（`.bin` / `.hex`，用于验证烧录与点灯） |
| `Ld/` | CH592 链接脚本（Flash 与 `.highcode` RAM 布局） |
| `ble_sdk/` | 南京沁恒 CH59x 官方 BLE 协议栈、外设驱动与启动文件 |
| `third_party/` | CherryUSB 1.5.3 USBFS 协议栈及 CH58x/CH59x 端口适配 |

---

## 从源码构建

环境要求（Windows）：

1. Python 3
2. WCH RISC-V GCC（实测 8.2.0），将环境变量 `CH592_RISCV_BIN` 指向其 `bin` 目录
3. Cortex-M Thumb 工具链（`arm-none-eabi-gcc` 或 Clang + LLD），若不在 `PATH` 中，设置 `CH592_THUMB_TOOLS`

在仓库根目录执行：

```bash
python tools/build_ble.py
```

输出文件（`.hex` / `.elf` / `.map` / `build_report.json`）在 `obj/ble_build/` 下，构建脚本会自动校验编译后保留栈空间不少于 4KiB。

---

## 更新记录（v0.4.4 → v0.5.0）

- **硬件 SPI0 2 线混合 SWD 驱动**：基于 CH592 SPI0（`PA13/PA15`、Mode 0、硬件 LSB First、8 字节 FIFO）重构 SWD 底层，最高支持 10MHz，保留 `CONFIG_SWD_HW_SPI` 一键回退软 GPIO。
- **5MHz 软 GPIO 稳定性修复**：保留软 GPIO 路径并放宽高速块读写余量，修复 `trn=0` 下遇到 `ACK_WAIT` 漏换向时钟的问题，块写结束补 8 个空闲低时钟再排空 `RDBUFF`。
- **网页诊断增强**：BLE SWD 诊断信息补全具体失败访存地址（`访存=0x...`），同步更新接线提示与 2MHz 无线档位建议。

第三方组件与许可证说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
