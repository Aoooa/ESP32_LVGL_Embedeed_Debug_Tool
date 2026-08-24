/* reader_view.c —— TXT 阅读界面（LVGL 全屏覆盖层，共享组件）。
 * 由 file_browser.c 的阅读覆盖层抽取而来，供浏览器/书架双入口共用。 */

#include "reader_view.h"
#include "flow_view.h"
#include "app_font.h"
#include "reader.h"
#include "gesture.h"
#include "speed_wheel.h"
#include "misc/lv_timer_private.h"
#include "core/lv_obj_private.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "reader_view";

/* 阅读界面配色（白底黑字，与深色浏览器/书架区分） */
#define RV_BG          lv_color_hex(0xFFFFFF)
#define RV_TEXT        lv_color_hex(0x111111)
#define RV_BTN_BORDER  lv_color_hex(0xD1D5DB)
#define RV_BTN_BAR_H   44     /* 底部容器高（透明，进度条悬浮） */
#define RV_BAR_BOTTOM  6
#define RV_TITLE_H     28
#define RV_LINES       17     /* 英文 12px 全屏行数；中文按行高动态重算 */

#define RV_PROG_TRACK  lv_color_hex(0x9CA3AF)
#define RV_PROG_INDIC  lv_color_hex(0x6B7280)
#define RV_PROG_KNOB   lv_color_hex(0x374151)

struct reader_view {
    lv_obj_t *root;            /* 全屏覆盖层 */
    lv_obj_t *title;
    lv_obj_t *title_label;
    lv_obj_t *bubble;
    lv_obj_t *prog;
    int prog_val;
    int prog_max;
    lv_obj_t *view;            /* flow_view：txt 内容 */
    lv_obj_t *bar;
    lv_timer_t *progress_timer;
    lv_obj_t *index_lbl;
    lv_obj_t *wheel;           /* 右侧调速器（系统组件 speed_wheel） */

    reader_view_back_cb_t back_cb;
    void *back_ctx;

    bool active;               /* 阅读中 */
    bool ui_hidden;            /* 纯阅读（栏隐藏） */
    reader_t *reader;          /* 数据层（NULL=未打开） */
    bool indexing;
    int line_w;                /* 打开时的折行像素宽 */
};

static int rv_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_horizontal_resolution(d) : 320;
}

static int rv_screen_h(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_vertical_resolution(d) : 240;
}

static lv_font_t *rv_ui_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* ── 进度条 ── */

static void rv_position_bubble(reader_view_t *rv)
{
    if (!rv->prog || !rv->bubble) return;
    int sw = lv_obj_get_width(rv->prog);
    int sh = lv_obj_get_height(rv->prog);
    int bw = lv_obj_get_width(rv->bubble);
    int sx = lv_obj_get_x(rv->prog);
    int max = rv->prog_max > 0 ? rv->prog_max : 1;
    int pct = rv->prog_val * 100 / max;
    if (pct > 100) pct = 100;
    int x = sx + pct * sw / 100 - bw / 2;
    if (x < sx) x = sx;
    if (x > sx + sw - bw) x = sx + sw - bw;
    lv_obj_align(rv->bubble, LV_ALIGN_TOP_LEFT, x, RV_BTN_BAR_H - sh - lv_obj_get_height(rv->bubble) - 4);
}

static void rv_prog_draw(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!rv) return;
    int w = lv_obj_get_width(obj);
    int h = lv_obj_get_height(obj);
    int r = h / 2;
    int track_w = w - h;

    lv_area_t a = obj->coords;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = r;

    dsc.bg_color = RV_PROG_TRACK;
    lv_draw_rect(layer, &dsc, &a);

    int max = rv->prog_max > 0 ? rv->prog_max : 1;
    int v = rv->prog_val;
    if (v < 0) v = 0;
    if (v > max) v = max;
    int ind_w = r + track_w * v / max;
    if (v > 0) {
        a.x2 = a.x1 + ind_w - 1;
        dsc.bg_color = RV_PROG_INDIC;
        lv_draw_rect(layer, &dsc, &a);
    }
    int cx = a.x1 + r + track_w * v / max;
    lv_area_t k = { cx - r, a.y1, cx + r - 1, a.y2 };
    dsc.bg_color = RV_PROG_KNOB;
    dsc.radius = r;
    lv_draw_rect(layer, &dsc, &k);
}

static void rv_prog_event(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    if (!rv || !rv->active) return;
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_PRESSED && c != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_t *obj = lv_event_get_target(e);
    int w = lv_obj_get_width(obj);
    int h = lv_obj_get_height(obj);
    int max = rv->prog_max > 0 ? rv->prog_max : 1;
    int v = (int)((long long)(p.x - obj->coords.x1 - h / 2) * max / (w - h));
    if (v < 0) v = 0;
    if (v > max) v = max;
    rv->prog_val = v;
    flow_view_go_to(rv->view, v);
    rv_position_bubble(rv);
    lv_obj_invalidate(obj);
}

static void rv_update_progress(reader_view_t *rv)
{
    int max = flow_view_get_max_top(rv->view);
    if (max <= 0) {
        lv_label_set_text(rv->bubble, "100%");
        rv->prog_max = 1;
        rv->prog_val = 0;
        if (rv->prog) lv_obj_invalidate(rv->prog);
        rv_position_bubble(rv);
        return;
    }
    int top = flow_view_get_view_top(rv->view);
    int pct = (int)((long long)top * 100 / max);
    if (pct > 100) pct = 100;
    lv_label_set_text_fmt(rv->bubble, "%d%%", pct);
    rv->prog_max = max;
    rv->prog_val = top;
    if (rv->prog) lv_obj_invalidate(rv->prog);
    rv_position_bubble(rv);
}

static void rv_progress_timer(lv_timer_t *t)
{
    reader_view_t *rv = t->user_data;
    if (!rv || !rv->active) return;

    if (rv->indexing) {
        lv_label_set_text_fmt(rv->index_lbl, "索引中 %d%%", reader_progress(rv->reader));
        if (!reader_is_indexing(rv->reader)) {
            rv->indexing = false;
            lv_obj_add_flag(rv->index_lbl, LV_OBJ_FLAG_HIDDEN);
            flow_view_go_to(rv->view, 0);
            rv_update_progress(rv);
        }
        return;
    }
    rv_update_progress(rv);
}

/* ── 栏显隐（点击中心切换） ── */

static void rv_chrome_ready_cb(lv_anim_t *a);
static void rv_chrome_anim(reader_view_t *rv, bool show)
{
    if (show) {
        lv_obj_clear_flag(rv->title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(rv->bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(rv->bubble, LV_OBJ_FLAG_HIDDEN);
        rv_position_bubble(rv);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_time(&a, 180);
    lv_anim_set_user_data(&a, rv);
    lv_anim_set_var(&a, rv->title);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, show ? -RV_TITLE_H : 0, show ? 0 : -RV_TITLE_H);
    lv_anim_start(&a);
    lv_anim_set_var(&a, rv->bar);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    int sh = rv_screen_h();
    lv_anim_set_values(&a, show ? sh : sh - RV_BTN_BAR_H - RV_BAR_BOTTOM,
                       show ? sh - RV_BTN_BAR_H - RV_BAR_BOTTOM : sh);
    if (!show) lv_anim_set_ready_cb(&a, rv_chrome_ready_cb);
    lv_anim_start(&a);
}

static void rv_chrome_ready_cb(lv_anim_t *a)
{
    reader_view_t *rv = a->user_data;
    if (rv) {
        lv_obj_add_flag(rv->title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rv->bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rv->bubble, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rv_tap_event(void *user_data, lv_point_t pos)
{
    reader_view_t *rv = user_data;
    if (!rv || !rv->active) return;
    if (pos.x < rv_screen_w() / 3 || pos.x > rv_screen_w() * 2 / 3 ||
        pos.y < rv_screen_h() / 3 || pos.y > rv_screen_h() * 2 / 3) {
        return;
    }
    /* 点击中间 → 退出/收起状态栏（若栏未显示则无操作） */
    if (!rv->ui_hidden) {
        rv->ui_hidden = true;
        rv_chrome_anim(rv, false);
    }
}

/* 上边缘下滑 → 显示状态栏（全局手势，系统层识别、阅读器订阅） */
static void rv_topdrop_cb(void *ctx)
{
    reader_view_t *rv = ctx;
    if (!rv || !rv->active) return;
    if (rv->ui_hidden) {
        rv->ui_hidden = false;
        rv_chrome_anim(rv, true);
    }
}

/* 右侧调速器 → 滚动 txt（speed_wheel 系统组件；pos -1..1，按位移滚动 flow_view） */
static void rv_speed_cb(void *ctx, float pos)
{
    reader_view_t *rv = ctx;
    if (!rv || !rv->active || rv->indexing) return;
    /* 上推(pos<0) → 内容上移（scroll 增大）；下拉 → 下移。粗略按 60px/满程 换算 */
    int dy = -(int)(pos * 60.0f);
    if (dy == 0) return;
    lv_obj_scroll_by_bounded(rv->view, 0, dy, LV_ANIM_OFF);
}

/* ── 打开/关闭 ── */

bool reader_view_open(reader_view_t *rv, const char *path)
{
    if (!rv) return false;
    if (rv->active) reader_view_close(rv);

    rv->active = true;
    rv->ui_hidden = false;

    lv_obj_clear_flag(rv->root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(rv->title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(rv->bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(rv->bubble, LV_OBJ_FLAG_HIDDEN);

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    lv_label_set_text(rv->title_label, base);

    lv_font_t *cn = app_font_get(16);
    lv_font_t *rf = cn ? cn : &lv_font_montserrat_12;
    flow_view_set_font(rv->view, rf);
    lv_obj_set_pos(rv->bar, 0, rv_screen_h() - RV_BTN_BAR_H - RV_BAR_BOTTOM);
    int lines = (rv_screen_h() + lv_font_get_line_height(rf) - 1) / lv_font_get_line_height(rf);
    flow_view_set_visible_lines(rv->view, lines);

    int lw = rv_screen_w() - 8;
    rv->reader = reader_open(path, lw, rf);
    if (!rv->reader) {
        ESP_LOGW(TAG, "open: reader_open failed for %s", path);
        reader_view_close(rv);
        return false;
    }
    rv->line_w = lw;
    rv->indexing = true;
    const flow_view_line_provider_t prov = { reader_count, reader_line };
    flow_view_set_line_provider(rv->view, &prov, rv->reader);

    lv_obj_clear_flag(rv->index_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(rv->index_lbl, "索引中 0%");
    lv_obj_update_layout(rv->root);
    return true;
}

void reader_view_close(reader_view_t *rv)
{
    if (!rv) return;
    rv->active = false;
    rv->indexing = false;
    if (rv->reader) {
        flow_view_set_line_provider(rv->view, NULL, NULL);
        reader_close(rv->reader);
        rv->reader = NULL;
    }
    lv_obj_add_flag(rv->root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(rv->title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(rv->bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(rv->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(rv->index_lbl, LV_OBJ_FLAG_HIDDEN);
}

bool reader_view_active(const reader_view_t *rv)
{
    return rv && rv->active;
}

bool reader_view_handle_back(reader_view_t *rv)
{
    if (!rv || !rv->active) return false;
    if (!rv->ui_hidden) {
        /* 状态栏显示中：右滑返回手势 → 先隐藏栏并拦截（不进入返回拖动） */
        rv->ui_hidden = true;
        rv_chrome_anim(rv, false);
        ESP_LOGI(TAG, "back-gesture: chrome shown -> hide, consumed");
        return true;
    }
    return false;   /* 栏已隐藏 → 放行，正常跟随右滑返回上一级 */
}

void reader_view_set_back_cb(reader_view_t *rv, reader_view_back_cb_t cb, void *ctx)
{
    if (!rv) return;
    rv->back_cb = cb;
    rv->back_ctx = ctx;
}

void reader_view_destroy(reader_view_t *rv)
{
    if (!rv) return;
    gesture_set_topdrop_handler(NULL, NULL);   /* 注销上边缘下滑订阅 */
    if (rv->reader) reader_view_close(rv);
    if (rv->progress_timer) lv_timer_delete(rv->progress_timer);
    if (rv->root) lv_obj_delete(rv->root);
    free(rv);
}

/* ── 创建 ── */

reader_view_t *reader_view_create(lv_obj_t *parent)
{
    reader_view_t *rv = calloc(1, sizeof(reader_view_t));
    if (!rv) return NULL;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, RV_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    rv->root = root;

    rv->view = flow_view_create(root);
    lv_obj_set_pos(rv->view, 0, 0);
    lv_obj_set_style_bg_color(rv->view, RV_BG, 0);
    flow_view_set_color(rv->view, RV_TEXT);
    flow_view_set_follow(rv->view, false);
    flow_view_set_visible_lines(rv->view, RV_LINES);
    flow_view_set_clicked_cb(rv->view, rv_tap_event, rv);

    /* 顶部状态栏：← 返回 + 文件名（set_pos 防布局重算与滑入动画冲突） */
    rv->title = lv_obj_create(root);
    lv_obj_set_size(rv->title, lv_pct(100), RV_TITLE_H);
    lv_obj_set_pos(rv->title, 0, 0);
    lv_obj_set_style_bg_color(rv->title, RV_BG, 0);
    lv_obj_set_style_bg_opa(rv->title, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rv->title, 1, 0);
    lv_obj_set_style_border_side(rv->title, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(rv->title, RV_BTN_BORDER, 0);
    lv_obj_set_style_radius(rv->title, 0, 0);
    lv_obj_set_style_pad_all(rv->title, 0, 0);
    lv_obj_set_flex_flow(rv->title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rv->title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rv->title, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(rv->title, LV_OBJ_FLAG_HIDDEN);

    rv->title_label = lv_label_create(rv->title);
    lv_obj_set_flex_grow(rv->title_label, 1);
    lv_obj_set_height(rv->title_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(rv->title_label, RV_TEXT, 0);
    lv_obj_set_style_text_font(rv->title_label, rv_ui_font(), 0);
    lv_obj_set_style_text_align(rv->title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(rv->title_label, "");

    /* 底部栏：透明悬浮，进度条 + 气泡（OVERFLOW_VISIBLE 防裁剪） */
    rv->bar = lv_obj_create(root);
    lv_obj_set_size(rv->bar, lv_pct(100), RV_BTN_BAR_H);
    lv_obj_set_pos(rv->bar, 0, rv_screen_h() - RV_BTN_BAR_H - RV_BAR_BOTTOM);
    lv_obj_add_flag(rv->bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(rv->bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rv->bar, 0, 0);
    lv_obj_set_style_radius(rv->bar, 0, 0);
    lv_obj_set_style_pad_all(rv->bar, 0, 0);
    lv_obj_set_style_pad_gap(rv->bar, 0, 0);
    lv_obj_set_flex_flow(rv->bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rv->bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(rv->bar, LV_OBJ_FLAG_HIDDEN);

    rv->bubble = lv_label_create(rv->bar);
    lv_obj_add_flag(rv->bubble, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_color(rv->bubble, lv_color_hex(0xFBBF24), 0);
    lv_obj_set_style_bg_opa(rv->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rv->bubble, 9, 0);
    lv_obj_set_style_text_color(rv->bubble, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(rv->bubble, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_hor(rv->bubble, 6, 0);
    lv_obj_set_style_pad_ver(rv->bubble, 1, 0);
    lv_obj_add_flag(rv->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(rv->bubble, "0%");

    rv->prog = lv_obj_create(rv->bar);
    lv_obj_set_size(rv->prog, lv_pct(88), 12);
    lv_obj_add_flag(rv->prog, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(rv->prog, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_opa(rv->prog, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(rv->prog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rv->prog, 0, 0);
    lv_obj_set_style_radius(rv->prog, 0, 0);
    lv_obj_set_style_pad_all(rv->prog, 0, 0);
    lv_obj_set_ext_click_area(rv->prog, 10);
    lv_obj_add_event_cb(rv->prog, rv_prog_draw, LV_EVENT_DRAW_MAIN, rv);
    lv_obj_add_event_cb(rv->prog, rv_prog_event, LV_EVENT_ALL, rv);

    rv->progress_timer = lv_timer_create(rv_progress_timer, 500, rv);

    rv->index_lbl = lv_label_create(root);
    lv_obj_center(rv->index_lbl);
    lv_obj_set_style_text_color(rv->index_lbl, RV_TEXT, 0);
    lv_obj_set_style_text_font(rv->index_lbl, rv_ui_font(), 0);
    lv_obj_add_flag(rv->index_lbl, LV_OBJ_FLAG_HIDDEN);

    /* 右侧调速器（系统组件）：悬浮右缘中间，上下拖动滚动 txt */
    rv->wheel = speed_wheel_create(root, 100, rv_speed_cb, rv);
    lv_obj_set_pos(rv->wheel, rv_screen_w() - 36 - 6, (rv_screen_h() - 100) / 2);

    /* 订阅上边缘下滑（显示状态栏）：全局手势，本阅读器响应 */
    gesture_set_topdrop_handler(rv_topdrop_cb, rv);

    return rv;
}
