# AGENTS.md

ESP32-S3 嵌入式调试工具（ESP-IDF v5.5.3 + LVGL 9.4）。项目根 = `Program/Embedded_debug_tool/`（git 仓库根在上级 `D:\Task\embedded_debug_tool`）。更完整说明见 `CLAUDE.md`。

## 构建 / 烧录（非标准环境，必须手动设 env）

PowerShell 下直接跑 `idf.py` 会失败，必须先设置工具链 PATH 再调用：

```powershell
$env:IDF_PATH = "D:\esp\v5.5.3\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:PATH = "C:\Espressif\tools\python\v5.5.3\venv\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;" + $env:PATH
python "D:\esp\v5.5.3\esp-idf\tools\idf.py" build      # 编译
python "D:\esp\v5.5.3\esp-idf\tools\idf.py" -p COM14 flash   # 烧录
```

- `idf.py fullclean` 后必须**先 build 再 flash**，否则 flash 报 `ninja failed with exit code 1`
- **烧录前先杀残留 monitor**：`Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -match 'monitor' } | Stop-Process`，否则 COM14 被占
- 设备断开后重新插可能换 COM 号，用 `[System.IO.Ports.SerialPort]::GetPortNames()` 查
- sdkconfig 变更需 `fullclean && build`

## 编码陷阱（极易踩）

- 源文件含 UTF-8 中文，**部分历史注释字节已损坏**（read 工具显示 `�`）
- **不要用 PowerShell `Set-Content`/`Get-Content` 编辑 .c/.h**——会按 GBK 读或破坏 UTF-8
- 用 opencode 的 `edit`/`write` 工具改源码；批量替换非 ASCII 内容时改用 Python 脚本读写 UTF-8

## 架构（本次重构后）

```
main/       组装：services → display 平台 → launcher；默认不启动 SoftAP
uart wifi tcp web sdcard/   服务层（与 UI 无关，drv_+app_）
display/    LVGL 平台：初始化/旋转/字体/SD 通知
platform/
  input/gesture.c   触摸坐标旋转映射 + 手势（贴边/候选/触发/禁单击）
  components/       flow_view（终端+阅读器共用）、font_manager、reader
apps/       每 APP 独立文件夹 + 统一描述符
  launcher/   桌面 + APP 返回栈（深度4）+ 事件路由
  reader/     阅读器（书架模式 + arg 直接打开）
  file_browser/ net_console/ terminal/
  app_manifest.h   统一 APP 接口（见下）
  test/       回调测试模块（默认 CMake 注释关闭）
```

- **APP 依赖方向**：服务层 ← display 平台 ← launcher ← APP。display 平台**不得**引用具体 APP（除启动 launcher 外）
- **统一 APP 接口** `apps/app_manifest.h`：`launch/destroy/back/rotate/refresh/debug_event` 回调表 + launcher 返回栈调度。带参启动用 `launcher_app_launch(id, arg)`，APP 内 `launcher_app_get_arg()` 取参（阅读器 arg=txt 路径）
- **系统事件路由**：右滑返回 → gesture → `launcher_app_swipe_back`；旋转 → `launcher_event_rotate`；SD 就绪 → `launcher_event_sd_ready`
- 右滑手势参数在 `platform/input/gesture.c`：`SWIPE_EDGE_X=25`、`SWIPE_CANDIDATE_DX=10`、`SWIPE_MIN_DX=30`

## LVGL 9 关键坑

- **事件 filter 是精确枚举比较**：多事件必须逐个 `lv_obj_add_event_cb`，不能位或
- `transform_scale` 单位是 **256 的倍数**（`LV_SCALE_NONE=256`=100%）；设 100 会缩到 39%。像素级尺寸用 `transform_width/height`（只改绘制边界，不缩放子对象内容）
- 透明度常量只有 `LV_OPA_0/10/20/.../100`，没有 `LV_OPA_15/25`
- box_shadow 绘制在背景**之前**，卡片等不透明背景会盖住样式级阴影
- SD 与 LCD 共享 SPI2 总线：SD 枚举/扫描必须与 LVGL 渲染同线程（持 `esp_lv_adapter_lock`）

## 其他

- 卡片图标/文字对齐在 `apps/launcher/launcher.c` relayout：图标 x=10、文字 x=58
- 调速器：胶囊外框 + 圆形滑块，霓虹粉 `WHEEL_COLOR 0xFF3E9E`；触摸热区在 wheel 容器，事件不绑滑块
- 卡片按下效果：边框加亮 + `transform_width/height=-5`（勿加 transform_scale）
- `APP_NET_UART_FWD_ENABLED`（uart/app_uart.h）控制 TCP 转发，当前=1
- 测试模块启用：CMake 取消 `apps/test/app_test.c` 注释 + main.c `#if 0`→`#if 1`
