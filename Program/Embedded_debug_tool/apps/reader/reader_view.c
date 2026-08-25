/* reader_view.c —— TXT 阅读界面（LVGL 全屏覆盖层，共享组件）。
 * 由 file_browser.c 的阅读覆盖层抽取而来，供浏览器/书架双入口共用。 */

#include "reader_view.h"
#include "flow_view.h"
#include "app_font.h"
#include "reader.h"
#include "reader_favcache.h"
#include "gesture.h"
#include "speed_wheel.h"
#include "num_input.h"
#include "esp_lv_adapter.h"
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
#define RV_BAR_H       84     /* 底部设置栏总高（上部进度条区 + 下部白色按钮区，整体滑入/滑出） */
#define RV_TITLE_H     28
#define RV_LINES       17     /* 英文 12px 全屏行数；中文按行高动态重算 */

#define RV_PROG_TRACK  lv_color_hex(0x9CA3AF)
#define RV_PROG_INDIC  lv_color_hex(0x6B7280)
#define RV_PROG_KNOB   lv_color_hex(0x374151)

/* 底部设置栏内部布局（进度条在上、白色按钮区在下；栏体贴屏幕底，
 * 白色按钮区上下留白等距 → 按钮垂直居中） */
#define RV_PROG_TOP    26     /* 进度条距栏顶；上方留出空间给百分比气泡（收进栏内） */
#define RV_BTN_PANEL_H 40     /* 白色按钮区高（贴栏底 = 屏幕底） */
#define RV_BTN_W       46
#define RV_BTN_H       32
#define RV_BTN_GAP     8
#define RV_SPACING_MAX 16     /* 行距调节范围（0..16px，步进 2，纯 UI） */
#define RV_SPACING_STEP 2
#define RV_AUTO_TICK   400    /* 自动滚动周期 ms（慢速，2.5 行/秒） */
#define RV_AUTO_GREEN  0x16A34A
#define RV_AUTO_RED    0xDC2626
#define RV_FAV_GRAY    lv_color_hex(0x9CA3AF)   /* 未收藏 ☆ 灰 */
#define RV_FAV_COLOR   lv_color_hex(0xFBBF24)   /* 已收藏 ★ 黄 */
#define RV_TOAST_MS    1600

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

    /* 收藏 */
    lv_obj_t *star_btn;        /* ⭐ 收藏当前行 */
    lv_obj_t *star_lbl;
    lv_obj_t *list_btn;        /* 📋 收藏列表 */
    char path[160];            /* 当前 txt 路径 */
    reader_fav_list_t fav;     /* 收藏列表（内存） */

    /* 底部扩展区（搜索跳行 / 行距 / 自动滚动） */
    lv_obj_t *go_btn;
    lv_obj_t *spacing_m_btn;
    lv_obj_t *spacing_p_btn;
    lv_obj_t *auto_btn;
    lv_obj_t *auto_lbl;
    int line_spacing;          /* 当前行距（0..RV_SPACING_MAX） */
    bool auto_active;
    lv_timer_t *auto_timer;

    /* 收藏列表弹窗（NULL=未开） */
    lv_obj_t *fav_dlg;
    lv_obj_t *fav_rows;

    /* 提示（居中短时消息） */
    lv_obj_t *toast;
    lv_timer_t *toast_timer;

    reader_view_back_cb_t back_cb;
    void *back_ctx;

    bool active;               /* 阅读中 */
    bool ui_hidden;            /* 纯阅读（栏隐藏） */
    reader_t *reader;          /* 数据层（NULL=未打开） */
    bool indexing;
    int line_w;                /* 打开时的折行像素宽 */
    float speed_acc;           /* 调速器速度累积器（行数，取整翻页） */
};

/* 前向声明（互引：进度条/调速器手动操作停自动滚动；收藏切换刷新弹窗） */
static void rv_auto_stop(reader_view_t *rv);
static void rv_fav_dlg_refresh(reader_view_t *rv);
static void rv_refresh_star(reader_view_t *rv);

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
    int bw = lv_obj_get_width(rv->bubble);
    int sx = lv_obj_get_x(rv->prog);
    int max = rv->prog_max > 0 ? rv->prog_max : 1;
    int pct = rv->prog_val * 100 / max;
    if (pct > 100) pct = 100;
    int x = sx + pct * sw / 100 - bw / 2;
    if (x < sx) x = sx;
    if (x > sx + sw - bw) x = sx + sw - bw;
    /* 气泡在进度条上方（进度条位于栏上部 → 气泡可能越出栏顶，OVERFLOW_VISIBLE 渲染） */
    int py = lv_obj_get_y(rv->prog);
    lv_obj_align(rv->bubble, LV_ALIGN_TOP_LEFT, x, py - lv_obj_get_height(rv->bubble) - 4);
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
    if (c == LV_EVENT_PRESSED) rv_auto_stop(rv);   /* 拖动进度条手动定位 → 停自动滚动 */
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
            lv_obj_clear_flag(rv->wheel, LV_OBJ_FLAG_HIDDEN);   /* 索引完成显示调速器 */
            flow_view_go_to(rv->view, 0);
            rv_update_progress(rv);
            rv_refresh_star(rv);
        }
        return;
    }
    rv_update_progress(rv);
    rv_refresh_star(rv);   /* 滚动时星标跟随当前行 */
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
    lv_anim_set_values(&a, show ? sh : sh - RV_BAR_H,
                       show ? sh - RV_BAR_H : sh);
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

/* 右侧调速器 → 滚动 txt：速度 = pos³（符号保持，近中心极慢、越远越快）。
 * 累积器把连续回调的小步进聚成整数行才翻页，避免快速滑动一次跳太多。 */
static void rv_speed_cb(void *ctx, float pos)
{
    reader_view_t *rv = ctx;
    if (!rv || !rv->active || rv->indexing) return;
    if (pos != 0.0f) rv_auto_stop(rv);   /* 手动滚动 → 停自动滚动 */
    /* 低端线性 + 高端次方：起步即有可见响应（不虚位）但慢，拉远平滑加速。
     * f = 0.35·pos + 0.65·pos³：pos=0.2→0.075, 0.3→0.122, 0.6→0.35, 1→1 */
    float f = pos * (0.35f + 0.65f * pos * pos);
    rv->speed_acc += f * 0.35f;               /* 基线调低，整体慢一拍 */
    int delta = (int)rv->speed_acc;
    if (delta == 0) return;
    rv->speed_acc -= (float)delta;
    int top = flow_view_get_view_top(rv->view);
    int max = flow_view_get_max_top(rv->view);
    int nt = top + delta;                     /* 下拉(pos>0)向后翻；上推向前 */
    if (nt < 0) nt = 0;
    if (nt > max) nt = max;
    flow_view_go_to(rv->view, nt);
}

/* ── 提示（居中短时消息，脚本化显示后自动隐藏） ── */

static void rv_toast_hide(lv_timer_t *t)
{
    reader_view_t *rv = t->user_data;
    if (rv && rv->toast) lv_obj_add_flag(rv->toast, LV_OBJ_FLAG_HIDDEN);
    rv->toast_timer = NULL;   /* 一次性定时器：本次触发后由 LVGL 自动删除 */
}

static void rv_toast(reader_view_t *rv, const char *msg)
{
    if (!rv || !rv->toast) return;
    lv_label_set_text(rv->toast, msg);
    lv_obj_clear_flag(rv->toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(rv->toast);
    if (rv->toast_timer) lv_timer_delete(rv->toast_timer);
    rv->toast_timer = lv_timer_create(rv_toast_hide, RV_TOAST_MS, rv);
    lv_timer_set_repeat_count(rv->toast_timer, 1);
}

/* ── 收藏：星标（当前行） + 列表弹窗 ── */

static int rv_fav_find(reader_view_t *rv, int line)
{
    for (int i = 0; i < rv->fav.count; i++) {
        if (rv->fav.items[i].line == line) return i;
    }
    return -1;
}

/* 星标状态跟随当前行（打开/滚动/收藏变更后刷新） */
static void rv_refresh_star(reader_view_t *rv)
{
    if (!rv->star_lbl) return;
    int line = flow_view_get_view_top(rv->view);
    bool faved = rv_fav_find(rv, line) >= 0;
    lv_label_set_text(rv->star_lbl, faved ? "★" : "☆");
    lv_obj_set_style_text_color(rv->star_lbl, faved ? RV_FAV_COLOR : RV_FAV_GRAY, 0);
}

static void rv_fav_save(reader_view_t *rv)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        reader_fav_save(rv->path, &rv->fav);
        esp_lv_adapter_unlock();
    }
}

/* 收藏列表弹窗操作 */

static void rv_fav_dlg_close(reader_view_t *rv)
{
    if (!rv || !rv->fav_dlg) return;
    lv_obj_delete(rv->fav_dlg);
    rv->fav_dlg = NULL;
    rv->fav_rows = NULL;
}

static void rv_fav_dlg_delete(reader_view_t *rv, int idx)
{
    if (idx < 0 || idx >= rv->fav.count) return;
    memmove(&rv->fav.items[idx], &rv->fav.items[idx + 1],
            (size_t)(rv->fav.count - idx - 1) * sizeof(rv->fav.items[0]));
    rv->fav.count--;
    rv_fav_save(rv);
    rv_refresh_star(rv);
    rv_fav_dlg_refresh(rv);
}

static void rv_fav_del_evt(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    rv_fav_dlg_delete(rv, idx);
}

/* 点击收藏项：关闭弹窗并跳到对应行 */
static void rv_fav_row_evt(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!rv || idx < 0 || idx >= rv->fav.count) return;
    int line = rv->fav.items[idx].line;
    rv_fav_dlg_close(rv);
    rv_auto_stop(rv);                       /* 手动跳转 → 停自动滚动 */
    if (line < 0) line = 0;
    int max = flow_view_get_max_top(rv->view);
    if (line > max) line = max;
    flow_view_go_to(rv->view, line);
    rv_update_progress(rv);
    rv_refresh_star(rv);
}

static void rv_fav_dlg_dismiss_evt(lv_event_t *e)
{
    rv_fav_dlg_close(lv_event_get_user_data(e));
}

/* 重建弹窗行列表（打开/删除/收藏变更后） */
static void rv_fav_dlg_refresh(reader_view_t *rv)
{
    if (!rv->fav_dlg || !rv->fav_rows) return;
    lv_obj_clean(rv->fav_rows);
    if (rv->fav.count == 0) {
        lv_obj_t *l = lv_label_create(rv->fav_rows);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, RV_FAV_GRAY, 0);
        lv_obj_set_style_text_font(l, rv_ui_font(), 0);
        lv_label_set_text(l, "(none)");
        return;
    }
    for (int i = 0; i < rv->fav.count; i++) {
        lv_obj_t *row = lv_obj_create(rv->fav_rows);
        lv_obj_set_size(row, lv_pct(100), 32);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xF9FAFB), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xEEF2FF), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, lv_color_hex(0xE5E7EB), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, rv_fav_row_evt, LV_EVENT_CLICKED, rv);

        lv_obj_t *ln = lv_label_create(row);
        lv_obj_set_width(ln, 52);
        lv_obj_set_style_text_color(ln, lv_color_hex(0x6B7280), 0);
        lv_obj_set_style_text_font(ln, rv_ui_font(), 0);
        lv_obj_set_style_pad_left(ln, 6, 0);
        lv_label_set_text_fmt(ln, "L%d", rv->fav.items[i].line + 1);   /* 显示 1 基 */

        lv_obj_t *ct = lv_label_create(row);
        lv_obj_set_flex_grow(ct, 1);
        lv_obj_set_style_text_color(ct, RV_TEXT, 0);
        lv_obj_set_style_text_font(ct, rv_ui_font(), 0);
        lv_label_set_long_mode(ct, LV_LABEL_LONG_DOT);
        lv_label_set_text(ct, rv->fav.items[i].content);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 36, 28);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_color(del, RV_BTN_BORDER, 0);
        lv_obj_set_style_border_width(del, 1, 0);
        lv_obj_set_style_radius(del, 6, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_t *dl = lv_label_create(del);
        lv_obj_center(dl);
        lv_label_set_text(dl, "×");
        lv_obj_set_style_text_font(dl, rv_ui_font(), 0);
        lv_obj_set_style_text_color(dl, RV_TEXT, 0);
        lv_obj_set_user_data(del, (void *)(intptr_t)i);
        lv_obj_add_event_cb(del, rv_fav_del_evt, LV_EVENT_CLICKED, rv);
    }
}

static void rv_fav_dlg_build(reader_view_t *rv)
{
    if (!rv || !rv->active || rv->fav_dlg) return;
    int sw = rv_screen_w(), sh = rv_screen_h();

    /* 全屏遮罩：点空白关闭 */
    lv_obj_t *dlg = lv_obj_create(rv->root);
    lv_obj_remove_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dlg, sw, sh);
    lv_obj_set_pos(dlg, 0, 0);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_40, 0);
    lv_obj_set_style_border_width(dlg, 0, 0);
    lv_obj_set_style_radius(dlg, 0, 0);
    lv_obj_set_style_pad_all(dlg, 0, 0);
    lv_obj_add_event_cb(dlg, rv_fav_dlg_dismiss_evt, LV_EVENT_CLICKED, rv);

    lv_obj_t *panel = lv_obj_create(dlg);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, sw - 20, sh - 60);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, RV_BTN_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* 退出按钮（底部边缘中间；该按钮在 rows 之前创建 → z 序在列表之上，
     * 按住列表滚动不误触，独立点击生效） */
    lv_obj_t *cx = lv_button_create(panel);
    lv_obj_set_size(cx, 88, 34);
    lv_obj_align(cx, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(cx, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(cx, lv_color_hex(0xF3F4F6), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(cx, RV_BTN_BORDER, 0);
    lv_obj_set_style_border_width(cx, 1, 0);
    lv_obj_set_style_radius(cx, 8, 0);
    lv_obj_set_style_pad_all(cx, 0, 0);
    lv_obj_t *xl = lv_label_create(cx);
    lv_obj_center(xl);
    lv_label_set_text(xl, "×");
    lv_obj_set_style_text_font(xl, rv_ui_font(), 0);
    lv_obj_set_style_text_color(xl, RV_TEXT, 0);
    lv_obj_add_event_cb(cx, rv_fav_dlg_dismiss_evt, LV_EVENT_CLICKED, rv);

    /* 列表占满标题区 + 上方留白（退出按钮之上） */
    lv_obj_t *rows = lv_obj_create(panel);
    lv_obj_set_pos(rows, 6, 6);
    lv_obj_set_size(rows, sw - 32, sh - 60 - 6 - 34 - 6 - 6);   /* 面板高 - 上下边距 - 按钮高 */
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_style_radius(rows, 0, 0);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_scroll_dir(rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(rows, 4, 0);

    rv->fav_dlg = dlg;
    rv->fav_rows = rows;
    rv_fav_dlg_refresh(rv);
}

static void rv_list_evt(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    if (!rv || !rv->active) return;
    if (rv->fav_dlg) {
        rv_fav_dlg_close(rv);   /* 再点关闭 */
        return;
    }
    rv_fav_dlg_build(rv);
}

/* 收藏/取消当前行（顶栏 ⭐）。顺序：先改内存 + 立即刷星标（视觉先行，
 * 立即变黄/灰），再做内容快照（可能触发 SD 缓存块读）与落盘（SD 写）——
 * 两者都可能阻塞数十 ms，放视觉之后避免"点了很久才变色"。 */
static void rv_fav_toggle(reader_view_t *rv)
{
    if (!rv || !rv->reader) return;
    int line = flow_view_get_view_top(rv->view);
    int idx = rv_fav_find(rv, line);
    if (idx >= 0) {
        memmove(&rv->fav.items[idx], &rv->fav.items[idx + 1],
                (size_t)(rv->fav.count - idx - 1) * sizeof(rv->fav.items[0]));
        rv->fav.count--;
        rv_refresh_star(rv);
        rv_fav_save(rv);
    } else {
        if (rv->fav.count >= FAV_MAX_ITEMS) {
            rv_toast(rv, "Fav full (64)");
            return;
        }
        if (reader_is_indexing(rv->reader)) {
            rv_toast(rv, "Indexing...");
            return;
        }
        /* 按行号有序插入（小列表线性插入），先留空内容占位 */
        int i = rv->fav.count;
        while (i > 0 && rv->fav.items[i - 1].line > line) {
            rv->fav.items[i] = rv->fav.items[i - 1];
            i--;
        }
        recent_fav_item_t *it = &rv->fav.items[i];
        it->line = line;
        it->content[0] = '\0';
        rv->fav.count++;
        rv_refresh_star(rv);       /* 立即变黄 */
        /* 内容快照（块读） + 落盘（写） */
        const char *s = reader_line_at(rv->reader, line);
        if (s) {
            strncpy(it->content, s, sizeof(it->content) - 1);
            it->content[sizeof(it->content) - 1] = '\0';
            for (char *p = it->content; *p; p++) {
                if (*p == '\t') *p = ' ';   /* 收藏文件以 \t 分隔，内容内的 tab 替换掉 */
            }
        } else {
            strncpy(it->content, "(unreadable)", sizeof(it->content) - 1);
        }
        rv_fav_save(rv);
    }
    if (rv->fav_dlg) rv_fav_dlg_refresh(rv);
}

static void rv_star_evt(lv_event_t *e)
{
    rv_fav_toggle(lv_event_get_user_data(e));
}

/* ── 底部扩展区：搜索跳行 / 行距 / 自动滚动 ── */

static void rv_auto_tick(lv_timer_t *t)
{
    reader_view_t *rv = t->user_data;
    if (!rv || !rv->active || !rv->auto_active || rv->indexing) return;
    int top = flow_view_get_view_top(rv->view);
    int max = flow_view_get_max_top(rv->view);
    if (top >= max) {
        rv_auto_stop(rv);   /* 滚到底自动停 */
        return;
    }
    flow_view_go_to(rv->view, top + 1);
}

static void rv_auto_stop(reader_view_t *rv)
{
    rv->auto_active = false;
    if (rv->auto_timer) {
        lv_timer_delete(rv->auto_timer);
        rv->auto_timer = NULL;
    }
    if (rv->auto_btn) {
        lv_obj_set_style_bg_color(rv->auto_btn, lv_color_hex(RV_AUTO_GREEN), 0);
        if (rv->auto_lbl) lv_label_set_text(rv->auto_lbl, "Auto");
    }
}

static void rv_auto_start(reader_view_t *rv)
{
    if (!rv->reader || reader_is_indexing(rv->reader)) return;
    rv->auto_active = true;
    if (!rv->auto_timer) rv->auto_timer = lv_timer_create(rv_auto_tick, RV_AUTO_TICK, rv);
    if (rv->auto_btn) {
        lv_obj_set_style_bg_color(rv->auto_btn, lv_color_hex(RV_AUTO_RED), 0);
        if (rv->auto_lbl) lv_label_set_text(rv->auto_lbl, "Stop");
    }
}

static void rv_auto_evt(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    if (!rv || !rv->active) return;
    if (rv->auto_active) rv_auto_stop(rv);
    else rv_auto_start(rv);
}

/* 搜索跳行：数字键盘输入（1 基），越界提示 */
static void rv_go_done(void *ctx, bool ok, int value)
{
    reader_view_t *rv = ctx;
    if (!rv || !ok || !rv->active) return;
    if (reader_is_indexing(rv->reader)) {
        rv_toast(rv, "Indexing...");
        return;
    }
    int line = value - 1;                       /* 输入 1 基 → 行号 0 基 */
    int total = reader_total_lines(rv->reader);
    if (line < 0 || total <= 0 || line >= total) {
        rv_toast(rv, "Line out of range");
        return;
    }
    rv_auto_stop(rv);                           /* 手动跳转 → 停自动滚动 */
    flow_view_go_to(rv->view, line);
    rv_update_progress(rv);
    rv_refresh_star(rv);
}

static void rv_go_evt(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    if (!rv || !rv->active) return;
    if (!rv->reader || reader_is_indexing(rv->reader)) {
        rv_toast(rv, "Indexing...");
        return;
    }
    num_input_show(rv->root, 1, 1, 1000000, false, 0, rv_go_done, rv);
}

/* 行距 ±（纯 UI：改可见行数，不动折行/索引） */
static void rv_spacing_evt(lv_event_t *e)
{
    reader_view_t *rv = lv_event_get_user_data(e);
    int delta = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!rv || !rv->active || delta == 0) return;
    int ns = rv->line_spacing + delta;
    if (ns < 0) ns = 0;
    if (ns > RV_SPACING_MAX) ns = RV_SPACING_MAX;
    if (ns == rv->line_spacing) return;
    rv->line_spacing = ns;
    flow_view_set_line_spacing(rv->view, ns);
    rv_update_progress(rv);
    rv_refresh_star(rv);
}

/* ── 打开/关闭 ── */

bool reader_view_open(reader_view_t *rv, const char *path)
{
    if (!rv) return false;
    if (rv->active) reader_view_close(rv);

    rv->active = true;
    rv->ui_hidden = false;

    lv_obj_set_x(rv->root, 0);   /* 清除上次拖动滑出/回弹残留的 x 偏移 */
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
    lv_obj_set_pos(rv->bar, 0, rv_screen_h() - RV_BAR_H);   /* 与 create/chrome 动画一致：贴屏幕底 */
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
    lv_obj_add_flag(rv->wheel, LV_OBJ_FLAG_HIDDEN);   /* 索引中不显示调速器 */
    const flow_view_line_provider_t prov = { reader_count, reader_line };
    flow_view_set_line_provider(rv->view, &prov, rv->reader);

    /* 读回本 txt 的 SD 收藏缓存 */
    strncpy(rv->path, path, sizeof(rv->path) - 1);
    rv->path[sizeof(rv->path) - 1] = '\0';
    rv_auto_stop(rv);
    rv->fav.count = 0;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        reader_fav_load(rv->path, &rv->fav);
        esp_lv_adapter_unlock();
    }
    rv_refresh_star(rv);

    lv_obj_clear_flag(rv->index_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(rv->index_lbl, "索引中 0%");
    lv_obj_update_layout(rv->root);
    return true;
}

void reader_view_close(reader_view_t *rv)
{
    if (!rv) return;
    rv_auto_stop(rv);
    rv_fav_dlg_close(rv);
    rv->active = false;
    rv->indexing = false;
    if (rv->toast) {
        if (rv->toast_timer) {
            lv_timer_delete(rv->toast_timer);
            rv->toast_timer = NULL;
        }
        lv_obj_add_flag(rv->toast, LV_OBJ_FLAG_HIDDEN);
    }
    if (rv->star_lbl) {
        lv_label_set_text(rv->star_lbl, "☆");
        lv_obj_set_style_text_color(rv->star_lbl, RV_FAV_GRAY, 0);
    }
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

lv_obj_t *reader_view_get_root(const reader_view_t *rv)
{
    return rv ? rv->root : NULL;
}

bool reader_view_handle_back(reader_view_t *rv)
{
    if (!rv || !rv->active) return false;
    if (num_input_is_active()) {
        num_input_cancel();   /* 数字键盘打开：先取消输入（返回键） */
        return true;
    }
    if (rv->fav_dlg) {
        rv_fav_dlg_close(rv);   /* 收藏弹窗打开：先关闭弹窗 */
        return true;
    }
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
    rv_auto_stop(rv);
    rv_fav_dlg_close(rv);
    if (rv->toast_timer) {
        lv_timer_delete(rv->toast_timer);
        rv->toast_timer = NULL;
    }
    if (rv->reader) reader_view_close(rv);
    if (rv->progress_timer) lv_timer_delete(rv->progress_timer);
    if (rv->root) lv_obj_delete(rv->root);
    free(rv);
}

/* ── 创建 ── */

/* 底部扩展区标准按钮（白底灰边、黑字、按下反白） */
static lv_obj_t *rv_bar_btn(lv_obj_t *row, const char *label,
                            lv_event_cb_t cb, void *ctx)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, RV_BTN_W, RV_BTN_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xF3F4F6), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, RV_BTN_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_center(l);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, rv_ui_font(), 0);
    lv_obj_set_style_text_color(l, RV_TEXT, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ctx);
    return b;
}

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

    /* 收藏星标（左上）：☆ 未收藏 / ★ 黄 已收藏当前行 */
    rv->star_btn = lv_button_create(rv->title);
    lv_obj_set_size(rv->star_btn, 44, RV_TITLE_H);
    lv_obj_set_ext_click_area(rv->star_btn, 16);   /* 顶栏小按钮加大热区，防轻触漂移丢点击 */
    lv_obj_set_style_bg_opa(rv->star_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(rv->star_btn, lv_color_hex(0xE5E7EB), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(rv->star_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(rv->star_btn, 0, 0);
    lv_obj_set_style_radius(rv->star_btn, 0, 0);
    lv_obj_set_style_pad_all(rv->star_btn, 0, 0);
    rv->star_lbl = lv_label_create(rv->star_btn);
    lv_obj_center(rv->star_lbl);
    lv_obj_set_style_text_font(rv->star_lbl, rv_ui_font(), 0);
    lv_obj_set_style_text_color(rv->star_lbl, RV_FAV_GRAY, 0);
    lv_label_set_text(rv->star_lbl, "☆");
    lv_obj_add_event_cb(rv->star_btn, rv_star_evt, LV_EVENT_CLICKED, rv);

    rv->title_label = lv_label_create(rv->title);
    lv_obj_set_flex_grow(rv->title_label, 1);
    lv_obj_set_height(rv->title_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(rv->title_label, RV_TEXT, 0);
    lv_obj_set_style_text_font(rv->title_label, rv_ui_font(), 0);
    lv_obj_set_style_text_align(rv->title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(rv->title_label, "");

    /* 收藏列表（右上） */
    rv->list_btn = lv_button_create(rv->title);
    lv_obj_set_size(rv->list_btn, 48, RV_TITLE_H);
    lv_obj_set_ext_click_area(rv->list_btn, 16);
    lv_obj_set_style_bg_opa(rv->list_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(rv->list_btn, lv_color_hex(0xE5E7EB), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(rv->list_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(rv->list_btn, 0, 0);
    lv_obj_set_style_radius(rv->list_btn, 0, 0);
    lv_obj_set_style_pad_all(rv->list_btn, 0, 0);
    lv_obj_t *fav_lbl = lv_label_create(rv->list_btn);
    lv_obj_center(fav_lbl);
    lv_obj_set_style_text_font(fav_lbl, rv_ui_font(), 0);
    lv_obj_set_style_text_color(fav_lbl, RV_TEXT, 0);
    lv_label_set_text(fav_lbl, "Fav");
    lv_obj_add_event_cb(rv->list_btn, rv_list_evt, LV_EVENT_CLICKED, rv);

    /* 底部设置栏：透明容器 + 上部进度条/气泡 + 下部白色按钮区（OVERFLOW_VISIBLE
     * 防气泡越界裁剪）。整体随栏显隐动画滑入/滑出（见 rv_chrome_anim） */
    rv->bar = lv_obj_create(root);
    lv_obj_set_size(rv->bar, lv_pct(100), RV_BAR_H);
    lv_obj_set_pos(rv->bar, 0, rv_screen_h() - RV_BAR_H);   /* 贴屏幕底，白色按钮区无底部空隙 */
    lv_obj_add_flag(rv->bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(rv->bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rv->bar, 0, 0);
    lv_obj_set_style_radius(rv->bar, 0, 0);
    lv_obj_set_style_pad_all(rv->bar, 0, 0);
    lv_obj_clear_flag(rv->bar, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_align(rv->prog, LV_ALIGN_TOP_MID, 0, RV_PROG_TOP);
    lv_obj_set_style_opa(rv->prog, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(rv->prog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rv->prog, 0, 0);
    lv_obj_set_style_radius(rv->prog, 0, 0);
    lv_obj_set_style_pad_all(rv->prog, 0, 0);
    lv_obj_set_ext_click_area(rv->prog, 10);
    lv_obj_add_event_cb(rv->prog, rv_prog_draw, LV_EVENT_DRAW_MAIN, rv);
    lv_obj_add_event_cb(rv->prog, rv_prog_event, LV_EVENT_ALL, rv);

    /* 白色按钮区（位于进度条下方，贴屏幕底；随栏一起滑入/滑出） */
    lv_obj_t *panel = lv_obj_create(rv->bar);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, 0, RV_BAR_H - RV_BTN_PANEL_H);
    lv_obj_set_size(panel, lv_pct(100), RV_BTN_PANEL_H);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, RV_BTN_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_pad_gap(panel, RV_BTN_GAP, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    rv->go_btn = rv_bar_btn(panel, "Go", rv_go_evt, rv);          /* 🔍 搜索跳行 */
    rv->spacing_m_btn = rv_bar_btn(panel, "-", rv_spacing_evt, rv);
    lv_obj_set_user_data(rv->spacing_m_btn, (void *)(intptr_t)(-RV_SPACING_STEP));
    rv->spacing_p_btn = rv_bar_btn(panel, "+", rv_spacing_evt, rv);
    lv_obj_set_user_data(rv->spacing_p_btn, (void *)(intptr_t)(RV_SPACING_STEP));

    /* 自动滚动：绿 Auto →（点击）慢速下滚 → 红 Stop →（点击停止） */
    rv->auto_btn = lv_button_create(panel);
    lv_obj_set_size(rv->auto_btn, RV_BTN_W, RV_BTN_H);
    lv_obj_set_style_bg_color(rv->auto_btn, lv_color_hex(RV_AUTO_GREEN), 0);
    lv_obj_set_style_border_width(rv->auto_btn, 0, 0);
    lv_obj_set_style_radius(rv->auto_btn, 8, 0);
    lv_obj_set_style_pad_all(rv->auto_btn, 0, 0);
    rv->auto_lbl = lv_label_create(rv->auto_btn);
    lv_obj_center(rv->auto_lbl);
    lv_obj_set_style_text_font(rv->auto_lbl, rv_ui_font(), 0);
    lv_obj_set_style_text_color(rv->auto_lbl, lv_color_white(), 0);
    lv_label_set_text(rv->auto_lbl, "Auto");
    lv_obj_add_event_cb(rv->auto_btn, rv_auto_evt, LV_EVENT_CLICKED, rv);

    rv->progress_timer = lv_timer_create(rv_progress_timer, 500, rv);

    rv->index_lbl = lv_label_create(root);
    lv_obj_center(rv->index_lbl);
    lv_obj_set_style_text_color(rv->index_lbl, RV_TEXT, 0);
    lv_obj_set_style_text_font(rv->index_lbl, rv_ui_font(), 0);
    lv_obj_add_flag(rv->index_lbl, LV_OBJ_FLAG_HIDDEN);

    /* 右侧调速器（系统组件）：悬浮右缘中间，上下拖动滚动 txt */
    rv->wheel = speed_wheel_create(root, 100, rv_speed_cb, rv);
    lv_obj_set_pos(rv->wheel, rv_screen_w() - 36 - 6, (rv_screen_h() - 100) / 2);

    /* 提示（居中短时消息） */
    rv->toast = lv_label_create(root);
    lv_obj_center(rv->toast);
    lv_obj_set_style_bg_color(rv->toast, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(rv->toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rv->toast, 8, 0);
    lv_obj_set_style_text_color(rv->toast, lv_color_white(), 0);
    lv_obj_set_style_text_font(rv->toast, rv_ui_font(), 0);
    lv_obj_set_style_pad_hor(rv->toast, 10, 0);
    lv_obj_set_style_pad_ver(rv->toast, 4, 0);
    lv_obj_add_flag(rv->toast, LV_OBJ_FLAG_HIDDEN);

    /* 订阅上边缘下滑（显示状态栏）：全局手势，本阅读器响应 */
    gesture_set_topdrop_handler(rv_topdrop_cb, rv);

    return rv;
}
