# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

嵌入式调试工具，基于 ESP32-S3-Touch-LCD-2 开发板，使用 ESP-IDF v5.5.3 构建。当前实现：UART-TCP/WebSocket 透传桥接器 + LVGL LCD 显示。

## Build Commands

```bash
D:\esp\v5.5.3\esp-idf\export.bat
cd D:\Task\embedded_debugger_tool\Program\Embedded_debug_tool
idf.py build
idf.py -p COMx flash monitor
idf.py fullclean
```

## Architecture

模块化功能划分，每个文件夹 = 一个功能模块，drv_ 和 app_ 在同一文件夹内：

```
Embedded_debug_tool/
├── uart/          ← UART 硬件驱动 + 桥接逻辑
│   ├── drv_uart.h/c    ← 系统 API 封装
│   └── app_uart.h/c    ← 业务逻辑（转发/发送/定时器）
├── wifi/          ← WiFi AP
│   ├── drv_wifi.h/c
│   └── app_wifi.h/c
├── tcp/           ← TCP 服务器
│   └── app_tcp.h/c
├── web/           ← HTTP + WebSocket
│   ├── app_web.h
│   ├── app_web_http.c
│   ├── app_web_ws.c
│   └── app_web_broadcast.c
├── display/       ← LVGL LCD 显示
│   ├── drv_display.h/c  ← ST7789 + CST816S 硬件初始化
│   └── app_display.h/c  ← LVGL 适配器 + UI
└── main/
    ├── main.c           ← 初始化入口
    └── CMakeLists.txt   ← 所有源文件路径集中管理
```

**依赖规则**: drv_ → 系统 API; app_ → drv_ + 其他 app_; main → 只调用 app_xxx_start/init

## Hardware — GPIO Pin Mapping

### 当前使用引脚

| 功能 | TX | RX | 说明 |
|------|----|----|------|
| UART1 | IO2 | IO4 | 串口1 |
| UART2 | IO16 | IO17 | 串口2 |
| LCD SPI | MOSI=IO38 | SCLK=IO39 | ST7789 |
| LCD 控制 | CS=IO45, DC=IO42 | RST=IO0, BL=IO1 | |
| Touch I2C | SDA=IO48 | SCL=IO47, INT=-1 | CST816S |

### 完整 GPIO 分配表

| GPIO | 功能 | 可用性 |
|------|------|--------|
| **IO0** | LCD_RST / BOOT | ⚠️ 复用 |
| **IO1** | LCD_BL | ⚠️ 已用 |
| **IO2** | UART1 TX | ⚠️ 已用 |
| **IO3** | IMU_INT1 | ⚠️ 已用 |
| **IO4** | UART1 RX / BAT_ADC | ⚠️ 复用 |
| **IO5** | 空闲 | ✅ 可用 |
| **IO6-IO15** | 摄像头 IO（未使用） | ✅ 可用 |
| **IO16** | UART2 TX | ⚠️ 已用 |
| **IO17** | CAM_PWDN | ✅ 可用 |
| **IO18** | 空闲 | ✅ 可用 |
| **IO19/20** | USB D-/D+ | ❌ 占用 |
| **IO21** | 摄像头 I2C | ✅ 可用 |
| **IO33** | LCD_MOSI / SD_MOSI | ❌ 占用 |
| **IO34** | LCD_SCLK / SD_SCLK | ❌ 占用 |
| **IO35** | SD_MISO | ⚠️ 已用 |
| **IO36** | SD_CS | ⚠️ 已用 |
| **IO37** | — | ✅ 可用 |
| **IO38** | LCD_MOSI | ⚠️ 已用 |
| **IO39** | LCD_SCLK | ⚠️ 已用 |
| **IO40** | IMU_SCL / TP_SDA | ❌ 占用 |
| **IO41** | IMU_SDA / TP_SCL | ❌ 占用 |
| **IO42** | LCD_DC | ⚠️ 已用 |
| **IO43** | U0_RXD | ⚠️ 已用 |
| **IO44** | 空闲 | ✅ 可用 |
| **IO45** | LCD_CS | ⚠️ 已用 |
| **IO46** | Touch INT | ⚠️ 已用 |
| **IO47** | Touch SCL | ⚠️ 已用 |
| **IO48** | Touch SDA | ⚠️ 已用 |

### 可用 GPIO

| 类别 | GPIO |
|------|------|
| 摄像头 IO（未使用） | IO6-IO15, IO21, IO37 |
| 完全空闲 | IO5, IO18, IO44 |

## Hardware — Module Details

### LCD — ST7789T3 (2" 240×320)
- SPI: MOSI=IO38, SCLK=IO39, CS=IO45, DC=IO42, RST=-1, BL=IO1
- 时钟: 80MHz, RGB 顺序, 需要颜色反转
- 参考例程: `示例程序/ESP32-S3-Touch-LCD-2-Demo/ESP-IDF/06_lvgl_example/`

### 触摸屏 — CST816S
- I2C: SDA=IO48, SCL=IO47, INT=-1, RST=-1
- 地址: ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS

### 已实现功能
1. **WiFi AP** — SSID: Embedded-debug-tool, 无密码, 192.168.4.1
2. **UART-TCP 透传** — UART1:8080, UART2:8081
3. **Web 界面** — http://192.168.4.1/ (HTTP + WebSocket 实时推送)
4. **LCD 显示** — 白底黑字，显示 IP/端口/IO 映射信息

### 未使用硬件
- 摄像头接口: 24pin FPC (J1), IO2/IO6-IO17/IO21 共 14 个 GPIO 可复用
- SD 卡槽: CS=IO36, MOSI=IO33, SCLK=IO34, MISO=IO35
- IMU: QMI8658C, I2C (IO40/IO41), 地址 0x6A

## Notes

- ESP-IDF: v5.5.3, 目标: ESP32-S3
- 原理图: `ESP32-S3-Touch-LCD-2-SchDoc.pdf`
- 参考例程: `示例程序/ESP32-S3-Touch-LCD-2-Demo/ESP-IDF/`
- LCD/SD 共享 SPI 总线 (IO33/IO34), 需分时复用
- 触摸/IMU 共享 I2C 总线 (IO40/IO41)
- LVGL 组件位于 `components/` (lvgl, esp_lvgl_adapter, esp_lcd_touch*)
- sdkconfig 变更需 `idf.py fullclean && idf.py build`
