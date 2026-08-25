/* reader_app.c —— 阅读器 APP（LVGL 9）：书架页（扫描 SD 列 txt）+ 阅读页（reader_view）。
 * 由 launcher 统一管理：arg=NULL 书架模式；arg=路径 直接打开指定 txt。 */

#include "reader_app.h"
#include "drv_sdcard.h"
#include "app_sdcard.h"
#include "app_font.h"
#include "reader_view.h"
#include "sd_async.h"
#include "esp_lv_adapter.h"
#include "launcher.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "misc/lv_timer_private.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#define RA_PATH_MAX     128
#define RA_ENTRY_MAX    128      /* 扫描收集上限（128×208B≈26KB 内部 RAM） */
#define RA_NAME_MAX     80       /* 显示名（去后缀） */
#define RA_TOP_BAR_H    28
#define RA_ROW_H        48       /* 书架行高（书名 + 状态说明 + 右侧星） */
#define RA_SETBAR_H     44       /* 底部设置栏高（Favs/Sort/View/翻页） */
#define RA_PAGE_ROWS    3        /* 翻页模式每页行数（list 高 ≈ 172px / 48px 行高） */
#define RA_BTN_W        46
#define RA_BTN_H        32
#define RA_BTN_GAP      8
#define RA_DIRQ_MAX     128      /* 待扫描目录队列上限（BFS，PSRAM 分配） */
#define RA_MAX_DEPTH    8        /* 扫描最大深度（广度优先，栈需求恒定） */
#define RA_SCAN_DEPTH   1        /* 根目录 + 一级子目录 */

/* 排序 / 显示模式 */
typedef enum {
    RA_SORT_NAME_ASC,
    RA_SORT_NAME_DESC,
    RA_SORT_MTIME_NEW,
    RA_SORT_MTIME_OLD,
} ra_sort_t;

typedef enum {
    RA_VIEW_SCROLL,
    RA_VIEW_PAGES,
} ra_view_t;

/* 配色（与 file_browser 深色一致） */
#define RA_BG           lv_color_hex(0x000000)
#define RA_ROW          lv_color_hex(0x000000)
#define RA_ROW_PRESSED  lv_color_hex(0x374151)
#define RA_TEXT         lv_color_hex(0xFFFFFF)
#define RA_EMPTY        lv_color_hex(0x9CA3AF)
#define RA_BTN_BORDER   lv_color_hex(0x374151)
#define RA_FAV_YELLOW   lv_color_hex(0xFBBF24)
#define RA_FAV_GRAY     lv_color_hex(0x9CA3AF)

typedef struct {
    char path[RA_PATH_MAX];   /* 完整路径（打开阅读用） */
    char name[RA_NAME_MAX];   /* 显示名（去 .txt 后缀） */
    time_t mtime;             /* 修改时间（排序用） */
    bool favbook;             /* 是否收藏本书（.favbook 存在） */
    int prog_state;           /* 阅读进度：-1 无；-2 完成；>=0 行号（0 基） */
} ra_entry_t;

struct reader_app {
    lv_obj_t *root;
    lv_obj_t *list;
    lv_obj_t *lbl_count;
    reader_app_back_cb_t back_cb;
    void *back_ctx;
    reader_view_t *rv;
    lv_timer_t *no_sd_timer;   /* 无 SD 自动返回定时器 */
    bool direct_mode;          /* 直接打开模式（arg 带路径）：右滑关阅读页直接回来源 */
    char *direct_arg;          /* launch arg 副本（create 时保存；launcher 的 s_launch_arg
                                * 在 create 返回后即清空，entered 延迟读取需用此副本） */

    ra_entry_t *entries;
    int entry_count;
    int pending_idx;          /* 待打开的书索引（事件回调置位，定时器执行） */
    lv_timer_t *defer_timer;  /* 延迟打开一次性定时器 */
    /* 扫描上下文（回调内使用） */
    char scan_dir[RA_PATH_MAX];
    int scan_depth;
    /* 广度优先目录队列（PSRAM，回调里入队） */
    struct { char path[RA_PATH_MAX]; int depth; } *dirq;
    int dirq_tail;
    bool dirq_oom;              /* 队列满标志 */

    /* 书架视图状态 */
    ra_sort_t sort;
    ra_view_t view_mode;
    int page;                   /* 翻页模式当前页（0 基） */
    int page_count;
    lv_obj_t *setbar;           /* 底部设置栏 */
    lv_obj_t *btn_favs, *btn_sort, *btn_view, *btn_prev, *btn_next, *btn_page; /* 页码按钮 */
    /* 弹窗（收藏夹/排序/视图） */
    lv_obj_t *dlg;              /* NULL=未开 */
    lv_obj_t *dlg_rows;
};

static int ra_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_horizontal_resolution(d) : 320;
}

static lv_font_t *ra_ui_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static bool ra_is_txt(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    return strcasecmp(name + n - 4, ".txt") == 0;
}

/* 书名点击：延迟打开阅读（一次性定时器，避开 indev 事件上下文） */
static void ra_open_deferred(lv_timer_t *t)
{
    reader_app_t *app = t->user_data;
    app->defer_timer = NULL;
    if (!app || app->pending_idx < 0) return;
    int idx = app->pending_idx;
    app->pending_idx = -1;
    if (app->entries && idx < app->entry_count && !reader_view_active(app->rv)) {
        reader_view_open(app->rv, app->entries[idx].path);
    }
}

/* 直接打开模式打开失败（空文件等）：延迟返回来源 APP */
static void ra_open_fail_back(lv_timer_t *t)
{
    reader_app_t *app = t->user_data;
    app->defer_timer = NULL;
    if (app && app->back_cb) app->back_cb(app->back_ctx);
}

static void ra_item_event(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    reader_app_t *app = lv_event_get_user_data(e);
    if (!app || !row || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    if (idx < 0 || idx >= app->entry_count || reader_view_active(app->rv)) return;
    app->pending_idx = idx;
    app->defer_timer = lv_timer_create(ra_open_deferred, 1, app);
    lv_timer_set_repeat_count(app->defer_timer, 1);
}

/* ── 扫描（LVGL 线程同步执行，非递归 BFS：根 + 一级子目录。
 *    与 LCD 渲染同线程 → 共享 SPI2 总线串行访问，避免跨任务并发冲突） ── */

static void ra_entry_cb(void *ctx, const char *name, bool is_dir, long size, time_t mtime)
{
    (void)size;
    reader_app_t *app = ctx;
    if (is_dir) {
        if (app->scan_depth < RA_MAX_DEPTH && app->dirq_tail < RA_DIRQ_MAX) {
            if (snprintf(app->dirq[app->dirq_tail].path, sizeof(app->dirq[app->dirq_tail].path),
                         "%s/%s", app->scan_dir, name)
                < (int)sizeof(app->dirq[app->dirq_tail].path)) {
                app->dirq[app->dirq_tail].depth = app->scan_depth + 1;
                app->dirq_tail++;
            }
        }
        return;
    }
    /* 只收 txt；去后缀显示名 */
    if (!ra_is_txt(name)) return;
    if (app->entry_count >= RA_ENTRY_MAX) return;
    if (app->scan_depth > RA_SCAN_DEPTH) return;   /* 只收根 + 一级子目录 */
    ra_entry_t *e = &app->entries[app->entry_count];
    if (snprintf(e->path, sizeof(e->path), "%s/%s", app->scan_dir, name)
        >= (int)sizeof(e->path)) return;
    size_t n = strlen(name);
    size_t nl = n - 4;   /* 去 .txt 后缀长度 */
    if (nl >= sizeof(e->name)) nl = sizeof(e->name) - 1;
    memcpy(e->name, name, nl);
    e->name[nl] = '\0';
    e->mtime = mtime;
    e->favbook = false;
    e->prog_state = -1;
    app->entry_count++;
}

/* 当前排序模式（qsort 无 ctx，用静态） */
static ra_sort_t s_cmp_sort = RA_SORT_NAME_ASC;

static int ra_cmp(const void *a, const void *b)
{
    const ra_entry_t *ea = a, *eb = b;
    switch (s_cmp_sort) {
    case RA_SORT_NAME_DESC:
        return -strcasecmp(ea->name, eb->name);
    case RA_SORT_MTIME_NEW:
        return (eb->mtime > ea->mtime) - (eb->mtime < ea->mtime);
    case RA_SORT_MTIME_OLD:
        return (ea->mtime > eb->mtime) - (ea->mtime < eb->mtime);
    default:
        return strcasecmp(ea->name, eb->name);
    }
}

/* 无 SD 卡：显示提示后自动返回 */
static void ra_no_sd_back(lv_timer_t *t)
{
    reader_app_t *app = t->user_data;
    if (!app) return;
    app->no_sd_timer = NULL;
    if (app->back_cb) app->back_cb(app->back_ctx);
}

/* ── 书架交互：行渲染 / 收藏本书 / 弹窗 / 设置栏 ── */

static void ra_reload_states(reader_app_t *app)
{
    if (app->entry_count <= 0) return;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        for (int i = 0; i < app->entry_count; i++) {
            ra_entry_t *b = &app->entries[i];
            b->favbook = sd_favbook_exists(b->path);
            b->prog_state = sd_read_prog(b->path);
        }
        esp_lv_adapter_unlock();
    }
}

static void ra_update_count(reader_app_t *app)
{
    if (!app->lbl_count) return;
    if (app->view_mode == RA_VIEW_PAGES && app->page_count > 0) {
        lv_label_set_text_fmt(app->lbl_count, "%d/%d", app->page + 1, app->page_count);
    } else {
        lv_label_set_text_fmt(app->lbl_count, "%d 本", app->entry_count);
    }
}

/* 书架行渲染（书名放大 + 状态说明 + 右星） */
static void ra_build_row(lv_obj_t *row, const ra_entry_t *b, int idx, reader_app_t *app);

/* 书架行右侧 ★：切换本书收藏（内存即时 + 异步落盘），并重建本行刷新星显 */
static void ra_star_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    lv_obj_t *star = lv_event_get_target_obj(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(star);
    if (!app || idx < 0 || idx >= app->entry_count) return;
    ra_entry_t *b = &app->entries[idx];
    b->favbook = !b->favbook;
    sd_async_set_favbook(b->path, b->favbook);
    lv_obj_t *row = lv_obj_get_parent(star);
    if (row) {
        lv_obj_clean(row);
        ra_build_row(row, b, idx, app);
    }
    lv_obj_update_layout(app->root);
}

/* 书架行渲染（书名放大 + 状态说明 + 右星） */
static void ra_build_row(lv_obj_t *row, const ra_entry_t *b, int idx, reader_app_t *app)
{
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_gap(col, 4, 0);   /* 缩放后的书名与状态行留距 */
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, b->name);
    lv_obj_set_style_text_color(name, RA_TEXT, 0);
    lv_obj_set_style_text_font(name, ra_ui_font(), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_transform_scale(name, 320, 0);   /* 视觉 ~20px（内置中文仅 16px，缩放放大） */

    lv_obj_t *st = lv_label_create(col);
    lv_obj_set_style_text_color(st, RA_EMPTY, 0);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
    if (b->prog_state == -2) lv_label_set_text(st, "Completed");
    else if (b->prog_state >= 0) lv_label_set_text_fmt(st, "Read L%d", b->prog_state + 1);
    else lv_label_set_text(st, "Not read");

    lv_obj_t *star = lv_button_create(row);
    lv_obj_set_size(star, RA_ROW_H, RA_ROW_H);   /* 视觉方形，热区大 */
    lv_obj_set_ext_click_area(star, 10);
    lv_obj_set_style_bg_opa(star, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(star, lv_color_hex(0x1F2937), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(star, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(star, 0, 0);
    lv_obj_set_style_radius(star, 0, 0);
    lv_obj_set_style_pad_all(star, 0, 0);
    lv_obj_t *sl = lv_label_create(star);
    lv_obj_center(sl);
    lv_label_set_text(sl, b->favbook ? "★" : "☆");
    lv_obj_set_style_text_color(sl, b->favbook ? RA_FAV_YELLOW : RA_FAV_GRAY, 0);
    lv_obj_set_style_text_font(sl, ra_ui_font(), 0);
    lv_obj_set_user_data(star, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(star, ra_star_evt, LV_EVENT_CLICKED, app);
}

/* 统一渲染：滚动模式全量；翻页模式只画当前页（行数 = RA_PAGE_ROWS） */
static void ra_render_list(reader_app_t *app)
{
    lv_obj_clean(app->list);
    if (app->entry_count == 0) {
        lv_obj_t *t = lv_label_create(app->list);
        lv_label_set_text(t, "(没有 TXT 文件)");
        lv_obj_set_style_text_color(t, RA_EMPTY, 0);
        return;
    }
    app->page_count = (app->entry_count + RA_PAGE_ROWS - 1) / RA_PAGE_ROWS;
    if (app->page >= app->page_count) app->page = app->page_count - 1;
    if (app->page < 0) app->page = 0;

    int start = (app->view_mode == RA_VIEW_PAGES) ? app->page * RA_PAGE_ROWS : 0;
    int end = (app->view_mode == RA_VIEW_PAGES) ? (app->page + 1) * RA_PAGE_ROWS : app->entry_count;
    if (end > app->entry_count) end = app->entry_count;

    for (int i = start; i < end; i++) {
        lv_obj_t *row = lv_obj_create(app->list);
        lv_obj_set_size(row, lv_pct(100), RA_ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_color(row, RA_ROW, 0);
        lv_obj_set_style_bg_color(row, RA_ROW_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, ra_item_event, LV_EVENT_CLICKED, app);
        ra_build_row(row, &app->entries[i], i, app);
    }
}

/* ── 弹窗（收藏夹 / 排序 / 视图），复用阅读器收藏弹窗样式 ── */

static void ra_dlg_close(reader_app_t *app)
{
    if (!app || !app->dlg) return;
    lv_obj_delete(app->dlg);
    app->dlg = NULL;
    app->dlg_rows = NULL;
}

static void ra_dlg_dismiss_evt(lv_event_t *e)
{
    ra_dlg_close(lv_event_get_user_data(e));
}

/* 建空弹窗（遮罩 + 白面板 + 行列表 + 底部 ×），随后填充行 */
static void ra_dlg_build(reader_app_t *app)
{
    if (!app || app->dlg) return;
    lv_display_t *disp = lv_display_get_default();
    int sw = disp ? lv_display_get_horizontal_resolution(disp) : 320;
    int sh = disp ? lv_display_get_vertical_resolution(disp) : 240;

    lv_obj_t *dlg = lv_obj_create(app->root);
    lv_obj_remove_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dlg, sw, sh);
    lv_obj_set_pos(dlg, 0, 0);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_40, 0);
    lv_obj_set_style_border_width(dlg, 0, 0);
    lv_obj_set_style_radius(dlg, 0, 0);
    lv_obj_set_style_pad_all(dlg, 0, 0);
    lv_obj_add_event_cb(dlg, ra_dlg_dismiss_evt, LV_EVENT_CLICKED, app);   /* 点空白关闭 */

    lv_obj_t *panel = lv_obj_create(dlg);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, sw - 20, sh - 60);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    lv_obj_t *clos = lv_button_create(panel);
    lv_obj_set_size(clos, 36, 36);
    lv_obj_align(clos, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(clos, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(clos, lv_color_hex(0xF3F4F6), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(clos, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_border_width(clos, 1, 0);
    lv_obj_set_style_radius(clos, 8, 0);
    lv_obj_set_style_pad_all(clos, 0, 0);
    lv_obj_t *xl = lv_label_create(clos);
    lv_obj_center(xl);
    lv_label_set_text(xl, "×");
    lv_obj_set_style_text_font(xl, ra_ui_font(), 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x111111), 0);
    lv_obj_add_event_cb(clos, ra_dlg_dismiss_evt, LV_EVENT_CLICKED, app);

    lv_obj_t *rows = lv_obj_create(panel);
    lv_obj_set_pos(rows, 6, 6);
    lv_obj_set_size(rows, sw - 32, sh - 60 - 6 - 36 - 6 - 6);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_style_radius(rows, 0, 0);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_scroll_dir(rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(rows, 4, 0);

    app->dlg = dlg;
    app->dlg_rows = rows;
}

/* 弹窗内单行（白底黑字，可点击） */
static void ra_dlg_row(reader_app_t *app, const char *label, lv_event_cb_t cb, int user)
{
    if (!app || !app->dlg_rows) return;
    lv_obj_t *row = lv_obj_create(app->dlg_rows);
    lv_obj_set_size(row, lv_pct(100), 40);
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
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_color(l, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(l, ra_ui_font(), 0);
    lv_obj_set_style_pad_left(l, 8, 0);
    lv_label_set_text(l, label);
    if (cb) {
        lv_obj_set_user_data(row, (void *)(intptr_t)user);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, app);
    }
}

/* 收藏夹：列出已收藏本书，点击关闭弹窗并打开该 txt（复用书架行打开流程） */
static void ra_fav_book_open_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    ra_dlg_close(app);
    if (!app || idx < 0 || idx >= app->entry_count) return;
    if (reader_view_active(app->rv)) return;
    app->pending_idx = idx;
    app->defer_timer = lv_timer_create(ra_open_deferred, 1, app);
    lv_timer_set_repeat_count(app->defer_timer, 1);
}

static void ra_favs_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    if (!app) return;
    ra_dlg_build(app);
    if (!app->dlg_rows) return;
    int shown = 0;
    for (int i = 0; i < app->entry_count; i++) {
        if (!app->entries[i].favbook) continue;
        ra_dlg_row(app, app->entries[i].name, ra_fav_book_open_evt, i);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *l = lv_label_create(app->dlg_rows);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, RA_EMPTY, 0);
        lv_obj_set_style_text_font(l, ra_ui_font(), 0);
        lv_label_set_text(l, "No favorites");
    }
}

/* 排序：弹出选项列表 */
static void ra_sort_pick_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!app) return;
    ra_dlg_close(app);
    app->sort = (ra_sort_t)v;
    app->page = 0;
    s_cmp_sort = app->sort;
    qsort(app->entries, app->entry_count, sizeof(ra_entry_t), ra_cmp);
    ra_render_list(app);
    ra_update_count(app);
}

static void ra_sort_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    if (!app) return;
    ra_dlg_build(app);
    if (!app->dlg_rows) return;
    ra_dlg_row(app, "Name A-Z", ra_sort_pick_evt, RA_SORT_NAME_ASC);
    ra_dlg_row(app, "Name Z-A", ra_sort_pick_evt, RA_SORT_NAME_DESC);
    ra_dlg_row(app, "Newest",   ra_sort_pick_evt, RA_SORT_MTIME_NEW);
    ra_dlg_row(app, "Oldest",   ra_sort_pick_evt, RA_SORT_MTIME_OLD);
}

/* 显示模式：滚动 / 翻页 */
static void ra_view_pick_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!app) return;
    ra_dlg_close(app);
    app->view_mode = (ra_view_t)v;
    app->page = 0;
    if (app->view_mode == RA_VIEW_PAGES) {
        lv_obj_clear_flag(app->btn_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(app->btn_next, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(app->btn_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(app->btn_next, LV_OBJ_FLAG_HIDDEN);
    }
    ra_render_list(app);
    ra_update_count(app);
}

static void ra_view_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    if (!app) return;
    ra_dlg_build(app);
    if (!app->dlg_rows) return;
    ra_dlg_row(app, "Scroll", ra_view_pick_evt, RA_VIEW_SCROLL);
    ra_dlg_row(app, "Pages",  ra_view_pick_evt, RA_VIEW_PAGES);
}

/* 翻页 ◀ ▶ */
static void ra_prev_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    if (!app || app->view_mode != RA_VIEW_PAGES) return;
    ra_dlg_close(app);
    if (app->page > 0) {
        app->page--;
        ra_render_list(app);
        ra_update_count(app);
    }
}

static void ra_next_evt(lv_event_t *e)
{
    reader_app_t *app = lv_event_get_user_data(e);
    if (!app || app->view_mode != RA_VIEW_PAGES) return;
    ra_dlg_close(app);
    if (app->page + 1 < app->page_count) {
        app->page++;
        ra_render_list(app);
        ra_update_count(app);
    }
}

/* 底部设置栏标准按钮（白面板上：白底黑字） */
static lv_obj_t *ra_bar_btn(lv_obj_t *row, const char *label, lv_event_cb_t cb, void *ctx)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, RA_BTN_W, RA_BTN_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xF3F4F6), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_center(l);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, ra_ui_font(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x111111), 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ctx);
    return b;
}

static void ra_scan(reader_app_t *app)
{
    lv_obj_clean(app->list);
    app->entry_count = 0;

    /* 条目缓冲（PSRAM，释放内部 RAM） */
    if (app->entries) {
        heap_caps_free(app->entries);
        app->entries = NULL;
    }
    app->entries = heap_caps_calloc(RA_ENTRY_MAX, sizeof(ra_entry_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!app->entries) {
        lv_obj_t *t = lv_label_create(app->list);
        lv_label_set_text(t, "(no memory)");
        lv_obj_set_style_text_color(t, RA_EMPTY, 0);
        return;
    }

    /* 目录队列（PSRAM，广度优先，栈需求恒定） */
    if (app->dirq) {
        heap_caps_free(app->dirq);
        app->dirq = NULL;
    }
    app->dirq = heap_caps_calloc(RA_DIRQ_MAX, sizeof(*app->dirq),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!app->dirq) {
        heap_caps_free(app->entries);
        app->entries = NULL;
        lv_obj_t *t = lv_label_create(app->list);
        lv_label_set_text(t, "(no memory)");
        lv_obj_set_style_text_color(t, RA_EMPTY, 0);
        return;
    }
    app->dirq_tail = 0;
    app->dirq_oom = false;
    snprintf(app->dirq[0].path, sizeof(app->dirq[0].path), "%s", DRV_SDCARD_MOUNT_POINT);
    app->dirq[0].depth = 0;
    app->dirq_tail = 1;   /* 根目录入队（漏置 1 会导致 BFS 循环不执行） */

    /* 广度优先遍历：根目录 + 子目录（队列迭代，任意深度栈需求恒定）。
     * 全程持 SD 锁：与后台 sd_async 写盘串行（共享 SPI2 总线） */
    bool sd_ok = true;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        for (int qh = 0; qh < app->dirq_tail; qh++) {
            strncpy(app->scan_dir, app->dirq[qh].path, sizeof(app->scan_dir) - 1);
            app->scan_depth = app->dirq[qh].depth;
            esp_err_t err = app_sdcard_list_dir(app->dirq[qh].path, ra_entry_cb, app);
            if (err != ESP_OK && qh == 0) {
                sd_ok = false;   /* 根目录打不开 = SD 未就绪 */
            }
        }
        esp_lv_adapter_unlock();
    }

    if (!sd_ok) {
        /* SD 未就绪：提示 + 延迟自动返回 */
        heap_caps_free(app->entries);
        app->entries = NULL;
        heap_caps_free(app->dirq);
        app->dirq = NULL;
        lv_obj_t *t = lv_label_create(app->list);
        lv_label_set_text(t, "无 SD 卡");
        lv_obj_set_style_text_color(t, RA_EMPTY, 0);
        lv_obj_set_style_text_font(t, ra_ui_font(), 0);
        if (app->no_sd_timer) lv_timer_delete(app->no_sd_timer);
        app->no_sd_timer = lv_timer_create(ra_no_sd_back, 1000, app);
        lv_timer_set_repeat_count(app->no_sd_timer, 1);
        lv_label_set_text(app->lbl_count, "0 本");
        return;
    }

    if (app->no_sd_timer) {
        lv_timer_delete(app->no_sd_timer);
        app->no_sd_timer = NULL;
    }

    if (app->entry_count > 0) {
        s_cmp_sort = app->sort;
        qsort(app->entries, app->entry_count, sizeof(ra_entry_t), ra_cmp);
        ra_reload_states(app);   /* 读每本书的收藏/进度状态（持 SD 锁） */
    }
    ra_render_list(app);
    ra_update_count(app);
}

/* ── Public API ── */

reader_app_t *reader_app_create(lv_obj_t *parent, reader_app_back_cb_t back_cb, void *ctx)
{
    reader_app_t *app = calloc(1, sizeof(reader_app_t));
    if (!app) return NULL;
    app->back_cb = back_cb;
    app->back_ctx = ctx;
    app->pending_idx = -1;
    /* create 期间 s_launch_arg 仍有效（launcher 在 launch() 返回后清空），
     * 立即保存副本供 entered（动画完成后）使用 */
    const char *arg = launcher_app_get_arg();
    if (arg && *arg) {
        app->direct_arg = strdup(arg);
        app->direct_mode = true;
    }

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, RA_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    app->root = root;

    /* 顶部栏：书架（居中） | N 本（右上） */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, lv_pct(100), RA_TOP_BAR_H);
    lv_obj_set_style_bg_color(bar, RA_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, RA_BTN_BORDER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_color(title, RA_TEXT, 0);
    lv_obj_set_style_text_font(title, ra_ui_font(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "书架");

    app->lbl_count = lv_label_create(bar);
    lv_obj_set_width(app->lbl_count, 56);
    lv_obj_set_style_text_color(app->lbl_count, RA_TEXT, 0);
    lv_obj_set_style_text_font(app->lbl_count, ra_ui_font(), 0);
    lv_obj_set_style_text_align(app->lbl_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_right(app->lbl_count, 8, 0);
    lv_label_set_text(app->lbl_count, "0 本");

    /* 中部列表（顶栏与底部设置栏之间） */
    int list_h = lv_obj_get_height(parent) - RA_TOP_BAR_H - RA_SETBAR_H;
    app->list = lv_list_create(root);
    lv_obj_set_size(app->list, lv_pct(100), list_h);
    lv_obj_align(app->list, LV_ALIGN_TOP_LEFT, 0, RA_TOP_BAR_H);
    lv_obj_set_style_bg_color(app->list, RA_BG, 0);
    lv_obj_set_style_border_width(app->list, 0, 0);
    lv_obj_set_style_radius(app->list, 0, 0);
    lv_obj_set_style_pad_all(app->list, 0, 0);
    lv_obj_set_style_pad_row(app->list, 2, 0);
    lv_obj_set_style_text_color(app->list, RA_TEXT, 0);

    /* 底部设置栏（白面板，贴底）：Favs / Sort / View + 翻页 ◀▶ */
    app->setbar = lv_obj_create(root);
    lv_obj_remove_flag(app->setbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(app->setbar, 0, lv_obj_get_height(parent) - RA_SETBAR_H);
    lv_obj_set_size(app->setbar, lv_pct(100), RA_SETBAR_H);
    lv_obj_set_style_bg_color(app->setbar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(app->setbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(app->setbar, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_border_width(app->setbar, 1, 0);
    lv_obj_set_style_border_side(app->setbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(app->setbar, 8, 0);
    lv_obj_set_style_pad_all(app->setbar, 0, 0);
    lv_obj_set_style_pad_gap(app->setbar, RA_BTN_GAP, 0);
    lv_obj_set_flex_flow(app->setbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(app->setbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    app->btn_favs = ra_bar_btn(app->setbar, "Favs", ra_favs_evt, app);
    app->btn_sort = ra_bar_btn(app->setbar, "Sort", ra_sort_evt, app);
    app->btn_view = ra_bar_btn(app->setbar, "View", ra_view_evt, app);
    app->btn_prev = ra_bar_btn(app->setbar, "<", ra_prev_evt, app);
    app->btn_next = ra_bar_btn(app->setbar, ">", ra_next_evt, app);
    lv_obj_add_flag(app->btn_prev, LV_OBJ_FLAG_HIDDEN);   /* 翻页模式才显示 */
    lv_obj_add_flag(app->btn_next, LV_OBJ_FLAG_HIDDEN);

    /* 阅读器（全屏覆盖层，返回回书架） */
    app->rv = reader_view_create(root);
    if (app->rv) {
        reader_view_set_back_cb(app->rv, NULL, NULL);   /* 返回 = 仅关闭覆盖层，留在书架 */
    }

    /* 业务（扫描书架 / 直接打开文件）延迟到进入动画完成后（reader_app_entered），
     * 滑入期间只渲染 UI。direct-open 需要父对象已布局，entered 在动画完成
     * （~300ms 后）调用时布局早已完成。 */
    return app;
}

/* 进入动画完成（launcher 回调）：扫描书架或直接打开文件 */
void reader_app_entered(reader_app_t *app)
{
    if (!app) return;

    /* arg 副本在 create 时已保存（launcher 的 s_launch_arg 此时已清空） */
    const char *arg = app->direct_arg;
    if (arg && *arg) {
        ESP_LOGI("reader_app", "direct-open mode: %s", arg);
        /* 强制布局：flow_view 视口宽度依赖父对象已布局尺寸 */
        lv_obj_update_layout(app->root);
        if (app->rv) {
            if (!reader_view_open(app->rv, arg)) {
                ESP_LOGW("reader_app", "direct-open failed: %s", arg);
                /* 打开失败（空文件等）：隐藏书架 UI 防闪现，延迟弹栈返回来源 APP。
                 * entered 在动画完成（launcher 已入栈）后调用，可安全弹栈。 */
                lv_obj_add_flag(app->root, LV_OBJ_FLAG_HIDDEN);
                app->defer_timer = lv_timer_create(ra_open_fail_back, 1, app);
                lv_timer_set_repeat_count(app->defer_timer, 1);
            }
        }
    } else {
        ra_scan(app);
    }
}

void reader_app_destroy(reader_app_t *app)
{
    if (!app) return;
    if (app->no_sd_timer) lv_timer_delete(app->no_sd_timer);
    if (app->defer_timer) lv_timer_delete(app->defer_timer);
    if (app->dlg) ra_dlg_close(app);
    if (app->rv) reader_view_destroy(app->rv);
    if (app->entries) heap_caps_free(app->entries);
    if (app->dirq) heap_caps_free(app->dirq);
    free(app->direct_arg);
    /* 闪屏修复：先隐藏 + 立即刷新一帧（露出来源），再删除全屏对象 */
    if (app->root) {
        lv_obj_add_flag(app->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(app->root);
    }
    free(app);
}

void reader_app_refresh(reader_app_t *app)
{
    if (app) ra_scan(app);
}

/* 右滑返回（launcher 分发）：全屏 UI 一律允许跟手拖动（返回 true）。
 * 滑出后的行为由 reader_app_drag_exit 决定（返回目标按入口/栈确定：
 * direct 模式→弹栈回 file_browser；书架模式→关阅读层回书架）。 */
bool reader_app_swipe_back(reader_app_t *app)
{
    if (!app) return true;
    if (app->dlg) {
        ra_dlg_close(app);   /* 书架弹窗打开：先关闭（一步一动作） */
        return false;
    }
    /* 阅读页打开时：右滑返回手势先问阅读器——状态栏显示中则隐藏栏并拦截
     * （返回 false，取消拖动）；栏已隐藏则放行进入跟手拖动。
     * 书架模式：拖动由 drag_root 指定为阅读覆盖层（露出书架而非桌面），
     * 滑出后再由 drag_exit 关阅读层回书架 */
    if (app->rv && reader_view_active(app->rv)) {
        if (reader_view_handle_back(app->rv)) return false;   /* 栏显示→隐藏栏，拦截 */
        return true;   /* 栏已隐藏 → 放行跟手拖动（目标见 drag_root） */
    }
    return true;
}

/* 返回拖动时要平移的对象：书架模式阅读页 → 阅读覆盖层（露出下方书架，
 * 不闪桌面）；其余返回 NULL 用默认整 root。仅在被拖动（栏已隐藏）时被查询，
 * 故栏显示状态不影响此判定 */
lv_obj_t *reader_app_drag_root(reader_app_t *app)
{
    if (app && app->rv && reader_view_active(app->rv) && !app->direct_mode) {
        return reader_view_get_root(app->rv);
    }
    return NULL;
}

/* 拖动返回滑出动画完成（launcher 回调）：root 已滑到屏外。
 * 返回目标由入口决定（用户约定：上一级按栈确定，书架与文件浏览器都能打开 txt）：
 *   direct 模式（file_browser 跳转）→ 弹栈销毁本 APP，回 file_browser
 *   书架模式阅读页打开          → 关闭阅读层，root 复位回书架（APP 保留）
 *   书架页（无阅读层）          → 弹栈销毁，回来源/桌面 */
void reader_app_drag_exit(reader_app_t *app)
{
    if (!app) return;
    if (app->rv && reader_view_active(app->rv) && !app->direct_mode) {
        ESP_LOGI("reader_app", "[DRAG-EXIT] shelf-mode reader -> back to shelf");
        reader_view_close(app->rv);   /* 内部已异步保存阅读进度 */
        lv_obj_set_x(app->root, 0);   /* 滑出动画把 root 推到屏外，复位回书架 */
        lv_obj_update_layout(app->root);
        /* 回到书架：刷新进度/收藏状态显示（读 .prog/.favbook） */
        ra_reload_states(app);
        ra_render_list(app);
        ra_update_count(app);
        return;
    }
    ESP_LOGI("reader_app", "[DRAG-EXIT] close app (back to stack source)");
    launcher_app_close(NULL);
}

/* 调试事件（测试模块用）：打印内部状态，验证回调链路 */
void reader_app_debug_event(reader_app_t *app, int evt)
{
    if (!app) return;
    ESP_LOGI("reader_app", "[DBG] evt=%d entries=%d rv_active=%d",
             evt, app->entry_count, app->rv ? reader_view_active(app->rv) : -1);
}
