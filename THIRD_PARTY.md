# 组件来源

- USB 协议栈取自 [CherryUSB](https://github.com/cherry-embedded/CherryUSB) 1.5.3，采用 Apache-2.0，许可证随源码保存在 [third_party/cherryusb/LICENSE](third_party/cherryusb/LICENSE)。CH58x USBFS 端口增加了条件编译适配。
- `ble_sdk/` 包含南京沁恒的 CH59x BLE 库、启动文件、外设驱动及 ISP 库。保留了厂商文件中的版权与芯片用途声明。对外分发 SDK 文件、静态库和已链接固件前，需核实所用版本的随包授权。
- 工程启动与链接布局与本地 CH592_Template 例程同源，模板的原始发布者及单独授权尚待确认。
