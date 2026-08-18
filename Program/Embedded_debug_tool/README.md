# ESP32-S3 嵌入式调试工具

基于 ESP32-S3-Touch-LCD-2 开发板（2" 240×320 ST7789 + CST816S 触摸）的多功能调试设备。
桌面启动器 + 多 APP 架构，支持 UART-TCP/WebSocket 透传、SD 文件浏览、TXT 阅读、串口终端、网络信息显示。

- 平台：ESP32-S3 + ESP-IDF v5.5.3 + LVGL 9.4
- 屏幕：ST7789（SPI，80MHz）+ CST816S 触摸（I2C），支持运行时旋转 0/90/180/270°

## 功能

| APP | 说明 |
|-----|------|
| Files | SD 卡文件浏览器（目录导航/排序/打开 txt 跳转阅读器） |
| Reader | TXT 阅读器（书架模式：扫描 SD 列书；直接打开模式：从文件浏览器跳转） |
| Terminal | 串口终端（UART1/UART2 日志滚动显示、Pause/Clear/切换） |
| SerialIP | 网络信息（WiFi AP/IP/Web 地址 + Web 服务启停） |

系统能力：
- WiFi AP（Embedded-debug-tool，无密码，192.168.4.1）
- UART1 → TCP :8080 / UART2 → TCP :8081 透传桥接
- Web 界面（HTTP + WebSocket 实时数据 + 远程控制）
- 手势：屏幕左边缘右滑 = 返回（目录浏览器返回上一级 / 关闭 APP 回桌面）
- 横竖屏旋转自适应

## 构建与烧录

```bash
D:\esp\v5.5.3\esp-idf\export.bat
idf.py build
idf.py -p COMx flash monitor
```

## 代码结构（架构分层）

```
main/                  # 组装层：初始化服务 → UI 平台 → 启动桌面
services/              # 服务层（与 UI 无关）
  uart/  wifi/  tcp/  web/  sdcard/
platform/              # 平台层
  display/             # LVGL 平台：初始化/旋转/字体/SD 通知
  input/               # 手势层：触摸坐标旋转映射 + 右滑返回检测
  components/          # 公共 UI 组件
    flow_view/         # 滚动内容流视图（终端日志 + 阅读器渲染共用）
    font_manager/      # 字体管理
apps/                  # 应用层（每 APP 独立文件夹 + 统一描述符）
  launcher/            # 桌面 + APP 返回栈 + 系统事件路由
  reader/              # 阅读器 APP（书架页 + 阅读页）
  file_browser/        # 文件浏览器
  terminal/            # 串口终端
  net_console/         # 网络信息
  test/                # APP 回调测试模块（默认关闭）
  app_manifest.h       # 统一 APP 描述符
```

### APP 统一接口（app_manifest.h）

每个 APP 通过描述符表对外提供回调，launcher 统一调度，不关心 APP 内部实现：

```c
typedef struct app_manifest {
    launch_app_id_t id;
    const char *name;
    void *(*launch)(lv_obj_t *parent, void (*back)(void *ctx), void *ctx);  /* 创建 APP */
    void (*destroy)(void *app);        /* 销毁 */
    bool (*back)(void *app);           /* 返回事件：true=关闭，false=内部处理 */
    void (*rotate)(void *app, int deg);/* 旋转事件 */
    void (*refresh)(void *app);        /* SD 就绪事件 */
    void (*debug_event)(void *app, int evt);  /* 调试事件（测试用） */
} app_manifest_t;
```

### 系统事件路由

```
右滑手势 → gesture 层 → launcher_app_swipe_back → 栈顶 APP.back()
屏幕旋转 → app_display → launcher_event_rotate → 栈顶 APP.rotate()
SD 挂载 → app_display → launcher_event_sd_ready → 栈顶 APP.refresh()
```

- APP 返回栈：深度 4，压栈保活（来源 APP 状态天然保留），弹栈恢复
- 带参启动：`launcher_app_launch(id, arg)`，阅读器 arg=文件路径 = 直接打开

### 测试模块（apps/test/）

遍历 manifest 自动验证各 APP 回调链路（launch/debug/back/rotate/refresh）。默认关闭：
1. `main/CMakeLists.txt` 取消 `apps/test/app_test.c` 注释
2. `main/main.c` 将测试块 `#if 0` 改为 `#if 1`

## 硬件引脚

| 功能 | 引脚 |
|------|------|
| UART1 TX/RX | IO2 / IO4 |
| UART2 TX/RX | IO16 / IO17 |
| LCD MOSI/SCLK/CS/DC | IO38 / IO39 / IO45 / IO42 |
| LCD RST/BL | IO0 / IO1 |
| Touch SDA/SCL | IO48 / IO47 |
| SD CS | IO36 |

完整 GPIO 分配见 CLAUDE.md。

## 注意事项

- SD 与 LCD 共享 SPI2 总线：SD 枚举/扫描必须与 LVGL 渲染同线程（持锁），避免跨任务并发触发 SPI 断言
- LVGL 9 事件 filter 是精确枚举比较，多事件需分别注册
- sdkconfig 变更需 `idf.py fullclean && idf.py build`
- 内部 RAM 紧张（显示双缓冲 ~51KB），任务栈放 PSRAM
