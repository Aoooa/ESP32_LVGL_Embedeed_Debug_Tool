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
int  flow_view_get_line_count(lv_obj_t *obj);
void flow_view_go_to(lv_obj_t *obj, int line);               /* 跳转行号（阅读器） */
void flow_view_set_font(lv_obj_t *obj, const lv_font_t *font);
void flow_view_set_color(lv_obj_t *obj, lv_color_t color);

#endif /* FLOW_VIEW_H */
