# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

嵌入式调试工具，基于 ESP32-S3-Touch-LCD-2 开发板，使用 ESP-IDF v5.5.3 构建。目标是将该开发板打造成多功能嵌入式调试工具，包含串口调试、ADC/DAC、逻辑分析仪等功能。

## Build Commands

```bash
# 设置 ESP-IDF 环境 (Windows)
D:\esp\v5.5.3\esp-idf\export.bat

# 进入项目目录
cd D:\Task\embedded_debugger_tool\Program\embedded_debug_tool

# 构建项目
idf.py build

# 烧录并监控串口
idf.py -p COMx flash monitor

# 仅监控串口
idf.py -p COMx monitor

# 清理构建
idf.py fullclean
```

## Architecture

项目采用模块化 APP 架构，每个功能作为独立 APP 运行，功能切换需完全退出当前 APP 才能进入下一个。

```
D:\Task\embedded_debugger_tool\
├── Program\embedded_debug_tool\   ← 主项目 (ESP-IDF)
│   ├── CMakeLists.txt
│   └── main\main.c
├── 示例程序\                       ← 参考例程 (勿修改)
│   └── ESP32-S3-Touch-LCD-2-Demo\
│       ├── ESP-IDF\               ← ESP-IDF 例程
│       ├── Arduino\               ← Arduino 例程
│       └── Firmware\              ← 固件
└── ESP32-S3-Touch-LCD-2-SchDoc.pdf  ← 硬件原理图
```

## Hardware — GPIO Pin Mapping

从原理图提取的完整 GPIO 分配表。**摄像头 IO（IO2/IO6-IO17/IO21）当前未使用，可自由配置为其他功能。**

| GPIO | 功能复用 | 说明 | 可用性 |
|------|---------|------|--------|
| **IO0** | LCD_RST / BOOT | LCD 复位（R16 NC/0R 可选），BOOT 按键 | ⚠️ 复用 |
| **IO1** | LCD_BL | LCD 背光控制（SS8050 NPN 三极管驱动） | ⚠️ 已用 |
| **IO2** | CAM_D7 | 摄像头数据位 7 | ✅ **可用** |
| **IO3** | IMU_INT1 | QMI8658 中断输出 | ⚠️ 已用 |
| **IO4** | CAM_HREF / BAT_ADC | 电池电压 ADC 采样（R19 200K / R20 100K 分压） | ⚠️ 复用 |
| **IO5** | — | 空闲 | ✅ 可用 |
| **IO6** | CAM_VSYNC | 摄像场同步 | ✅ **可用** |
| **IO7** | CAM_D6 | 摄像头数据位 6 | ✅ **可用** |
| **IO8** | CAM_XCLK | 摄像头外部时钟 | ✅ **可用** |
| **IO9** | CAM_PCLK | 摄像头像素时钟 | ✅ **可用** |
| **IO10** | CAM_D5 | 摄像头数据位 5 | ✅ **可用** |
| **IO11** | CAM_D3 | 摄像头数据位 3 | ✅ **可用** |
| **IO12** | CAM_D0 | 摄像头数据位 0 | ✅ **可用** |
| **IO13** | CAM_D1 | 摄像头数据位 1 | ✅ **可用** |
| **IO14** | CAM_D4 | 摄像头数据位 4 | ✅ **可用** |
| **IO15** | CAM_D2 | 摄像头数据位 2 | ✅ **可用** |
| **IO16** | TWI_CLK | 摄像头 I2C 时钟（SCCB） | ✅ **可用** |
| **IO17** | CAM_PWDN | 摄像头电源控制（低有效，R6 10K 下拉默认关） | ✅ **可用** |
| **IO18** | — | 空闲 | ✅ 可用 |
| **IO19** | USB_N | USB 数据负（D-） | ❌ 占用 |
| **IO20** | USB_P | USB 数据正（D+） | ❌ 占用 |
| **IO21** | TWI_SDA | 摄像头 I2C 数据（SCCB） | ✅ **可用** |
| **IO33** | LCD_MOSI / SD_MOSI | LCD & SD SPI 数据（共享总线） | ❌ 占用 |
| **IO34** | LCD_SCLK / SD_SCLK | LCD & SD SPI 时钟（共享总线） | ❌ 占用 |
| **IO35** | SD_MISO | SD SPI 数据输出 | ⚠️ 已用 |
| **IO36** | SD_CS | SD 片选 | ⚠️ 已用 |
| **IO37** | LCD_DC | LCD 数据/命令选择 | ⚠️ 已用 |
| **IO38** | LCD_CS | LCD 片选 | ⚠️ 已用 |
| **IO39** | TP_INT | CST816 触摸中断 | ⚠️ 已用 |
| **IO40** | TP_SDA / IMU_SCL | 触摸 & IMU I2C 总线（SCL，4.7K 上拉至 3V3） | ❌ 占用 |
| **IO41** | TP_SCL / IMU_SDA | 触摸 & IMU I2C 总线（SDA，4.7K 上拉至 3V3） | ❌ 占用 |
| **IO42** | U0_TXD | UART0 发送（调试串口） | ⚠️ 已用 |
| **IO43** | U0_RXD | UART0 接收（调试串口） | ⚠️ 已用 |
| **IO44** | — | 空闲 | ✅ 可用 |

### 可用 GPIO 汇总

| 类别 | GPIO | 说明 |
|------|------|------|
| **摄像头 IO（全部可用）** | IO2, IO6, IO7, IO8, IO9, IO10, IO11, IO12, IO13, IO14, IO15, IO16, IO17, IO21 | 摄像头未使用，14 个 GPIO 可自由配置 |
| **完全空闲** | IO5, IO18, IO44 | 无任何硬件连接 |
| **ADC** | IO4 | BAT_ADC 通道（12-bit，R19/R20 分压，可读取外部模拟信号） |

## Hardware — Module Details

### MCU — ESP32-S3R8
- Flash: W25Q128JVSI (16MB) SPI Flash，接 IO47(SPIHD)/IO48(SPIWP)/专用 SPI 引脚
- 晶振: 40MHz 主晶振 (X1) + 32.768K RTC 晶振
- USB: IO19/IO20 经 22Ω 串接电阻直连 USB Type-C（内置 USB-OTG）
- 启动: BOOT 按键接 IO0，ESP_EN 按键接 EN 引脚（R8 10K 上拉）

### LCD — ST7789T3 (2" 240×320)
- 接口: SPI（与 SD 卡共享 MOSI/SCLK 总线，不同 CS）
- 连接器: J3 HXR20062C21（21pin FPC）
- 引脚: CS=IO38, DC=IO37, SCLK=IO34, MOSI=IO33, RST=IO0, BL=IO1
- 背光电路: IO1 → R12(1K) → T1(SS8050 NPN, B=IO1/E=GND/C=LEDA) → LEDK → R11(6.8Ω) → 3V3
- R16 为 NC/0R 可选，R18 为 NC/10K 可选（LCD_CS 上拉）

### 触摸屏 — CST816D (电容触摸)
- 接口: I2C（与 IMU 共享总线）
- 引脚: SDA=IO40, SCL=IO41, INT=IO39, RST=3V3
- I2C 上拉: R29=4.7K (SCL→3V3), R30=4.7K (SDA→3V3)
- 供电: 3V3

### 摄像头接口 — 24pin FPC (J1)（**当前未使用，IO 可复用**）
- 接口: 8-bit 并行 DVP
- 数据: D0=IO12, D1=IO13, D2=IO15, D3=IO11, D4=IO14, D5=IO10, D6=IO7, D7=IO2
- 同步: PCLK=IO9, VSYNC=IO6, HREF=IO4, XCLK=IO8
- 控制: PWDN=IO17 (R6 10K 下拉默认关), TWI_CLK=IO16, TWI_SDA=IO21
- 供电: AVDD=2.8V (U4 RT9166A-28PXL), DVDD=1.5V (U1 RT9166A-15PXLR)
- **注意**: TWI_CLK/TWI_SDA 是摄像头专用 I2C，与触摸/IMU 的 I2C 总线**不同**
- **复用提示**: 所有摄像头 IO 均可配置为 GPIO/UART/SPI/I2C/PWM 等功能

### SD 卡槽 — TF1 (Micro SD)
- 接口: SPI 模式
- 引脚: CS=IO36, MOSI=IO33, SCLK=IO34, MISO=IO35
- 注意: MOSI/SCLK 与 LCD 共享，使用时需通过 CS 分时复用

### IMU — QMI8658C (六轴)
- 接口: I2C（与触摸屏共享总线）
- 引脚: SCL=IO40, SDA=IO41, INT1=IO3
- I2C 地址: **0x6A**（原理图 SDO/SA0 经 R29/R30 区域接 3V3）
- I2C 上拉: 与触摸屏共用 R29=4.7K, R30=4.7K

### USB — Type-C (16pin, H1)
- 数据: USB_P=IO20, USB_N=IO19
- CC 配置: R21=5.1K (CC1), R28=5.1K (CC2)，各接 5.1K 下拉至 GND，标识为 Device
- 串接电阻: R23=22Ω (D+), R26=22Ω (D-)
- 供电: VBUS 5V 输入

### 电源管理
- **DC-DC 3V3**: TMI3112H (U7)，VCC 5V → 3V3，L4=2.2μH, C35=22μF, C34=22pF
- **电池充电**: ETA6098 (U6)，VCC 5V → VBAT，L3=2.2μH, R15=5K (电流设定)
- **LDO 2.8V**: RT9166A-28PXL (U4)，3V3 → 2V8（摄像头 AVDD）
- **LDO 1.5V**: RT9166A-15PXLR (U1)，3V3 → 1V5（摄像头 DVDD）
- **电池检测**: BAT_ADC = IO4，经 R19(200K)/R20(100K) 分压（1/3 分压，VBAT 最大 4.2V 时 ADC 约 1.4V）
- **指示灯**: LED1 (红色，充电状态，接 U6 STAT 引脚)，LED2 (红色，电源指示，经 Q1 AO3401 P-MOS 控制)

### 外部排针 — P1/P2 (2×14pin)
- P1: IO2, IO4, IO6, IO16, IO17, IO18, IO21, IO8, IO7, IO10, IO20, IO19, GND, 5V
- P2: 3V3, GND, IO43, IO44, IO47, IO48, IO15, IO13, IO11, IO12, IO14, IO9, GND, VBAT

## Key Dependencies (from reference examples)

- `esp_lcdPanel` — LCD 驱动 (ST7789)
- `esp_lcd_touch` — 触摸驱动 (CST816)
- `lvgl` — UI 框架
- `esp_lcd_qmi8658` / `FastIMU` — IMU 驱动
- `sdmmc` / `esp_vfs_fat` — SD 卡驱动

## Planned Features (per 需求文档)

1. **串口转 TCP/IP** — UART 数据通过 WiFi-AP 模式转发，PC 通过 TCP 读取
2. **Web 串口显示** — 极简 Web 页面查看串口数据，支持波特率设置、连接/断开控制
3. **LCD 串口显示** — 通过 LVGL 在屏幕显示串口数据
4. **串口总线监控** — 作为中间设备监控两个设备间的串口通信
5. ADC 输出 / DAC 读取 / 逻辑分析仪 / GPIO 控制

## Notes

- 参考例程位于 `示例程序\ESP32-S3-Touch-LCD-2-Demo\ESP-IDF\`，包含 SD 卡、LVGL、IMU、摄像头等示例
- 硬件原理图在根目录 `ESP32-S3-Touch-LCD-2-SchDoc.pdf`
- ESP-IDF 官方文档: https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.3/esp32/index.html
- 项目尚处于早期阶段，main.c 当前为空
- LCD 和 SD 卡共享 SPI 总线 (IO33/IO34)，需分时复用
- 触摸屏和 IMU 共享 I2C 总线 (IO40/IO41)，上拉电阻 R29/R30=4.7K
- **摄像头未使用，IO2/IO6-IO17/IO21 共 14 个 GPIO 可自由配置**，是本项目最重要的扩展资源
- IO5/IO18/IO44 为完全空闲 GPIO
- IO4 (BAT_ADC) 可复用为普通 ADC 输入，但需注意与电池检测分压电路的冲突
- IO17 控制摄像头电源（CAM_PWDN），R6 10K 下拉默认低电平（摄像头关），如需使用该 GPIO 注意上电初始状态
- 电池电压检测: IO4 经 R19(200K)/R20(100K) 分压，ADC 读值 ×3 ≈ VBAT
