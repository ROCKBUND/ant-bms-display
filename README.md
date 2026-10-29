# 蚂蚁 BMS 圆屏 · ESP32-S3

基于 ESP32S3-N16R16-128SPIT 一体板的 Arduino 工程，通过 BLE 读取蚂蚁保护板数据，在 GC9A01 240×240 圆屏上显示电量、电压、电流、功率和电池详情。

当前版本：v0.11。采用 LVGL 8.3.11 黑底青绿界面，支持开机与连接后的电量加载动画、充电流光、蓝牙设备选择和自动重连；采用局部刷新，并修正顶部状态文字的刷新区域。当前硬件为无触摸版，使用三个侧键操作。

## UI 预览

界面依次为首页、电池详情和蓝牙选择页。以下为 UI 布局预览，图中使用示例数据，非实机截图。

![蚂蚁 BMS 圆屏 UI：电量首页、电池详情和蓝牙选择](docs/images/ui-preview.png)

## 支持的保护板型号与范围

本工程面向采用 **蚂蚁 ANT BLE 实时数据协议（`7E A1 … AA 55`）** 的智能保护板，通过低功耗蓝牙读取数据。兼容性取决于蓝牙接口和固件协议，不能仅凭“蚂蚁”、串数或额定电流判断。

| 设备名称 / 型号 | 软件版本 / 硬件信息 | 本工程适配情况 |
|---|---|---|
| `ANT-BLE24CAUB` | 软件 `24CAUB00-230524B` | 当前默认适配目标；这是蓝牙设备名称，并非完整硬件型号 |
| `ANT-BLE16ZMUB` | 软件 `16ZMUB00-220501A`；硬件 `16ZMB0TB120A` | 社区 BLE 适配记录，本工程待验证 |
| `ANT-BLE24BHUB` | 软件 `24BHUB00-211026A`；硬件 `24BH` | 社区 BLE 适配记录，本工程待验证 |
| `ANT-BLE04DMUB` | 软件 `04DMUB00-240122B`；硬件 `04DMA0TB120A` | 社区 BLE 适配记录，本工程待验证 |
| `ANT@BLE22AAUB` | 软件 `22AAUB00-241008A`；硬件 `22PHB8TB130A` | 社区 BLE 适配记录，本工程待验证 |

上述社区型号来自 [esphome-ant-bms 的 BLE 支持记录](https://github.com/syssi/esphome-ant-bms#supported-devices)，属于其他项目的适配结果，不能作为本工程已实测支持的证明。当前没有完整的逐型号实测兼容清单。

使用其他型号前，需要确认：

- BLE 服务为 `0xFFE0`，特征为 `0xFFE1`，支持写入查询和通知返回数据。
- 能响应本工程的状态查询 `7E A1 01 00 00 BE 18 55 AA 55`，并返回字段布局与解析器一致的 `7E A1 11 … AA 55` 状态帧。仅帧头相同还不足以确认兼容。
- 当前解析器接受 **1～32 串、最多 4 路电池温度**；这是软件解析范围，不代表所有相应串数的保护板均已验证。
- 首次连接后，与手机 App 核对电压、电流、电量及容量；能扫描到设备或显示已连接，不等于已经正确读取数据。

蚂蚁旧协议、仅有经典蓝牙或 UART/RS485/CAN 接口的设备，以及极空 JK、嘉佰达 JBD、达锂 DALY 等其他品牌协议，当前均未适配。程序仅查询和显示状态，不提供保护参数修改或充放电开关控制。

协议格式可参考[蚂蚁官方实时数据协议文档](https://support-wiki.antbms.com/docs/app-help-documentation/bluetooth-protocol/connection-protocol/chinese/index.html)；本工程实际实现以 [BmsLink.cpp](AntRoundS3/BmsLink.cpp) 和 [AntProtocol.h](AntRoundS3/AntProtocol.h) 为准。

## 快速开始

1. 下载并完整解压本仓库，或使用 `git clone https://github.com/ROCKBUND/ant-bms-display.git`。
2. 在 Arduino IDE 中安装 ESP32 开发板包 **3.3.10**，以及 **GFX Library for Arduino 1.6.9**、**NimBLE-Arduino 2.5.0**。
3. 打开 `AntRoundS3/AntRoundS3.ino`，保留同目录所有 `.h`、`.cpp` 和完整 `src/` 目录。LVGL 已内置，无需单独安装。
4. 按[工程说明](AntRoundS3/README.md)设置开发板、Flash、PSRAM 和 USB 选项。
5. 在 `AntRoundS3/Config.h` 中核对目标蓝牙名称和地址，或使用侧键进入蓝牙选择页选择自己的保护板。

## 侧键操作

| 按键 | 数据页面 | 蓝牙选择页面 |
|---|---|---|
| 上键 GPIO14 | 进入蓝牙选择 | 返回 |
| 中键 GPIO15 | 进入蓝牙选择 | 连接选中设备；无候选时重新扫描 |
| 下键 GPIO16 | 翻页 | 下一个设备 |

中键长按约 3 秒由硬件控制开关机。

## 源码和许可

- `AntRoundS3/`：完整 Arduino 工程及内置 LVGL 源码。
- [硬件、依赖与使用说明](AntRoundS3/README.md)。
- [第三方来源与许可证](AntRoundS3/THIRD_PARTY.md)。
- [历史源码检查记录](AntRoundS3/SOURCE_CHECKS.md)。
- [项目许可证](LICENSE)，第三方代码和字体保留各自许可证与版权声明。
