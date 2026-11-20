# CH592-LINK

CH592 调试探针，当前验证目标为 STM32F103C8T6。USB 通过 CMSIS-DAP 接入 Keil；蓝牙通过本地网页写入 Intel HEX 或 BIN，并校验受影响的 Flash 页。

## 接线和使用

CH592 PA5 接目标 SWCLK，PA4 接 SWDIO，两板共地。目标板按其板级要求供电。

1. 将 `firmware/CH592-LINK-0.3.17.hex` 下载到 CH592 探针。
2. 使用蓝牙时，运行 `web/start.cmd`，在 Windows Chrome 或 Edge 中打开启动器给出的 localhost 地址。连接探针后选择目标程序。默认发送模式为 20B；使用 139B 有应答写需要 ATT MTU 至少为 142。
3. 使用 Keil 时，先释放网页蓝牙连接，再在目标工程的调试设置中选择 CMSIS-DAP。

网页默认逐页校验 CRC32，也可选择完整读回。擦写结果未知时，需重新建立会话，由用户决定是否重新烧录。

## 从源码构建

在 Windows 上安装 Python 3，准备 WCH RISC-V GCC（已验证 8.2.0）以及支持 Cortex-M3 Thumb 的 Clang 和 LLD。将环境变量 `CH592_RISCV_BIN` 和 `CH592_THUMB_TOOLS` 分别指向对应工具的 bin 目录。若工具已在 PATH 中，可省略环境变量。

从工程根目录运行 `python tools/build_ble.py`，生成的 HEX、ELF、MAP 和构建报告位于 `obj/ble_build/`。

## 实测

0.3.17 使用 139B 有应答写烧录 33,736B 镜像，写入 32.725s，擦除至校验完成共 38.198s，33 页 CRC32 全部通过。供对照，0.3.16 在同一份样例上的 Keil 完整下载中位数为 6.1082s（三轮）。

第三方组件及来源见 [THIRD_PARTY.md](THIRD_PARTY.md)。
