# ESP32-LCD 副屏 (WinUSB bulk)

## 硬件
- ESP32-S3 + 240x320 ST7789 LCD + USB 数据线

## PC 端设置 (一次性)

1. 安装 Python 依赖:
   pip install pyusb mss Pillow numpy

2. Windows 装 libusb-win32 或用 Zadig 把 ESP32 切到 WinUSB:
   - 下载 Zadig: https://zadig.akeo.ie/
   - 插 ESP32, 在 UsbDisp APP 点开启 (PID 0x4010 出现)
   - Zadig -> Options -> List All Devices -> 找 "ESP32_LCD_Display (0x4010)"
   - 选 WCID driver, Install

   (或装 libusb-win32: https://github.com/libusb/libusb/wiki/Windows)

3. 不需要关 Secure Boot, 不需要 test signing
   - ESP32 通过 MS OS 1.0 Descriptor 告诉 Windows 我接口是 WinUSB
   - Windows 自动加载内置 winusb.sys

## 使用

1. ESP32 插 USB, 等桌面
2. 左滑 UsbDisp 卡片 -> 进 APP
3. 点开启副屏 -> ESP32 切到 vendor 模式 (PID 0x4010)
4. Windows 自动加载 winusb.sys (几秒, 看设备管理器)
5. 跑 PC 脚本:
   python scripts/pc_push.py
6. LCD 实时显示 PC 截屏 (默认 15 fps @ 240x320)

## 协议
- 16 字节 packed 帧头 (xfz1986-compatible):
  - crc16(2) + type(1)=3(JPG) + cmd(1) + x_lo(2) + y_hi(2) + w_lo(2) + h_hi(2) + id10_len22(4)
  - id10_len22 = (frame_id & 0x3FF) << 22 | (payload_size & 0x3FFFFF)
- payload: 原始 JPEG bytes
- EP_OUT = 0x01 (host -> device bulk OUT)

## 故障排除
- 找不到设备:
  - 检查 ESP32 USB 连接
  - 检查 UsbDisp APP 已开启
  - Windows 设备管理器看 ESP32_LCD_Display 状态
- 装错驱动 (PID 0x4010 之前是 xfz1986 driver 占用):
  - 卸载 xfz1986 driver: pnputil /remove-device USB\VID_303A&PID_4010\0001
  - 重插 ESP32, 重新装 WinUSB
- 帧率低: 调 --quality 60 -> 40, 或调 --fps 15 -> 10
