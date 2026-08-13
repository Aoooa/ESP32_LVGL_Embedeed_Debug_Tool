/* file_browser.c —— SD 卡目录浏览器界面（LVGL 9） */

#include "file_browser.h"
#include "drv_sdcard.h"
#include "app_sdcard.h"
#include "drv_display.h"
#include "flow_view.h"
#include "app_font.h"
#include "misc/lv_timer_private.h"
#include "core/lv_obj_private.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <strings.h>
#include <sys/stat.h>

#define FB_PATH_MAX   128
#define FB_PATH_LABEL_H 28
#define FB_BTN_BAR_H    40
#define FB_ROW_H        34       /* 列表项高度 */
#define FB_ENTRY_MAX    512      /* 单目录条目上限 */
#define FB_ENTRY_NAME_MAX 96
#define FB_READER_MAX    (512 * 1024)   /* txt 阅读文件大小上限 */

/* 阅读界面配色（白底黑字，与浏览器深色区分） */
#define FB_READER_BG          lv_color_hex(0xFFFFFF)
#define FB_READER_TEXT        lv_color_hex(0x111111)
#define FB_READER_BTN_BORDER  lv_color_hex(0xD1D5DB)
#define FB_READER_BTN_BAR_H   44     /* 底部容器高 = 气泡区(24) + 进度条(12) + 边距；透明无视觉影响 */
#define FB_READER_BAR_BOTTOM  6      /* 底部栏距屏幕底边距（便于触摸） */
#define FB_READER_TITLE_H     28     /* 顶部状态栏高度 */

/* 点击屏幕中心区域（约 120x80）才切换按钮显隐 */
/* 屏幕尺寸动态化：跟随 LVGL 逻辑分辨率（横竖屏通用） */
static int fb_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_horizontal_resolution(d) : 320;
}
static int fb_screen_h(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_vertical_resolution(d) : 240;
}

/* 阅读文本行数：canvas 固定全屏行数，按钮栏以覆盖方式遮挡底部文字 */
#define FB_READER_LINES   17   /* 英文 12px 时的全屏行数（17x14=238px）；中文按行高动态重算 */

/* 中文字体（SD /fonts/，缺失时回退英文）；UI 主字体 16px */
static lv_font_t *fb_ui_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static const char *FB_TAG = "file_browser";

/* 列表项 user_data：完整路径 + 类型 */
typedef struct {
    char path[FB_PATH_MAX];
    bool is_dir;
} fb_row_data_t;

/* 配色（深色底：文件夹黄 / 文件白） */
#define FB_BG_COLOR       lv_color_hex(0x111827)
#define FB_ROW_COLOR      lv_color_hex(0x1F2937)
#define FB_ROW_PRESSED    lv_color_hex(0x374151)
#define FB_PATH_COLOR     lv_color_hex(0xFFFFFF)
#define FB_DIR_COLOR      lv_color_hex(0xFBBF24)   /* 文件夹黄色 */
#define FB_FILE_COLOR     lv_color_hex(0xFFFFFF)   /* 文件白色 */
#define FB_EMPTY_COLOR    lv_color_hex(0x9CA3AF)
#define FB_BTN_BG         lv_color_hex(0x1F2937)
#define FB_BTN_TEXT       lv_color_hex(0xE5E7EB)
#define FB_BTN_BORDER     lv_color_hex(0x374151)

/* ── 排序模式（12 种，点击排序按钮循环切换） ── */

typedef struct {
    bool grouped;    /* true=文件夹组+文件组；false=全量混合 */
    bool dir_first;  /* grouped 时：true=文件夹在前 */
    bool by_name;    /* true=按名称；false=按修改日期 */
    bool desc;       /* true=降序 */
} fb_sort_cfg_t;

static const fb_sort_cfg_t s_sort_cfgs[12] = {
    { true,  true,  true,  false },  /* 1  文件夹字母↑ + 文件字母↑ */
    { true,  true,  true,  true  },  /* 2  文件夹字母↓ + 文件字母↓ */
    { true,  false, true,  false },  /* 3  文件字母↑ + 文件夹字母↑ */
    { true,  false, true,  true  },  /* 4  文件字母↓ + 文件夹字母↓ */
    { true,  true,  false, false },  /* 5  文件夹日期↑ + 文件日期↑ */
    { true,  true,  false, true  },  /* 6  文件夹日期↓ + 文件日期↓ */
    { true,  false, false, false },  /* 7  文件日期↑ + 文件夹日期↑ */
    { true,  false, false, true  },  /* 8  文件日期↓ + 文件夹日期↓ */
    { false, true,  false, false },  /* 9  全部按日期↑ */
    { false, true,  false, true  },  /* 10 全部按日期↓ */
    { false, true,  true,  false },  /* 11 全部按字母↑ */
    { false, true,  true,  true  },  /* 12 全部按字母↓ */
};

#define FB_SORT_MAX  (int)(sizeof(s_sort_cfgs) / sizeof(s_sort_cfgs[0]))

/* 条目（枚举收集 + 排序） */
typedef struct {
    char name[FB_ENTRY_NAME_MAX];
    bool is_dir;
    time_t mtime;
} fb_entry_t;

typedef struct {
    lv_obj_t *path_label;
    lv_obj_t *list;
    lv_obj_t *bar;
    lv_obj_t *btn_up;
    lv_obj_t *btn_root;
    lv_obj_t *btn_sort;
    lv_obj_t *lbl_sort;
    char root[FB_PATH_MAX];
    char cur[FB_PATH_MAX];
    int sort_mode;          /* 0..11 */
    fb_entry_t *entries;    /* 枚举收集缓冲（fb_refresh 内分配） */
    int entry_count;
    /* 导航栈：进入子目录前保存父目录滚动位置 */
    struct { char path[FB_PATH_MAX]; int32_t scroll_y; } stack[8];
    int depth;
    int32_t pending_scroll; /* 刷新后需恢复的滚动位置（<0 = 不恢复） */
    /* 阅读界面：全屏覆盖层（只切换本层显隐，不触碰浏览器对象树） */
    lv_obj_t *reader_root;      /* 阅读覆盖层（初始隐藏） */
    lv_obj_t *reader_title;     /* 顶部状态栏：↑ 返回 + 当前 txt 文件名 */
    lv_obj_t *reader_title_label;
    lv_obj_t *bubble;           /* 进度百分比气泡（黄椭圆，飘在进度条头部） */
    lv_obj_t *prog;             /* 自绘进度条（轨道/指示/滑块完全可控） */
    int prog_val;               /* 进度条当前值（行号 0..prog_max） */
    int prog_max;               /* 进度条范围上限 = max_top */
    lv_obj_t *reader_view;      /* flow_view：txt 内容 */
    lv_obj_t *reader_bar;       /* 阅读模式底部栏（进度条 + 气泡） */
    lv_obj_t *btn_reader_up;    /* 返回浏览器 */
    lv_timer_t *progress_timer;
    bool reader_active;
    bool ui_hidden;             /* true=纯阅读（标题栏/底部栏隐藏） */
    bool pending_reader;        /* 待打开的 txt（事件回调置位，定时器执行） */
    char pending_reader_path[FB_PATH_MAX];
    char reader_path[FB_PATH_MAX];  /* 当前打开的 txt（旋转重建用） */
} fb_t;

static fb_t *fb_get(lv_obj_t *obj)
{
    return (lv_obj_t *)obj ? (fb_t *)lv_obj_get_user_data(obj) : NULL;
}

static void fb_item_event(lv_event_t *e);
static void fb_btn_event(lv_event_t *e);
static void fb_reader_btn_event(lv_event_t *e);
static void fb_list_event(lv_event_t *e);
static void fb_open_reader(fb_t *fb, const char *path);
static void fb_close_reader(fb_t *fb);

/* ── 导航 ── */

static bool fb_is_root(const fb_t *fb)
{
    return strcmp(fb->cur, fb->root) == 0;
}

static void fb_go_up(fb_t *fb)
{
    if (fb_is_root(fb)) return;
    char *slash = strrchr(fb->cur, '/');
    if (slash && slash != fb->cur) {
        *slash = '\0';
    } else {
        strncpy(fb->cur, fb->root, sizeof(fb->cur) - 1);
    }
}

/* ── 排序比较器（qsort_r，mode 经 arg 传入） ── */

static int fb_cmp(const void *a, const void *b, void *arg)
{
    const fb_entry_t *ea = a, *eb = b;
    const fb_sort_cfg_t *cfg = &s_sort_cfgs[*(int *)arg];

    int r;
    if (cfg->grouped && ea->is_dir != eb->is_dir) {
        /* 组顺序：dir_first 时文件夹在前 */
        r = (ea->is_dir == cfg->dir_first) ? -1 : 1;
        return r;
    }
    if (cfg->by_name) {
        r = strcasecmp(ea->name, eb->name);
    } else {
        if (ea->mtime < eb->mtime) r = -1;
        else if (ea->mtime > eb->mtime) r = 1;
        else r = strcasecmp(ea->name, eb->name);   /* 同时间按名称，保证确定性 */
    }
    return cfg->desc ? -r : r;
}

/* ── 列表项 ── */

static lv_obj_t *fb_add_row(fb_t *fb, const char *name, bool is_dir)
{
    lv_obj_t *row = lv_button_create(fb->list);
    lv_obj_set_size(row, lv_pct(100), FB_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_bg_color(row, FB_ROW_COLOR, 0);
    lv_obj_set_style_bg_color(row, FB_ROW_PRESSED, LV_STATE_PRESSED);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(icon, is_dir ? FB_DIR_COLOR : FB_FILE_COLOR, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, is_dir ? FB_DIR_COLOR : FB_FILE_COLOR, 0);
    lv_obj_set_style_text_font(lbl, fb_ui_font(), 0);

    /* 存完整路径 + 类型（点击时判断进入文件夹或阅读 txt） */
    fb_row_data_t *rd = malloc(sizeof(fb_row_data_t));
    if (rd) {
        snprintf(rd->path, sizeof(rd->path), "%.100s/%.24s", fb->cur, name);
        rd->is_dir = is_dir;
        lv_obj_set_user_data(row, rd);
    }

    lv_obj_add_event_cb(row, fb_item_event, LV_EVENT_CLICKED, fb);
    return row;
}

/* ── 枚举收集回调 ── */

static void fb_entry_cb(void *ctx, const char *name, bool is_dir, long size, time_t mtime)
{
    fb_t *fb = ctx;
    if (fb->entry_count >= FB_ENTRY_MAX) return;
    fb_entry_t *e = &fb->entries[fb->entry_count++];
    strncpy(e->name, name, FB_ENTRY_NAME_MAX - 1);
    e->name[FB_ENTRY_NAME_MAX - 1] = '\0';
    e->is_dir = is_dir;
    e->mtime = mtime;
    (void)size;
}

/* 刷新当前目录：清空列表 + 重新枚举 + 排序 + 重建 */
static void fb_refresh(fb_t *fb)
{
    lv_obj_clean(fb->list);
    lv_label_set_long_mode(fb->path_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(fb->path_label, fb->cur);
    lv_label_set_text_fmt(fb->lbl_sort, "%s %d", LV_SYMBOL_REFRESH, fb->sort_mode + 1);

    fb->entries = malloc(FB_ENTRY_MAX * sizeof(fb_entry_t));
    if (!fb->entries) {
        lv_obj_t *t = lv_label_create(fb->list);
        lv_label_set_text(t, "(no memory)");
        lv_obj_set_style_text_color(t, FB_EMPTY_COLOR, 0);
        return;
    }
    fb->entry_count = 0;

    esp_err_t err = app_sdcard_list_dir(fb->cur, fb_entry_cb, fb);

    if (err != ESP_OK) {
        lv_obj_t *t = lv_label_create(fb->list);
        lv_label_set_text(t, "SD card not ready");
        lv_obj_set_style_text_color(t, FB_EMPTY_COLOR, 0);
    } else if (fb->entry_count == 0) {
        lv_obj_t *t = lv_label_create(fb->list);
        lv_label_set_text(t, "(empty)");
        lv_obj_set_style_text_color(t, FB_EMPTY_COLOR, 0);
    } else {
        qsort_r(fb->entries, fb->entry_count, sizeof(fb_entry_t), fb_cmp, &fb->sort_mode);
        for (int i = 0; i < fb->entry_count; i++) {
            fb_add_row(fb, fb->entries[i].name, fb->entries[i].is_dir);
        }
    }
    free(fb->entries);
    fb->entries = NULL;
    fb->entry_count = 0;

    if (fb_is_root(fb)) {
        lv_obj_add_state(fb->btn_up, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(fb->btn_up, LV_STATE_DISABLED);
    }

    /* 返回上级时恢复父目录的滚动位置 */
    if (fb->pending_scroll > 0) {
        lv_obj_scroll_to_y(fb->list, fb->pending_scroll, LV_ANIM_OFF);
        fb->pending_scroll = -1;
    }
}

/* 屏幕旋转后重排：list 高度/按钮宽按新屏尺寸重算；
 * 阅读器打开时按新分辨率重建（行数/宽度自适应，滚动位置回到开头） */
void file_browser_relayout(lv_obj_t *obj)
{
    fb_t *fb = fb_get(obj);
    if (!fb) return;

    /* 强制布局：set_resolution 后 pct(100%) 尺寸惰性未刷新，父高/子宽需先更新 */
    lv_obj_update_layout(obj);

    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (parent) {
        int list_h = lv_obj_get_height(parent) - FB_PATH_LABEL_H - FB_BTN_BAR_H;
        lv_obj_set_size(fb->list, lv_pct(100), list_h);
    }
    int btn_w = (fb_screen_w() - 6 * 2 - 6 * 2) / 3;
    lv_obj_set_size(fb->btn_up, btn_w, 28);
    lv_obj_set_size(fb->btn_root, btn_w, 28);
    lv_obj_set_size(fb->btn_sort, btn_w, 28);

    if (fb->reader_active && fb->reader_path[0]) {
        fb_close_reader(fb);
        fb_open_reader(fb, fb->reader_path);
    }
}

/* ── 阅读界面（txt） ── */

/* 气泡定位：飘在进度条头部（knob）上方（bar 容器内，随 bar 动画同步）。
 * 进度条值为行号，换算成百分比定位；y 固定 = 进度条上方 4px（bar 内） */
static void fb_position_bubble(fb_t *fb)
{
    if (!fb->prog || !fb->bubble) return;
    int sw = lv_obj_get_width(fb->prog);
    int sh = lv_obj_get_height(fb->prog);
    int bw = lv_obj_get_width(fb->bubble);
    int sx = lv_obj_get_x(fb->prog);
    int max = fb->prog_max > 0 ? fb->prog_max : 1;
    int pct = fb->prog_val * 100 / max;
    if (pct > 100) pct = 100;
    int x = sx + pct * sw / 100 - bw / 2;
    if (x < sx) x = sx;
    if (x > sx + sw - bw) x = sx + sw - bw;
    /* align 立即更新 coords（set_x/y 只改 style，IGNORE_LAYOUT 对象布局跳过不更新） */
    lv_obj_align(fb->bubble, LV_ALIGN_TOP_LEFT, x, FB_READER_BTN_BAR_H - sh - lv_obj_get_height(fb->bubble) - 4);
}

/* 自绘进度条：轨道/指示/滑块完全可控。
 * 关键设计：0% 时滑块圆与轨道端半圆同心重合；指示器从轨道左端开始，
 * 拖动后 0% 位置整体变深灰（无 lv_slider 的 pad 浅灰缺陷） */
#define FB_PROG_TRACK  lv_color_hex(0x9CA3AF)
#define FB_PROG_INDIC  lv_color_hex(0x6B7280)
#define FB_PROG_KNOB   lv_color_hex(0x374151)

static void fb_prog_draw(lv_event_t *e)
{
    fb_t *fb = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!fb) return;
    int w = lv_obj_get_width(obj);
    int h = lv_obj_get_height(obj);
    int r = h / 2;
    int track_w = w - h;   /* 两端半圆区域 */

    lv_area_t a = obj->coords;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = r;

    /* 轨道 */
    dsc.bg_color = FB_PROG_TRACK;
    lv_draw_rect(layer, &dsc, &a);

    /* 指示器：从轨道左端（含半圆）到滑块中心 */
    int max = fb->prog_max > 0 ? fb->prog_max : 1;
    int v = fb->prog_val;
    if (v < 0) v = 0;
    if (v > max) v = max;
    int ind_w = r + track_w * v / max;
    if (v > 0) {
        a.x2 = a.x1 + ind_w - 1;
        dsc.bg_color = FB_PROG_INDIC;
        lv_draw_rect(layer, &dsc, &a);
    }

    /* 滑块：中心 = 指示端点（0% 时与轨道端半圆同心重合） */
    int cx = a.x1 + r + track_w * v / max;
    lv_area_t k = { cx - r, a.y1, cx + r - 1, a.y2 };
    dsc.bg_color = FB_PROG_KNOB;
    dsc.radius = r;
    lv_draw_rect(layer, &dsc, &k);
}

/* 进度条触摸：按下/拖动直接跳转到触摸位置（行号） */
static void fb_prog_event(lv_event_t *e)
{
    fb_t *fb = lv_event_get_user_data(e);
    if (!fb || !fb->reader_active) return;
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_PRESSED && c != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_t *obj = lv_event_get_target(e);
    int w = lv_obj_get_width(obj);
    int h = lv_obj_get_height(obj);
    int max = fb->prog_max > 0 ? fb->prog_max : 1;
    int v = (p.x - obj->coords.x1 - h / 2) * max / (w - h);
    if (v < 0) v = 0;
    if (v > max) v = max;
    fb->prog_val = v;
    flow_view_go_to(fb->reader_view, v);
    fb_position_bubble(fb);
    lv_obj_invalidate(obj);
}

static void fb_update_progress(fb_t *fb)
{
    int max = flow_view_get_max_top(fb->reader_view);
    if (max <= 0) {
        lv_label_set_text(fb->bubble, "100%");
        fb->prog_max = 1;
        fb->prog_val = 0;
        if (fb->prog) lv_obj_invalidate(fb->prog);
        fb_position_bubble(fb);
        return;
    }
    int top = flow_view_get_view_top(fb->reader_view);
    int pct = top * 100 / max;
    if (pct > 100) pct = 100;
    lv_label_set_text_fmt(fb->bubble, "%d%%", pct);
    fb->prog_max = max;
    fb->prog_val = top;
    if (fb->prog) lv_obj_invalidate(fb->prog);
    fb_position_bubble(fb);
}

static void fb_progress_timer(lv_timer_t *t)
{
    fb_t *fb = t->user_data;
    if (fb->reader_active) fb_update_progress(fb);
}

/* 判断是否为 .txt（大小写不敏感） */
static bool fb_is_txt(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    return strcasecmp(name + n - 4, ".txt") == 0;
}

static void fb_open_reader(fb_t *fb, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > FB_READER_MAX) {
        ESP_LOGW(FB_TAG, "open reader: stat fail or size %lld out of range", (long long)st.st_size);
        return;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        ESP_LOGW(FB_TAG, "open reader: malloc %lld failed", (long long)st.st_size);
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(FB_TAG, "open reader: fopen failed");
        free(buf);
        return;
    }
    size_t rd = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[rd] = '\0';

    fb->reader_active = true;
    strncpy(fb->reader_path, path, sizeof(fb->reader_path) - 1);

    fb->ui_hidden = false;   /* 默认打开上下栏 */

    /* 只切换阅读覆盖层，不触碰浏览器对象树；canvas 固定全屏行数 */
    lv_obj_clear_flag(fb->reader_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fb->reader_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fb->reader_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fb->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(fb->btn_reader_up, LV_STATE_DISABLED);

    /* 顶部状态栏：当前文件名（含后缀） */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    lv_label_set_text(fb->reader_title_label, base);

    /* 阅读字体：中文字体（缺失回退英文）；行数向上取整铺满全屏（无底部空白） */
    lv_font_t *cn = app_font_get(16);
    lv_font_t *rf = cn ? cn : &lv_font_montserrat_12;
    flow_view_set_font(fb->reader_view, rf);
    /* 底部栏按当前屏高重新定位（旋转后屏高变化） */
    lv_obj_set_pos(fb->reader_bar, 0, fb_screen_h() - FB_READER_BTN_BAR_H - FB_READER_BAR_BOTTOM);
    int lines = (fb_screen_h() + lv_font_get_line_height(rf) - 1) / lv_font_get_line_height(rf);
    flow_view_set_visible_lines(fb->reader_view, lines);

    flow_view_load_text(fb->reader_view, buf);
    flow_view_set_follow(fb->reader_view, false);
    flow_view_go_to(fb->reader_view, 0);
    free(buf);
    /* 进度条范围 = 行号（0..max_top），与实际阅读位置一一对应 */
    int max = flow_view_get_max_top(fb->reader_view);
    fb->prog_max = max > 0 ? max : 1;
    fb->prog_val = 0;
    /* 强制布局：prog 宽度为百分比(lv_pct)，未布局前宽度为 0，
     * 气泡按 0 宽定位会错位到滑动条末尾 */
    lv_obj_update_layout(fb->reader_root);
    fb_update_progress(fb);

}

/* 阅读器关闭后的列表刷新（延迟一帧执行，避免与隐藏同帧重入导致撕裂） */
static void fb_close_reader_refresh(lv_timer_t *t)
{
    fb_t *fb = t->user_data;
    if (fb) fb_refresh(fb);
}

static void fb_close_reader(fb_t *fb)
{
    fb->reader_active = false;
    lv_obj_add_flag(fb->reader_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fb->reader_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fb->reader_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fb->bubble, LV_OBJ_FLAG_HIDDEN);
    /* 列表重建延迟到下一帧 LVGL 循环，避开事件回调上下文 */
    lv_timer_t *t = lv_timer_create(fb_close_reader_refresh, 1, fb);
    lv_timer_set_repeat_count(t, 1);
    lv_timer_set_auto_delete(t, true);
}

/* 点击屏幕中心区域：切换标题栏/底部栏显隐（带上下滑入滑出动画） */
static void fb_chrome_ready_cb(lv_anim_t *a);
static void fb_chrome_anim(fb_t *fb, bool show)
{
    if (show) {
        lv_obj_clear_flag(fb->reader_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(fb->reader_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(fb->bubble, LV_OBJ_FLAG_HIDDEN);
        fb_position_bubble(fb);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_time(&a, 180);
    lv_anim_set_user_data(&a, fb);
    /* 顶部状态栏：上方滑入/滑出 */
    lv_anim_set_var(&a, fb->reader_title);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, show ? -FB_READER_TITLE_H : 0,
                       show ? 0 : -FB_READER_TITLE_H);
    lv_anim_start(&a);
    /* 底部栏：下方滑入/滑出 */
    lv_anim_set_var(&a, fb->reader_bar);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    int sh = fb_screen_h();
    lv_anim_set_values(&a, show ? sh : sh - FB_READER_BTN_BAR_H - FB_READER_BAR_BOTTOM,
                       show ? sh - FB_READER_BTN_BAR_H - FB_READER_BAR_BOTTOM : sh);
    if (!show) lv_anim_set_ready_cb(&a, fb_chrome_ready_cb);
    lv_anim_start(&a);
}

static void fb_chrome_ready_cb(lv_anim_t *a)
{
    fb_t *fb = a->user_data;
    if (fb) {
        lv_obj_add_flag(fb->reader_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fb->reader_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fb->bubble, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fb_reader_tap_event(void *user_data, lv_point_t pos)
{
    fb_t *fb = user_data;
    if (!fb || !fb->reader_active) return;
    if (pos.x < fb_screen_w() / 3 || pos.x > fb_screen_w() * 2 / 3 ||
        pos.y < fb_screen_h() / 3 || pos.y > fb_screen_h() * 2 / 3) {
        return;   /* 仅中心区域触发 */
    }
    fb->ui_hidden = !fb->ui_hidden;
    fb_chrome_anim(fb, !fb->ui_hidden);
}

static void fb_reader_tap_event(void *user_data, lv_point_t pos);

/* 延迟打开阅读（一次性定时器，下一帧 LVGL 循环执行，避开 indev 事件上下文） */
static void fb_open_reader_deferred(lv_timer_t *t)
{
    fb_t *fb = t->user_data;
    if (!fb->pending_reader) return;
    fb->pending_reader = false;
    fb_open_reader(fb, fb->pending_reader_path);
}

/* ── 事件 ── */

/* 列表项点击：文件夹进入下级；.txt 打开阅读；其他文件无操作 */
static void fb_item_event(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    fb_t *fb = (fb_t *)lv_event_get_user_data(e);   /* 注册时传入的是 fb_t* */
    if (!fb || !row) return;

    fb_row_data_t *rd = lv_obj_get_user_data(row);
    if (!rd) return;

    if (rd->is_dir) {
        /* 保存父目录的滚动位置（返回时恢复） */
        if (fb->depth < (int)(sizeof(fb->stack) / sizeof(fb->stack[0]))) {
            strncpy(fb->stack[fb->depth].path, fb->cur, sizeof(fb->stack[fb->depth].path) - 1);
            fb->stack[fb->depth].scroll_y = lv_obj_get_scroll_y(fb->list);
            fb->depth++;
        }
        strncpy(fb->cur, rd->path, sizeof(fb->cur) - 1);
        fb->cur[sizeof(fb->cur) - 1] = '\0';
        free(rd);
        lv_obj_set_user_data(row, NULL);
        fb->pending_scroll = -1;   /* 子目录从头开始 */
        fb_refresh(fb);
    } else if (fb_is_txt(rd->path)) {
        /* 保存浏览器滚动位置（返回阅读器时恢复），再延迟打开（避开 indev 事件上下文） */
        fb->pending_scroll = lv_obj_get_scroll_y(fb->list);
        strncpy(fb->pending_reader_path, rd->path, sizeof(fb->pending_reader_path) - 1);
        fb->pending_reader_path[sizeof(fb->pending_reader_path) - 1] = '\0';
        fb->pending_reader = true;
        lv_timer_t *t = lv_timer_create(fb_open_reader_deferred, 1, fb);
        lv_timer_set_repeat_count(t, 1);
        lv_timer_set_auto_delete(t, true);
        free(rd);
        lv_obj_set_user_data(row, NULL);
    } else {
        free(rd);   /* 非 txt 文件：无操作 */
        lv_obj_set_user_data(row, NULL);
    }
}

/* 底部按钮：上级 / 根目录 / 排序（浏览器模式） */
static void fb_btn_event(lv_event_t *e)
{
    fb_t *fb = fb_get(lv_event_get_user_data(e));
    if (!fb || lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    lv_obj_t *t = lv_event_get_target_obj(e);

    if (fb->reader_active) {
        /* 阅读模式：↑=返回浏览器，⌂=回根目录（浏览器按钮被覆盖层遮挡，防御处理） */
        if (t == fb->btn_up) {
            fb_close_reader(fb);
        } else if (t == fb->btn_root) {
            fb_close_reader(fb);
            fb->depth = 0;
            fb->pending_scroll = -1;
            strncpy(fb->cur, fb->root, sizeof(fb->cur) - 1);
            fb_refresh(fb);
        }
        return;
    }

    if (t == fb->btn_up) {
        /* 返回上一级：恢复进入前的位置 */
        if (fb->depth > 0) {
            fb->depth--;
            fb->pending_scroll = fb->stack[fb->depth].scroll_y;
        } else {
            fb->pending_scroll = -1;
        }
        fb_go_up(fb);
    } else if (t == fb->btn_root) {
        /* 返回根目录：清空导航栈，回顶部 */
        fb->depth = 0;
        fb->pending_scroll = -1;
        strncpy(fb->cur, fb->root, sizeof(fb->cur) - 1);
    } else if (t == fb->btn_sort) {
        /* 循环切换排序模式 */
        fb->sort_mode = (fb->sort_mode + 1) % FB_SORT_MAX;
    }
    fb_refresh(fb);
}

/* 阅读模式顶部状态栏按钮：↑=返回浏览器（文件所在目录） */
static void fb_reader_btn_event(lv_event_t *e)
{
    fb_t *fb = fb_get(lv_event_get_user_data(e));
    if (!fb || !fb->reader_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    fb_close_reader(fb);
}

/* 滑动滚动时清除列表项的按下态（避免滑动误触发选择动画） */
static void fb_list_event(lv_event_t *e)
{
    fb_t *fb = fb_get(lv_event_get_user_data(e));
    if (!fb || lv_event_get_code(e) != LV_EVENT_SCROLL_BEGIN) return;
    uint32_t n = lv_obj_get_child_count(fb->list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_clear_state(lv_obj_get_child(fb->list, i), LV_STATE_PRESSED);
    }
}

/* ── 创建 ── */

lv_obj_t *file_browser_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(obj, FB_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    fb_t *fb = calloc(1, sizeof(fb_t));
    if (!fb) return obj;
    strncpy(fb->root, DRV_SDCARD_MOUNT_POINT, sizeof(fb->root) - 1);
    strncpy(fb->cur, fb->root, sizeof(fb->cur) - 1);
    fb->pending_scroll = -1;
    lv_obj_set_user_data(obj, fb);

    /* 顶部路径 */
    fb->path_label = lv_label_create(obj);
    lv_obj_set_size(fb->path_label, lv_pct(100), FB_PATH_LABEL_H);
    lv_obj_align(fb->path_label, LV_ALIGN_TOP_LEFT, 8, 0);
    lv_obj_set_style_text_color(fb->path_label, FB_PATH_COLOR, 0);
    lv_obj_set_style_bg_color(fb->path_label, FB_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(fb->path_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(fb->path_label, 5, 0);
    lv_obj_set_style_text_font(fb->path_label, fb_ui_font(), 0);

    /* 中部列表 */
    int list_h = lv_obj_get_height(parent) - FB_PATH_LABEL_H - FB_BTN_BAR_H;
    fb->list = lv_list_create(obj);
    lv_obj_set_size(fb->list, lv_pct(100), list_h);
    lv_obj_align(fb->list, LV_ALIGN_TOP_LEFT, 0, FB_PATH_LABEL_H);
    lv_obj_set_style_bg_color(fb->list, FB_BG_COLOR, 0);
    lv_obj_set_style_border_width(fb->list, 0, 0);
    lv_obj_set_style_radius(fb->list, 0, 0);
    lv_obj_set_style_pad_all(fb->list, 0, 0);
    lv_obj_set_style_pad_row(fb->list, 2, 0);
    lv_obj_set_style_text_color(fb->list, FB_FILE_COLOR, 0);
    lv_obj_add_event_cb(fb->list, fb_list_event, LV_EVENT_SCROLL_BEGIN, obj);

    /* 底部按钮栏 */
    lv_obj_t *bar = lv_obj_create(obj);
    lv_obj_set_size(bar, lv_pct(100), FB_BTN_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, FB_BG_COLOR, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_set_style_pad_gap(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    fb->btn_up = lv_button_create(bar);
    fb->btn_root = lv_button_create(bar);
    fb->btn_sort = lv_button_create(bar);
    lv_obj_t *lbl_up = lv_label_create(fb->btn_up);
    lv_label_set_text(lbl_up, LV_SYMBOL_UP);
    lv_obj_t *lbl_root = lv_label_create(fb->btn_root);
    lv_label_set_text(lbl_root, LV_SYMBOL_HOME);
    fb->lbl_sort = lv_label_create(fb->btn_sort);
    lv_label_set_text_fmt(fb->lbl_sort, "%s 1", LV_SYMBOL_REFRESH);

    lv_obj_t *btns[3] = { fb->btn_up, fb->btn_root, fb->btn_sort };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_size(btns[i], (fb_screen_w() - 6 * 2 - 6 * 2) / 3, 28);   /* 屏宽自适应均分（横竖屏通用） */
        lv_obj_set_style_bg_color(btns[i], FB_BTN_BG, 0);
        lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btns[i], FB_BTN_BORDER, 0);
        lv_obj_set_style_border_width(btns[i], 1, 0);
        lv_obj_set_style_radius(btns[i], 6, 0);
        lv_obj_set_style_text_color(btns[i], FB_BTN_TEXT, 0);
        lv_obj_add_event_cb(btns[i], fb_btn_event, LV_EVENT_CLICKED, obj);
    }

    /* ── 阅读界面（全屏覆盖层，白底黑字；初始隐藏；只切换本层显隐） ── */
    fb->reader_root = lv_obj_create(obj);
    lv_obj_set_size(fb->reader_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(fb->reader_root, FB_READER_BG, 0);
    lv_obj_set_style_bg_opa(fb->reader_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fb->reader_root, 0, 0);
    lv_obj_set_style_radius(fb->reader_root, 0, 0);
    lv_obj_set_style_pad_all(fb->reader_root, 0, 0);
    lv_obj_clear_flag(fb->reader_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(fb->reader_root, LV_OBJ_FLAG_HIDDEN);

    /* 阅读内容（flow_view：整段文本 + 触摸滚动，白底黑字；尺寸由行数决定） */
    fb->reader_view = flow_view_create(fb->reader_root);
    lv_obj_set_pos(fb->reader_view, 0, 0);
    lv_obj_set_style_bg_color(fb->reader_view, FB_READER_BG, 0);
    flow_view_set_color(fb->reader_view, FB_READER_TEXT);
    flow_view_set_follow(fb->reader_view, false);
    flow_view_set_visible_lines(fb->reader_view, FB_READER_LINES);
    /* 点击屏幕中心区域：切换标题栏/底部栏显隐（组件内点击回调，滚动不触发） */
    flow_view_set_clicked_cb(fb->reader_view, fb_reader_tap_event, fb);

    /* 顶部状态栏：↑ 返回按钮（左上角，紧贴边缘）+ 当前 txt 文件名（居中）；
     * 初始隐藏，点击中心弹出；底部一分界线。用 set_pos 而非 align：
     * align 会注册 style_align，布局刷新时按父高重算 y，与滑入动画冲突 */
    fb->reader_title = lv_obj_create(fb->reader_root);
    lv_obj_set_size(fb->reader_title, lv_pct(100), 28);
    lv_obj_set_pos(fb->reader_title, 0, 0);
    lv_obj_set_style_bg_color(fb->reader_title, FB_READER_BG, 0);
    lv_obj_set_style_bg_opa(fb->reader_title, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fb->reader_title, 1, 0);
    lv_obj_set_style_border_side(fb->reader_title, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(fb->reader_title, FB_READER_BTN_BORDER, 0);
    lv_obj_set_style_radius(fb->reader_title, 0, 0);
    lv_obj_set_style_pad_hor(fb->reader_title, 0, 0);
    lv_obj_set_style_pad_ver(fb->reader_title, 0, 0);
    lv_obj_set_style_pad_gap(fb->reader_title, 0, 0);
    lv_obj_set_flex_flow(fb->reader_title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fb->reader_title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(fb->reader_title, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(fb->reader_title, LV_OBJ_FLAG_HIDDEN);

    fb->btn_reader_up = lv_button_create(fb->reader_title);
    lv_obj_set_size(fb->btn_reader_up, 28, 28);
    lv_obj_set_ext_click_area(fb->btn_reader_up, 30);   /* 触摸区扩大（屏幕边缘按钮，手指中心偏右点不中） */
    lv_obj_set_flex_flow(fb->btn_reader_up, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fb->btn_reader_up, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(fb->btn_reader_up, 0, 0);
    lv_obj_set_style_bg_color(fb->btn_reader_up, FB_READER_BG, 0);
    lv_obj_set_style_bg_opa(fb->btn_reader_up, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(fb->btn_reader_up, FB_READER_BTN_BORDER, 0);
    lv_obj_set_style_border_width(fb->btn_reader_up, 1, 0);
    lv_obj_set_style_radius(fb->btn_reader_up, 6, 0);
    lv_obj_set_style_text_color(fb->btn_reader_up, FB_READER_TEXT, 0);
    lv_obj_add_event_cb(fb->btn_reader_up, fb_reader_btn_event, LV_EVENT_CLICKED, obj);
    lv_obj_t *lbl_r_up = lv_label_create(fb->btn_reader_up);
    lv_label_set_text(lbl_r_up, LV_SYMBOL_LEFT);

    fb->reader_title_label = lv_label_create(fb->reader_title);
    lv_obj_set_flex_grow(fb->reader_title_label, 1);
    lv_obj_set_height(fb->reader_title_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(fb->reader_title_label, FB_READER_TEXT, 0);
    lv_obj_set_style_text_font(fb->reader_title_label, fb_ui_font(), 0);
    lv_obj_set_style_text_align(fb->reader_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(fb->reader_title_label, "");

    /* 阅读模式底部栏（初始隐藏，点击中心弹出）：
     * 无背景悬浮：透明 + 无分界线，进度条悬浮于文字之上，文字透出。
     * LVGL 9 lv_obj 默认裁剪子对象：必须加 OVERFLOW_VISIBLE，
     * 否则气泡（负 y 在容器上方）和滑块超出部分被裁掉。
     * set_pos 而非 align（防布局重算与滑入动画冲突） */
    fb->reader_bar = lv_obj_create(fb->reader_root);
    lv_obj_set_size(fb->reader_bar, lv_pct(100), FB_READER_BTN_BAR_H);
    lv_obj_set_pos(fb->reader_bar, 0, fb_screen_h() - FB_READER_BTN_BAR_H - FB_READER_BAR_BOTTOM);
    lv_obj_add_flag(fb->reader_bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(fb->reader_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fb->reader_bar, 0, 0);
    lv_obj_set_style_radius(fb->reader_bar, 0, 0);
    lv_obj_set_style_pad_all(fb->reader_bar, 0, 0);
    lv_obj_set_style_pad_gap(fb->reader_bar, 0, 0);
    lv_obj_set_flex_flow(fb->reader_bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(fb->reader_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(fb->reader_bar, LV_OBJ_FLAG_HIDDEN);

    /* 百分比气泡：黄色椭圆，飘在进度条头部（knob）上方。
     * 放在透明 bar 容器内，随 bar 滑入滑出动画同步移动。
     * 必须 IGNORE_LAYOUT：bar 是 flex 容器，否则气泡 x/y 被 flex 布局接管覆盖 */
    fb->bubble = lv_label_create(fb->reader_bar);
    lv_obj_add_flag(fb->bubble, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_color(fb->bubble, lv_color_hex(0xFBBF24), 0);
    lv_obj_set_style_bg_opa(fb->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fb->bubble, 9, 0);
    lv_obj_set_style_text_color(fb->bubble, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(fb->bubble, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_hor(fb->bubble, 6, 0);
    lv_obj_set_style_pad_ver(fb->bubble, 1, 0);
    lv_obj_add_flag(fb->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(fb->bubble, "0%");

    /* 自绘进度条：轨道/指示/滑块完全可控（0% 重合 + 指示器满端），
     * 悬浮于 bar 底部，60% 半透明，触摸区上下扩展 */
    fb->prog = lv_obj_create(fb->reader_bar);
    lv_obj_set_size(fb->prog, lv_pct(88), 12);
    lv_obj_add_flag(fb->prog, LV_OBJ_FLAG_IGNORE_LAYOUT);   /* 不参与 flex，位置对齐控制 */
    lv_obj_align(fb->prog, LV_ALIGN_BOTTOM_MID, 0, 0);      /* bar 内底部居中（无动画，align 安全） */
    lv_obj_set_style_opa(fb->prog, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(fb->prog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fb->prog, 0, 0);
    lv_obj_set_style_radius(fb->prog, 0, 0);
    lv_obj_set_style_pad_all(fb->prog, 0, 0);
    lv_obj_set_ext_click_area(fb->prog, 10);   /* 触摸命中区上下各扩 10px，视觉不变 */
    lv_obj_add_event_cb(fb->prog, fb_prog_draw, LV_EVENT_DRAW_MAIN, fb);
    lv_obj_add_event_cb(fb->prog, fb_prog_event, LV_EVENT_ALL, fb);

    fb->progress_timer = lv_timer_create(fb_progress_timer, 500, fb);

    fb_refresh(fb);
    return obj;
}

void file_browser_set_root(lv_obj_t *obj, const char *root_path)
{
    fb_t *fb = fb_get(obj);
    if (!fb || !root_path) return;
    strncpy(fb->root, root_path, sizeof(fb->root) - 1);
    fb->root[sizeof(fb->root) - 1] = '\0';
    strncpy(fb->cur, fb->root, sizeof(fb->cur) - 1);
    fb_refresh(fb);
}

const char *file_browser_get_current_path(lv_obj_t *obj)
{
    fb_t *fb = fb_get(obj);
    return fb ? fb->cur : "";
}

void file_browser_refresh(lv_obj_t *obj)
{
    fb_t *fb = fb_get(obj);
    if (fb) fb_refresh(fb);
}
