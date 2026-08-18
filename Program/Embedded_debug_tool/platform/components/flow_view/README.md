# flow_view —— 可滚动内容流视图组件

基于 LVGL 的通用内容流显示组件：在有限视口内显示"超出视口的内容流"，
支持触摸滚动查看环形历史、跟随最新、滚动指示。业务无关，可复用于
串口终端、日志滚动、阅读器、监控数据流、列表等场景。

## 特性

- **业务无关**：数据源通过 `flow_view_append` / `flow_view_load_text` 注入，
  组件不感知来源（串口 / 文件 / 网络 / 其他 UI 模块均可）
- **多实例**：全部状态在实例内，可同屏创建多个（零全局状态）
- **线程安全**：`append` / `load_text` / `clear` 可在任意任务调用（内部锁保护）；
  渲染统一在 LVGL 线程（内部刷新定时器 + 触摸回调），调用方无需大栈
- **内存可插拔**：`flow_view_malloc` / `flow_view_free` 钩子（默认 malloc，
  可覆盖为 PSRAM 等专用分配）
- **锁可插拔**：`flow_view_set_lock` 注入与 LVGL 渲染共享的锁
- **行抽象预留**：`flow_model_append_line(style)` 已支持行样式字段，
  后续扩展着色 / 图标 / 高亮为纯加法
- **模型/视图分离**：`flow_model` 为纯 C 数据模型（零 LVGL 依赖），可独立单测
  或供其他渲染器复用

## 快速开始

```c
#include "flow_view.h"

/* ① 创建（任意 parent） */
lv_obj_t *view = flow_view_create(scr);

/* ② 配置（可选，均有默认值） */
flow_view_set_max_lines(view, 256);     /* 环形历史上限 */
flow_view_set_follow(view, true);       /* 跟随底部（默认 true） */

/* ③ 数据注入（任意线程/时机） */
/* 流式（终端/日志/数据流）： */
flow_view_append(view, rx_buf, rx_len);

/* 整段（阅读器）： */
flow_view_set_follow(view, false);
flow_view_load_text(view, book_text);
flow_view_go_to(view, 1000);            /* 跳转行号 */

/* 带样式整行（预留着色）： */
flow_view_append_line(view, "ERROR: fail", 1);

/* ④ 清空 */
flow_view_clear(view);
```

## 配置

### 编译期（`-DFLOW_VIEW_*` 覆盖，默认值见头文件）

| 宏 | 默认 | 说明 |
|---|---|---|
| `FLOW_VIEW_MAX_LINES_DEF` | 256 | 环形历史上限 |
| `FLOW_VIEW_LINE_CHARS_DEF` | 60 | 单行字符上限（超出折行/截断） |
| `FLOW_VIEW_VISIBLE_LINES_DEF` | 12 | 可见行数（视口高 = 行数 × 行高） |
| `FLOW_VIEW_LINE_HEIGHT_DEF` | 14 | 默认行高 |
| `FLOW_VIEW_REFRESH_MS_DEF` | 10 | 内部渲染刷新周期（ms） |
| `FLOW_VIEW_DEF_WIDTH` | 320 | 默认视口宽 |

### 运行期（API）

| API | 说明 |
|---|---|
| `flow_view_set_max_lines` | 历史容量（自动重新分配缓冲） |
| `flow_view_set_follow` | 跟随/暂停跟随 |
| `flow_view_go_to` | 跳转行号 |
| `flow_view_set_font` / `set_color` | 字体/文本色 |

### 内存与锁钩子（宿主接入）

```c
/* 内存：位图与行历史（默认 malloc） */
flow_view_malloc = my_psram_alloc;      /* 如 heap_caps_aligned_alloc(64, size, SPIRAM) */
flow_view_free   = heap_caps_free;

/* 锁：与 LVGL 渲染共享的递归锁（必须可重入） */
static const flow_view_lock_t lk = { .lock = lock_fn, .unlock = unlock_fn };
flow_view_set_lock(&lk);
```

## 交互语义

- **无触摸**：自动跟随最新（数据到达即滚动显示末尾）
- **触摸拖动**：暂停跟随，查看历史；数据继续进入历史，右侧滚动指示条实时更新
- **滑回底部**：自动恢复跟随（拖动中到达底部即恢复）
- 触摸灵敏度：半行高 = 1 行（7px）

## 线程与栈要求

- `flow_view_create` / 触摸 / 渲染：在 LVGL 线程（或持有锁的上下文）
- `append` / `load_text` / `clear` / 配置：任意线程可调用（组件内部加锁，
  只做模型更新；渲染由内部 10ms 定时器在 LVGL 线程完成）
- 渲染（文本绘制）在 LVGL 线程执行，栈需求由 LVGL 任务栈承担

## 目录结构

```
ui/flow_view/
├── flow_view.h       公共 API + 编译期配置
├── flow_view.c       LVGL widget 实现（canvas 位图 / 重绘 / 触摸 / 滚动条）
├── flow_model.h      模型层接口（纯 C）
└── flow_model.c      模型层实现（环形行历史 / 字符解析 / 折行 / 窗口计算）
```

## 文件

- `flow_view.c`：视图层，LVGL 9 class 扩展模式（`lv_obj_private.h` /
  `lv_obj_class_private.h` / `lv_timer_private.h`）
- `flow_model.c`：模型层，零依赖；折行所需的字形宽度经 `flow_glyph_w_cb_t`
  回调注入（视图层默认注入 LVGL 字体的 `lv_font_get_glyph_width`）

## 已知限制

- `append_line` 整行超长直接截断（不折行）
- 换字体需同时匹配行高；`set_font` 不改变视口布局
- 样式字段当前仅存储（style=0 默认色渲染），着色映射为后续扩展
