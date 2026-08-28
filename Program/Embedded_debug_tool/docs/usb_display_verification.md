# USB 副屏 APP — 验证文档

## 实施状态（2026-08-27）

### 已完成
- 完整代码实现（服务层 + APP 层 + Launcher 集成）
- 三个现有 USB 服务的互斥检查（card_reader / dap / usb2ttl）
- SDK 改动：Vendor class 启用 + PID 0x2987 + product string 改为 xfz1986 协议要求
- 编译通过（binary 3.4MB / 45% 留白）
- 烧录成功（COM14）
- 设备正常启动，桌面 / 自测模式均工作
- **Windows 端验证：设备在 USB 设备列表中以 VID_303A PID_2987 正确枚举**（核心硬件/协议层 OK）
- PID_2987 在 ENABLE 时出现，DISABLE 后消失 — USB PHY 切换正确

### 待用户手动完成
1. **Windows 端首次：安装 xfz1986 驱动**
   - 下载：https://dl.espressif.com/AE/esp-iot-solution/xfz1986_usb_graphic_250224_rc_sign.exe
   - 安装：双击运行，按提示确认（已签名，无需 test mode）
   - 或参考源码：https://github.com/chuanjinpangang/win10_idd_xfz1986_usb_graphic_driver_display

2. **插入 USB 后 Windows 显示设置中应出现 "ESP USB Display" 240×320 显示器**

3. **将窗口拖到副屏位置** 即可在 ESP32-LCD 上看到画面

### 自测模式（可选）
在 `main/main.c` 顶部将：
```c
#define USDISP_SELFTEST_ENABLE     0   // 改 1 启用
```
启用后：开机 5 秒后自动开启副屏 → 持续 15 秒 → 自动关闭 → 持续 5 秒 → 循环
监控日志标签：`usdisp_selftest` / `app_usbdisp`

## 关键设计点

### USB 协议（xfz1986）
帧头 16 字节 packed：
```
uint16_t crc16;          // payload CRC16（设备忽略）
uint8_t  type;           // 0=RGB565, 3=JPG（本项目只用 JPG）
uint8_t  cmd;            // 命令码（保留）
uint16_t x_lo, y_hi;     // x = (y_hi<<16)|x_lo
uint16_t w_lo, h_hi;     // h = (h_hi<<16)|w_lo
uint32_t id10_len22;     // payload_total = id10_len22 & 0x3FFFFF (max 4MB)
```
帧结束：short packet (< 64B EP size) 或 payload 累计到 total

### 数据流
PC IDD 驱动 → USB Vendor bulk OUT EP → tud_vendor_rx_cb (USB 任务)
→ 协议帧解析 → s_frame_ready 标志 → decode_task (核心 1)
→ esp_new_jpeg 硬解 → RGB565 写入共享缓冲 → lv_timer (LVGL 线程)
→ lv_image_set_src + lv_obj_invalidate → LVGL flush
→ esp_lcd_panel_draw_bitmap → ST7789

### 互斥（与现有 3 个 USB 服务）
`app_usbdisp_enable()` 开头检查 card_reader/dap/usb2ttl 状态；
这三个服务的 enable() 也加了对 USDISP_ACTIVE 的检查。

### 内存（PSRAM）
- 帧 payload 缓冲：300 KB
- RGB565 ping-pong 双缓冲：2 × 153 KB = 306 KB
- JPEG 解码器工作内存：~50 KB（按需，esp_new_jpeg 内部 alloc）
- 端点 / 描述符 / 协议解析状态：~5 KB 内部 RAM
- LVGL 现有双缓冲（24 行）保留：~30 KB 内部 RAM

## APP UI 行为

### 桌面
- 卡片名："UsbDisp"（最右一张）
- 图标：显示器 + USB 插头

### APP 内
- IDLE 状态：标题 + PC 连接状态 / 服务状态 / 帧率 / 错误数 + "开启副屏" 按钮
- 点击按钮 → 调 `app_usbdisp_enable()` → 进入 STREAMING 状态
- STREAMING 状态：全屏 `lv_image` + 左上角"← 退出" 角标 + 右上角 FPS 角标
- 左滑（贴边右滑）：退出 STREAMING → 调 `app_usbdisp_disable()` → 回 IDLE
- 再次左滑：关 APP 回桌面
