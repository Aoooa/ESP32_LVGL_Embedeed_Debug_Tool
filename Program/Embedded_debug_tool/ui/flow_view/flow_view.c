/* flow_view.c —— 可滚动内容流视图组件（LVGL 9，class 扩展模式） */

#include "flow_view.h"
#include "core/lv_obj_private.h"
#include "core/lv_obj_class_private.h"
#include "misc/lv_timer_private.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

/* ── 内存钩子（可覆盖） ── */
void *(*flow_view_malloc)(size_t size) = malloc;
void (*flow_view_free)(void *ptr) = free;

/* ── 锁钩子（可选注入） ── */
static flow_view_lock_t s_lock;

void flow_view_set_lock(const flow_view_lock_t *lk)
{
    if (lk) {
        s_lock.lock = lk->lock;
        s_lock.unlock = lk->unlock;
    } else {
        s_lock.lock = NULL;
        s_lock.unlock = NULL;
    }
}

static void fv_lock(void)
{
    if (s_lock.lock) s_lock.lock();
}

static void fv_unlock(void)
{
    if (s_lock.unlock) s_lock.unlock();
}

/* ── 实例结构体（class 扩展，全部状态在此） ── */

typedef struct {
    lv_obj_t obj;                 /* 祖先（官方惯例） */
    flow_model_t model;           /* 内容流模型 */
    lv_obj_t *canvas;             /* 位图视口 */
    lv_obj_t *scrollbar;          /* 右侧滚动指示条 */
    uint8_t *bitmap;              /* RGB565 位图（create 时分配） */
    const lv_font_t *font;
    lv_color_t color;
    int line_h;                   /* 行高（默认宏，须与字体匹配） */
    int offset_px;                /* 像素级滚动偏移（0..max_px），平滑滚动 */
    int touch_y;                  /* 触摸跟踪 */
    int pending_dy;               /* 触摸累积位移（30ms 节流合并） */
    uint32_t last_scroll_ms;      /* 上次重绘时刻（节流） */
    int last_px;                  /* 上次渲染的像素偏移（增量重绘基准） */
    bool force_redraw;            /* 全量重绘（加载/跳转/行数变化） */
    flow_view_clicked_cb_t clicked_cb;
    void *clicked_user_data;
    flow_view_scroll_cb_t scroll_cb;      /* 滚动位置变化回调（硬件滚动同步用） */
    void *scroll_user_data;
    bool redraw_pending;          /* 外部 API 标记，由刷新定时器消费 */
    lv_timer_t *refresh_timer;
} flow_view_t;

#define MY_CLASS (&flow_view_class)

/* ── 字形宽度回调（模型层折行用） ── */

static int32_t fv_glyph_w(void *ctx, uint32_t code, uint32_t next)
{
    return (int32_t)lv_font_get_glyph_width((const lv_font_t *)ctx, code, next);
}

/* ── 内部工具 ── */

static int fv_bitmap_bytes(const flow_view_t *v)
{
    int w = lv_obj_get_width(v->canvas);
    int h = lv_obj_get_height(v->canvas);
    return w * h * 2;
}

/* 把一行文本绘制到位图的 y 行（必须在 LVGL 锁内） */
static void fv_draw_line(flow_view_t *v, const char *text, uint8_t style, int y)
{
    (void)style;   /* 样式表预留：后续按 style 选择前景色/图标 */

    if (text[0] == '\0') return;

    lv_layer_t layer;
    lv_canvas_init_layer(v->canvas, &layer);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = v->font;
    dsc.color = v->color;
    dsc.text = text;
    int w = lv_obj_get_width(v->canvas);
    lv_area_t coords = { 4, y, w - 4, y + v->line_h - 1 };
    lv_draw_label(&layer, &dsc, &coords);
    /* finish_layer 内部会全幅失效——抑制后由调用方统一失效 */
    lv_display_enable_invalidation(lv_obj_get_display(v->canvas), false);
    lv_canvas_finish_layer(v->canvas, &layer);
    lv_display_enable_invalidation(lv_obj_get_display(v->canvas), true);
}

static int fv_max_px(const flow_view_t *v)
{
    return flow_model_max_top(&v->model) * v->line_h;
}

/* 更新右侧滚动指示条（必须在 LVGL 锁内） */
static void fv_update_scrollbar(flow_view_t *v)
{
    int max_px = fv_max_px(v);
    if (max_px <= 0) {
        lv_obj_add_flag(v->scrollbar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(v->scrollbar, LV_OBJ_FLAG_HIDDEN);
    int vis_h = lv_obj_get_height(v->canvas);
    int thumb_h = vis_h * vis_h / (flow_model_line_count(&v->model) * v->line_h);
    if (thumb_h < 8) thumb_h = 8;
    if (thumb_h > vis_h) thumb_h = vis_h;
    int y = (vis_h - thumb_h) * v->offset_px / max_px;
    lv_obj_set_size(v->scrollbar, 4, thumb_h);
    lv_obj_set_pos(v->scrollbar, lv_obj_get_width(v->canvas) - 4, y);
}

/* 从模型窗口全量重绘位图（必须在 LVGL 锁内）。
 * 像素级平滑滚动：起始行 = offset_px/line_h，首行画在负偏移处 */
static void fv_full_redraw(flow_view_t *v)
{
    memset(v->bitmap, 0xFF, (size_t)fv_bitmap_bytes(v));

    int start = v->offset_px / v->line_h;
    int y0 = -(v->offset_px % v->line_h);
    int visible = v->model.visible_lines;
    for (int k = 0; k < visible + 1; k++) {
        int row = start + k;
        if (row >= flow_model_line_count(&v->model)) break;
        int y = y0 + k * v->line_h;
        if (y + v->line_h <= 0) continue;   /* 顶部移出的行跳过 */
        fv_draw_line(v, flow_model_line(&v->model, row),
                     flow_model_style(&v->model, row), y);
    }
    lv_obj_invalidate(v->canvas);
    fv_update_scrollbar(v);
}

/* 重绘与 [y0, y1] 区域相交的行（增量滚动后露出区） */
static void fv_draw_rows_in(flow_view_t *v, int y0, int y1)
{
    int start = v->offset_px / v->line_h;
    int first_y = -(v->offset_px % v->line_h);
    for (int k = 0; k < v->model.visible_lines + 1; k++) {
        int row = start + k;
        if (row >= flow_model_line_count(&v->model)) break;
        int y = first_y + k * v->line_h;
        if (y + v->line_h <= y0) continue;      /* 完全在区域上方 */
        if (y > y1) break;                      /* 完全在区域下方 */
        fv_draw_line(v, flow_model_line(&v->model, row),
                     flow_model_style(&v->model, row), y);
    }
}

/* 渲染入口（必须在 LVGL 锁内）：
 * 小位移滚动 → 位图 memmove 增量重绘（只画露出行）；
 * 注意：必须全幅 invalidate——显示 buffer 缓存了旧位图，
 * 局部 blit 无法更新整体 → 平移错位（撕裂）。
 * 大位移/内容变化 → 全量重绘 */
static void fv_redraw(flow_view_t *v)
{
    int vis_h = lv_obj_get_height(v->canvas);
    int stride = lv_obj_get_width(v->canvas) * 2;
    int shift = v->offset_px - v->last_px;
    v->last_px = v->offset_px;

    if (v->force_redraw || shift == 0 || shift > vis_h || shift < -vis_h) {
        v->force_redraw = false;
        fv_full_redraw(v);
        return;
    }

    int bytes = stride * vis_h;
    if (shift > 0) {   /* 内容上移：底部露出 */
        memmove(v->bitmap, v->bitmap + shift * stride, (size_t)(bytes - shift * stride));
        int y0 = vis_h - shift;
        memset(v->bitmap + y0 * stride, 0xFF, (size_t)(shift * stride));
        fv_draw_rows_in(v, y0, vis_h - 1);
    } else {           /* 内容下移：顶部露出 */
        memmove(v->bitmap + (-shift) * stride, v->bitmap, (size_t)(bytes + shift * stride));
        int y0 = -shift;
        memset(v->bitmap, 0xFF, (size_t)(y0 * stride));
        fv_draw_rows_in(v, 0, y0 - 1);
    }
    lv_obj_invalidate(v->canvas);   /* 全幅：防显示 buffer 缓存旧位图导致撕裂 */
    fv_update_scrollbar(v);
}

/* 像素偏移 → 同步模型行号（进度/滚动条换算用） */
static void fv_sync_row(flow_view_t *v)
{
    flow_model_set_view_top(&v->model, v->offset_px / v->line_h);
    if (v->scroll_cb) v->scroll_cb(v->scroll_user_data, v->offset_px);
}

/* ── 刷新定时器（LVGL 线程内运行，统一渲染） ── */

static void fv_timer_cb(lv_timer_t *timer)
{
    flow_view_t *v = timer->user_data;
    if (!v->model.lines) return;   /* 构造分配失败：组件不可用 */

    if (!v->redraw_pending) return;
    v->redraw_pending = false;

    if (flow_model_is_following(&v->model)) {
        v->offset_px = fv_max_px(v);
        fv_sync_row(v);
    }
    fv_redraw(v);
}

/* ── 触摸交互（canvas 事件，LVGL 线程内） ──
 * 拖动灵敏度：半行高 = 1 行（7px）。滑到最底部立即恢复跟随。 */

static void fv_canvas_event(lv_event_t *e)
{
    flow_view_t *v = lv_event_get_user_data(e);
    if (!v->model.lines) return;   /* 构造分配失败：组件不可用 */
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        v->touch_y = p.y;
    } else if (code == LV_EVENT_PRESSING) {
        int dy = p.y - v->touch_y;
        v->touch_y = p.y;
        int max_px = fv_max_px(v);
        v->offset_px -= dy;   /* 1:1 跟手：只更新偏移，O(1) 不阻塞 */
        if (v->offset_px < 0) v->offset_px = 0;
        if (v->offset_px > max_px) v->offset_px = max_px;
        flow_model_set_follow(&v->model, v->offset_px >= max_px);
        fv_sync_row(v);
        /* 重绘交给刷新定时器（LVGL 线程）执行——与渲染流水线融合，
         * flush(DMA) 期间可并行渲染下一帧，触摸事件本身零开销 */
        v->redraw_pending = true;
    } else if (code == LV_EVENT_RELEASED) {
        if (v->offset_px >= fv_max_px(v)) {
            flow_model_set_follow(&v->model, true);
            v->redraw_pending = true;
        v->force_redraw = true;
        }
    } else if (code == LV_EVENT_CLICKED) {
        /* 单击（无滑动）：触发点击回调（带屏幕坐标） */
        if (v->clicked_cb) v->clicked_cb(v->clicked_user_data, p);
    }
}

/* ── class 构造/析构 ── */

static void fv_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);
    flow_view_t *v = (flow_view_t *)obj;

    v->font = &lv_font_montserrat_12;
    v->color = lv_color_hex(0x1F2937);
    v->line_h = FLOW_VIEW_LINE_HEIGHT_DEF;
    v->offset_px = 0;
    v->last_px = 0;
    v->force_redraw = true;
    v->touch_y = 0;
    v->pending_dy = 0;
    v->last_scroll_ms = 0;
    v->redraw_pending = false;
    v->refresh_timer = NULL;

    /* 视口宽跟随 LVGL 逻辑分辨率（横竖屏通用，不再硬编码） */
    lv_display_t *disp = lv_display_get_default();
    int w = disp ? lv_display_get_horizontal_resolution(disp) : FLOW_VIEW_DEF_WIDTH;
    int h = FLOW_VIEW_VISIBLE_LINES_DEF * v->line_h;
    int max_lines = FLOW_VIEW_MAX_LINES_DEF;

    /* 分配位图 + 行缓冲（内存钩子，默认 malloc） */
    v->bitmap = flow_view_malloc((size_t)w * h * 2);
    char(*lines)[FLOW_VIEW_LINE_CHARS_DEF + 1] =
        flow_view_malloc((size_t)max_lines * (FLOW_VIEW_LINE_CHARS_DEF + 1));
    uint8_t *styles = flow_view_malloc((size_t)max_lines);
    if (!v->bitmap || !lines || !styles) {
        /* 分配失败：模型保持 lines=NULL，所有 API/定时器入口守卫，安全不可用 */
        flow_view_free(v->bitmap);
        flow_view_free(lines);
        flow_view_free(styles);
        v->bitmap = NULL;
        return;
    }
    memset(v->bitmap, 0xFF, (size_t)w * h * 2);   /* 初始白底 */

    flow_model_init(&v->model, lines, styles, max_lines,
                    FLOW_VIEW_VISIBLE_LINES_DEF, w - 8, fv_glyph_w, (void *)v->font);

    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);   /* 滚动由组件自身处理，禁止 LVGL 原生滚动容器 */

    v->canvas = lv_canvas_create(obj);
    lv_obj_set_size(v->canvas, w, h);
    lv_canvas_set_buffer(v->canvas, v->bitmap, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(v->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(v->canvas, LV_OBJ_FLAG_SCROLLABLE);
    /* LVGL 事件 filter 是单个事件码（不支持位或），用 LV_EVENT_ALL 接收后自判 */
    lv_obj_add_event_cb(v->canvas, fv_canvas_event, LV_EVENT_ALL, v);

    v->scrollbar = lv_obj_create(obj);
    lv_obj_set_size(v->scrollbar, 4, 8);
    lv_obj_set_style_bg_color(v->scrollbar, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_bg_opa(v->scrollbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v->scrollbar, 0, 0);
    lv_obj_set_style_radius(v->scrollbar, 2, 0);
    lv_obj_set_pos(v->scrollbar, w - 4, 0);
    lv_obj_add_flag(v->scrollbar, LV_OBJ_FLAG_HIDDEN);

    v->refresh_timer = lv_timer_create(fv_timer_cb, FLOW_VIEW_REFRESH_MS_DEF, v);
}

static void fv_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);
    flow_view_t *v = (flow_view_t *)obj;

    if (v->refresh_timer) lv_timer_delete(v->refresh_timer);
    flow_view_free(v->model.lines);
    flow_view_free(v->model.styles);
    flow_view_free(v->bitmap);
}

const lv_obj_class_t flow_view_class = {
    .base_class = &lv_obj_class,
    .constructor_cb = fv_constructor,
    .destructor_cb = fv_destructor,
    .instance_size = sizeof(flow_view_t),
    .name = "flow_view",
};

lv_obj_t *flow_view_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

/* ── 公共 API（任意线程可调用；渲染统一由刷新定时器在 LVGL 线程完成） ── */

void flow_view_append(lv_obj_t *obj, const char *data, size_t len)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines) {
        flow_model_append(&v->model, data, len);
        v->redraw_pending = true;
        v->force_redraw = true;
    }
    fv_unlock();
}

void flow_view_append_line(lv_obj_t *obj, const char *line, uint8_t style)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines) {
        flow_model_append_line(&v->model, line, style);
        v->redraw_pending = true;
        v->force_redraw = true;
    }
    fv_unlock();
}

void flow_view_load_text(lv_obj_t *obj, const char *text)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines) {
        flow_model_load_text(&v->model, text);
        v->offset_px = 0;
        v->redraw_pending = true;
        v->force_redraw = true;
    }
    fv_unlock();
}

void flow_view_clear(lv_obj_t *obj)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines) {
        flow_model_clear(&v->model);
        v->offset_px = 0;
        v->redraw_pending = true;
        v->force_redraw = true;
    }
    fv_unlock();
}

void flow_view_set_follow(lv_obj_t *obj, bool follow)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines) {
        flow_model_set_follow(&v->model, follow);
        if (follow) {
            v->redraw_pending = true;
            v->force_redraw = true;
        }
    }
    fv_unlock();
}

bool flow_view_is_following(lv_obj_t *obj)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    bool r = v->model.lines ? flow_model_is_following(&v->model) : true;
    fv_unlock();
    return r;
}

void flow_view_set_max_lines(lv_obj_t *obj, int max_lines)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (!v->model.lines) {
        fv_unlock();
        return;
    }
    if (max_lines < 1) max_lines = 1;

    char(*lines)[FLOW_VIEW_LINE_CHARS_DEF + 1] =
        flow_view_malloc((size_t)max_lines * (FLOW_VIEW_LINE_CHARS_DEF + 1));
    uint8_t *styles = flow_view_malloc((size_t)max_lines);
    if (lines && styles) {
        flow_view_free(v->model.lines);
        flow_view_free(v->model.styles);
        flow_model_init(&v->model, lines, styles, max_lines,
                        v->model.visible_lines,
                        lv_obj_get_width(v->canvas) - 8, fv_glyph_w, (void *)v->font);
        v->offset_px = 0;
        v->redraw_pending = true;
        v->force_redraw = true;
    } else {
        flow_view_free(lines);
        flow_view_free(styles);
    }
    fv_unlock();
}

int flow_view_get_line_count(lv_obj_t *obj)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    int n = v->model.lines ? flow_model_line_count(&v->model) : 0;
    fv_unlock();
    return n;
}

void flow_view_go_to(lv_obj_t *obj, int line)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines) {
        flow_model_set_follow(&v->model, false);   /* 主动跳转：退出跟随，否则滑到底后会被拉回 */
        flow_model_set_view_top(&v->model, line);
        v->offset_px = flow_model_view_top(&v->model) * v->line_h;
        v->redraw_pending = true;
        v->force_redraw = true;
    }
    fv_unlock();
}

void flow_view_set_font(lv_obj_t *obj, const lv_font_t *font)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines && font) {
        v->font = font;
        v->line_h = lv_font_get_line_height(font);   /* 行高随字体（中文行高更大） */
        v->model.glyph_ctx = (void *)font;
        v->offset_px = flow_model_view_top(&v->model) * v->line_h;   /* 按行号保持阅读位置 */
        v->redraw_pending = true;
        v->force_redraw = true;
    }
    fv_unlock();
}

void flow_view_set_color(lv_obj_t *obj, lv_color_t color)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (v->model.lines) {
        v->color = color;
        v->redraw_pending = true;
        v->force_redraw = true;
    }
    fv_unlock();
}

int flow_view_get_view_top(lv_obj_t *obj)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    int top = v->model.lines ? flow_model_view_top(&v->model) : 0;
    fv_unlock();
    return top;
}

int flow_view_get_max_top(lv_obj_t *obj)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    int max = v->model.lines ? flow_model_max_top(&v->model) : 0;
    fv_unlock();
    return max;
}

void flow_view_set_visible_lines(lv_obj_t *obj, int visible_lines)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    if (!v->model.lines || visible_lines < 1) {
        fv_unlock();
        return;
    }
    int w = lv_obj_get_width(lv_obj_get_parent(obj));   /* 视口宽 = 父（阅读区）宽，横竖屏自适应 */
    int h = visible_lines * v->line_h;
    uint8_t *buf = flow_view_malloc((size_t)w * h * 2);
    if (!buf) {
        fv_unlock();
        return;
    }
    memset(buf, 0xFF, (size_t)w * h * 2);
    flow_view_free(v->bitmap);
    v->bitmap = buf;
    v->model.visible_lines = visible_lines;
    v->offset_px = 0;
    lv_obj_set_size(v->canvas, w, h);
    lv_canvas_set_buffer(v->canvas, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(obj, w, h);   /* 对象尺寸与位图一致，消除底部空余 */
    v->redraw_pending = true;
        v->force_redraw = true;
    fv_unlock();
}

void flow_view_set_clicked_cb(lv_obj_t *obj, flow_view_clicked_cb_t cb, void *user_data)
{
    flow_view_t *v = (flow_view_t *)obj;
    fv_lock();
    v->clicked_cb = cb;
    v->clicked_user_data = user_data;
    fv_unlock();
}


void flow_view_set_scroll_cb(lv_obj_t *obj, flow_view_scroll_cb_t cb, void *user_data)
{
    flow_view_t *v = (flow_view_t *)obj;
    v->scroll_cb = cb;
    v->scroll_user_data = user_data;
}