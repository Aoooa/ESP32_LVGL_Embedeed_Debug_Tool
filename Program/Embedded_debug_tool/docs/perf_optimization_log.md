# 性能优化验证记录（ESP32-S3 + ST7789 240x320 滚动阅读器）

环境：ESP32-S3 240MHz / LVGL 9.4.0 / esp_lvgl_adapter 0.6.2 / SPI 80MHz / OCT PSRAM 80M
测量：LVGL sysmon（PERF_MONITOR_LOG_MODE，每 300ms 一档）
场景：打开 test1.txt 持续上下滑动（flow_view 软件滚动）

---

## Step 0: 基线（开启 FPS 监控）

- 日期：2026-08-13
- 改动：sdkconfig 开 `LV_USE_SYSMON=y` + `LV_USE_PERF_MONITOR=y` + `LV_USE_PERF_MONITOR_LOG_MODE=y` + `LV_USE_LOG=y` + `LV_LOG_LEVEL=INFO` + `LV_LOG_PRINTF=y`；app_display.c 调 `lv_sysmon_show_performance()` + heap 日志
- **滚动 FPS：27-28**（稳定，档位 26-30）
- **单帧成本：23ms**（refr avg）＝ render 14-17ms + flush 6-8ms
- CPU：100%（双核持续满载）
- heap：free=4808KB / **internal=23KB** / psram=4791KB
- 空闲假象：refr_cnt 30/300ms=100 FPS（tick 计数），只看 redraw_cnt>0 的档
- 结论：
  - **渲染是主瓶颈（~65%）**，flush 次之（~35%）
  - 内部 SRAM 仅余 23KB → buffer_height 提升必须先降 LV_MEM_SIZE
  - PSRAM 充足（4791KB 空闲）

---

## Step 1a: SPI max_transfer_sz 16行→40行整块

- 日期：2026-08-13
- 改动：drv_display.c `max_transfer_sz` 7680B(16行) → 25600B(320×40×2, 40行整块一次传)
- **滚动 FPS：27-28**（无变化）
- 单帧：23ms（render 15ms + flush 6-7ms）——flush 与基线持平
- 结论：**无收益**。flush 瓶颈不在分块次数（SPI 数据量+排队本身）。改动保留（ISR 次数略减，无副作用）
- 数据佐证：非连续滚动档位 FPS 50-96（短暂停顿帧成本低），连续滚动稳定 27-28

---

## Step 1b: SPI trans_queue_depth 10→4

- 日期：2026-08-13
- 改动：drv_display.c `trans_queue_depth` 10 → 4
- **滚动 FPS：29-32**（↑ 基线 27-28，+2-4 FPS）
- 单帧：20ms（render 11-13ms + flush 6-8ms）——refr 23→20ms，render 15→12ms
- 结论：**收益 +10%**。队列深度减小使 flush 与渲染重叠更顺畅（LVGL 双缓冲流水线），改队列深度而非吞吐
- 保持：trans_queue_depth=4

---

## Step 1c: flash DIO 80M → QIO（120M 尝试失败）

- 日期：2026-08-13
- 尝试 120M：编译失败——ESP32-S3 flash 120MHz 与 PSRAM 80M SDR 不兼容（需 PSRAM 120M DTR 实验特性，温漂风险）→ **放弃 120M**
- 最终改动：`FLASHMODE QIO=y` + 80MHz（DIO→QIO 保留，无冲突）
- **滚动 FPS：29-33**（与 1b 持平，略好于基线的噪声范围内）
- 结论：QIO 80M 无明显收益（XIP 带宽非当前瓶颈），保留（无副作用）
- 保持：QIO 80M

---

## Step 2: buffer_height 40→80 行（LV_MEM 512→256 释放 SRAM）

- 日期：2026-08-13
- 改动：app_display.c `buffer_height` 40→80（102.4KB SRAM 双缓冲）；sdkconfig `LV_MEM_SIZE_KILOBYTES` 512→256
- **滚动 FPS：29-33**（与 1b/1c 持平，无提升）
- 单帧：20ms（render 11-13ms + flush 6-8ms）——render 未降
- 发现：internal free 恒 23KB（LV_MEM 池实际在 PSRAM，512→256 不释放 SRAM）；80 行 buffer 分配成功（SRAM 够）
- 结论：**无收益**——flow_view 每帧全屏重绘（invalidate 全幅），tile 数减半但渲染总量不变 → buffer 大小不影响。**优化方向应减重绘面积（Step 4）**
- **回退**：buffer_height 回 40（释放 51.2KB SRAM 无性能损失）；LV_MEM 回 512（池在 PSRAM，无影响，保持原样）

---

## Step 4a: LV_DRAW_SW_DRAW_UNIT_CNT 2→1 对比

- 日期：2026-08-13
- 改动：sdkconfig `LV_DRAW_SW_DRAW_UNIT_CNT` 2 → 1
- **滚动 FPS：29**（与双 unit 持平，无净收益）
- 单帧：20ms——单 unit：render 7-8ms + flush 10-11ms；双 unit：render 11-13ms + flush 6-8ms
- 分析：双 unit 的线程同步开销抬高 render，但单 unit 时 DMA 与渲染并行度下降抬高 flush——两者互换抵消
- 结论：**无净收益**。回退双 unit=2（官方推荐、双核利用率、复杂场景潜力）
- 保持：DRAW_UNIT_CNT=2

---

## Step 3a: LVGL 任务绑核 CPU1

- 日期：2026-08-13
- 改动：app_display.c `adapter_cfg.task_core_id = 1`（LVGL 主循环 CPU1，WiFi 默认 CPU0）
- **滚动 FPS：29-32**（持平，无提升）
- 单帧：20ms（render 11-12ms + flush 7-8ms）
- 结论：**无收益**——双核已饱和，refr 与 draw 竞争非瓶颈，render 11ms 是纯渲染工作
- 回退：task_core_id 还原 -1（默认）

---

## 结论（截至 Step 3a，全部低成本项完成）

- 净提升：基线 27-28 FPS → **29-32 FPS**（+10%，唯一有效项 Step 1b queue_depth 10→4）
- 已验证无效/持平：max_transfer_sz、flash QIO/120M、buffer 80 行、draw unit 1vs2、LVGL 绑核
- 瓶颈模型：滚动时全屏内容变化 → 每帧 8 tile 渲染（~11ms）+ 全屏传输（~7ms）= 20ms/帧
  → 30FPS 为该架构稳定极限（SPI 全屏刷新理论 ~50FPS，扣除渲染+调度后 30）
- 剩余突破方向（未执行，待用户决策）：
  1. 0x37 硬件滚动 V2（滚动只刷新行条带，理论 50+ FPS；此前实现失败：flush 映射撕裂+进度条反色）
  2. 阅读器 canvas 排除标题栏（省 ~9% 渲染/传输面积，低收益）
  3. 接受当前 30FPS

