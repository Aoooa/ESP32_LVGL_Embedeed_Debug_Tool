#ifndef FLOW_VIEW_H
#define FLOW_VIEW_H

/* flow_view —— 可滚动内容流视图组件（基于 LVGL）。
 *
 * 定位：在有限视口内显示"超出视口的内容流"，支持触摸滚动查看环形历史、
 * 跟随最新、滚动指示。业务无关：数据源通过 append/load_text 注入，
 * 不感知来源（串口/文件/网络/其他 UI 模块均可）。
 *
 * 适用：串口终端、日志滚动、阅读器（整段文本 + 触摸翻页）、
 * 列表/监控数据流等"行内容流"场景。
 *
 * 特性：
 *  - 多实例：全部状态在实例内，可同屏创建多个
 *  - 线程安全：append/load_text/clear 可在任意任务调用（内部锁保护）
 *  - 渲染统一在 LVGL 线程（内部刷新定时器 + 触摸回调），调用方无需大栈
 *  - 内存可插拔：通过 flow_view_malloc/flow_view_free 钩子指定（如 PSRAM）
 *  - 锁可插拔：通过 flow_view_set_lock 注入（如 esp_lv_adapter 的锁）
 *  - 行抽象预留 style 字段，后续扩展着色/图标等为纯加法
 */

#include "lvgl.h"
#include "flow_model.h"

/* ── 编译期配置（可用 -DFLOW_VIEW_* 覆盖） ── */
#ifndef FLOW_VIEW_MAX_LINES_DEF
#define FLOW_VIEW_MAX_LINES_DEF     256    /* 默认环形历史上限 */
#endif
#ifndef FLOW_VIEW_VISIBLE_LINES_DEF
#define FLOW_VIEW_VISIBLE_LINES_DEF 12     /* 默认可见行数（视口高 = 行数 × 行高） */
#endif
#ifndef FLOW_VIEW_LINE_HEIGHT_DEF
#define FLOW_VIEW_LINE_HEIGHT_DEF   14     /* 默认行高（与默认字体匹配） */
#endif
#ifndef FLOW_VIEW_REFRESH_MS_DEF
#define FLOW_VIEW_REFRESH_MS_DEF    10     /* 内部渲染刷新周期（ms） */
#endif
#ifndef FLOW_VIEW_DEF_WIDTH
#define FLOW_VIEW_DEF_WIDTH         320    /* 默认视口宽 */
#endif

/* ── 内存钩子（默认 malloc/free；可覆盖为 PSRAM 等专用分配） ── */
extern void *(*flow_view_malloc)(size_t size);
extern void (*flow_view_free)(void *ptr);

/* ── 锁钩子（可选注入：LVGL 渲染与外部的共享锁，如适配器锁） ── */
typedef struct {
    void (*lock)(void);
    void (*unlock)(void);
} flow_view_lock_t;
void flow_view_set_lock(const flow_view_lock_t *lk);

/* ── 创建/销毁（LVGL 对象，自动管理位图与历史缓冲） ── */
lv_obj_t *flow_view_create(lv_obj_t *parent);

/* ── 数据注入（任意线程可调用；渲染由内部定时器在 LVGL 线程完成） ── */
void flow_view_append(lv_obj_t *obj, const char *data, size_t len);   /* 流式 */
void flow_view_append_line(lv_obj_t *obj, const char *line, uint8_t style); /* 带样式整行 */
void flow_view_load_text(lv_obj_t *obj, const char *text);            /* 整段（阅读器） */
void flow_view_clear(lv_obj_t *obj);

/* ── 交互语义 ── */
void flow_view_set_follow(lv_obj_t *obj, bool follow);   /* 触摸暂停跟随；滑回底部自动恢复 */
bool flow_view_is_following(lv_obj_t *obj);

/* ── 配置 ── */
void flow_view_set_max_lines(lv_obj_t *obj, int max_lines);   /* 历史容量（重新分配缓冲） */

/* 追加文本（流式加载用）：内部按 UTF-8 逐字符解析折行，分段调用安全
 * （跨段行正确延续）。批量加载期间不触发重绘，完成后调用
 * flow_view_go_to / flow_view_request_redraw 统一刷新 */
void flow_view_append_text(lv_obj_t *obj, const char *text, size_t len);
int  flow_view_get_line_count(lv_obj_t *obj);
void flow_view_go_to(lv_obj_t *obj, int line);               /* 跳转行号（阅读器） */
void flow_view_set_font(lv_obj_t *obj, const lv_font_t *font);
void flow_view_set_color(lv_obj_t *obj, lv_color_t color);

/* 阅读进度：当前窗口起始行 / 允许的最大起始行（max_top<=0 表示无滚动） */
int flow_view_get_view_top(lv_obj_t *obj);
int flow_view_get_max_top(lv_obj_t *obj);

/* 动态调整可见行数（阅读器全屏/带栏切换）：重建位图与 canvas，内容保留重绘 */
void flow_view_set_visible_lines(lv_obj_t *obj, int visible_lines);

/* 点击回调（单击且无滑动时触发，pos 为屏幕坐标；滚动查看不触发）。 */
typedef void (*flow_view_clicked_cb_t)(void *user_data, lv_point_t pos);
void flow_view_set_clicked_cb(lv_obj_t *obj, flow_view_clicked_cb_t cb, void *user_data);

/* 外部行数据源（阅读器大文件模式）：行文本/行数由外部提供，
 * 内部 model 模式与 provider 模式二选一（set 后 provider 优先）。
 * count() 返回当前已知总行数；line() 返回第 row 行文本，
 * 返回 NULL 表示该行暂不可读（渲染为占位符）。均在渲染线程调用。 */
typedef struct {
    int (*count)(void *ctx);
    const char *(*line)(void *ctx, int row, uint8_t *style);
} flow_view_line_provider_t;
void flow_view_set_line_provider(lv_obj_t *obj, const flow_view_line_provider_t *provider,
                                 void *ctx);

/* 滚动位置回调（offset_px 变化时，硬件 0x37 滚动同步用） */
typedef void (*flow_view_scroll_cb_t)(void *user_data, int offset_px);
void flow_view_set_scroll_cb(lv_obj_t *obj, flow_view_scroll_cb_t cb, void *user_data);

#endif /* FLOW_VIEW_H */
