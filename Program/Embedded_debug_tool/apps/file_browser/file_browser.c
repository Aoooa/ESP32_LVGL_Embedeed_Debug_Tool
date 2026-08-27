/* file_browser.c —�?SD 卡目录浏览器界面（LVGL 9�?*/

#include "file_browser.h"
#include "drv_sdcard.h"
#include "app_sdcard.h"
#include "drv_display.h"
#include "launcher.h"
#include "flow_view.h"
#include "app_font.h"
#include "epub.h"
#include "misc/lv_timer_private.h"
#include "core/lv_obj_private.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <strings.h>

#define FB_PATH_MAX   128
#define FB_PATH_LABEL_H 28
#define FB_BTN_BAR_H    40
#define FB_ROW_H        34       /* 列表项高�?*/
#define FB_ENTRY_MAX    512      /* 单目录条目上�?*/
#define FB_ENTRY_NAME_MAX 96

/* 屏幕尺寸动态化：跟�?LVGL 逻辑分辨率（横竖屏通用�?*/
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

/* 中文字体（SD /fonts/，缺失时回退英文）；UI 主字�?16px */
static lv_font_t *fb_ui_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static const char *FB_TAG = "file_browser";

/* 列表�?user_data：完整路�?+ 类型 */
typedef struct {
    char path[FB_PATH_MAX + 128];   /* 完整路径（为长文件名/目录名留余量�?*/
    bool is_dir;
} fb_row_data_t;

/* 配色（深色底：文件夹�?/ 文件白） */
#define FB_BG_COLOR       lv_color_hex(0x000000)
#define FB_ROW_COLOR      lv_color_hex(0x000000)
#define FB_ROW_PRESSED    lv_color_hex(0x374151)
#define FB_PATH_COLOR     lv_color_hex(0xFFFFFF)
#define FB_DIR_COLOR      lv_color_hex(0xFBBF24)   /* 文件夹黄�?*/
#define FB_FILE_COLOR     lv_color_hex(0xFFFFFF)   /* 文件白色 */
#define FB_EMPTY_COLOR    lv_color_hex(0x9CA3AF)
#define FB_BTN_BG         lv_color_hex(0x000000)
#define FB_BTN_TEXT       lv_color_hex(0xE5E7EB)
#define FB_BTN_BORDER     lv_color_hex(0x374151)

/* ── 排序模式�?2 种，点击排序按钮循环切换�?── */

typedef struct {
    bool grouped;    /* true=文件夹组+文件组；false=全量混合 */
    bool dir_first;  /* grouped 时：true=文件夹在�?*/
    bool by_name;    /* true=按名称；false=按修改日�?*/
    bool desc;       /* true=降序 */
} fb_sort_cfg_t;

static const fb_sort_cfg_t s_sort_cfgs[12] = {
    { true,  true,  true,  false },  /* 1  文件夹字母↑ + 文件字母�?*/
    { true,  true,  true,  true  },  /* 2  文件夹字母↓ + 文件字母�?*/
    { true,  false, true,  false },  /* 3  文件字母�?+ 文件夹字母↑ */
    { true,  false, true,  true  },  /* 4  文件字母�?+ 文件夹字母↓ */
    { true,  true,  false, false },  /* 5  文件夹日期↑ + 文件日期�?*/
    { true,  true,  false, true  },  /* 6  文件夹日期↓ + 文件日期�?*/
    { true,  false, false, false },  /* 7  文件日期�?+ 文件夹日期↑ */
    { true,  false, false, true  },  /* 8  文件日期�?+ 文件夹日期↓ */
    { false, true,  false, false },  /* 9  全部按日期↑ */
    { false, true,  false, true  },  /* 10 全部按日期↓ */
    { false, true,  true,  false },  /* 11 全部按字母↑ */
    { false, true,  true,  true  },  /* 12 全部按字母↓ */
};

#define FB_SORT_MAX  (int)(sizeof(s_sort_cfgs) / sizeof(s_sort_cfgs[0]))

/* 条目（枚举收�?+ 排序�?*/
typedef struct {
    char name[FB_ENTRY_NAME_MAX];
    bool is_dir;
    time_t mtime;
} fb_entry_t;

typedef struct {
    lv_obj_t *path_label;
    lv_obj_t *list;
    lv_obj_t *bar;
    lv_obj_t *btn_root;
    lv_obj_t *btn_sort;
    lv_obj_t *lbl_sort;
    char root[FB_PATH_MAX];
    char cur[FB_PATH_MAX];
    int sort_mode;          /* 0..11 */
    fb_entry_t *entries;    /* 枚举收集缓冲（fb_refresh 内分配） */
    int entry_count;
    /* 导航栈：进入子目录前保存父目录滚动位�?*/
    struct { char path[FB_PATH_MAX]; int32_t scroll_y; } stack[8];
    int depth;
    int32_t pending_scroll; /* 刷新后需恢复的滚动位置（<0 = 不恢复） */
    file_browser_back_cb_t back_cb;   /* APP 模式返回回调（NULL=不显示按钮） */
    bool pending_reader;        /* 待跳转 txt（事件回调置位，定时器执行） */
    char pending_reader_path[FB_PATH_MAX];
    void *back_ctx;
    lv_timer_t *defer_timer;    /* 延迟打开定时器（destroy 时取消） */
} fb_t;

static fb_t *fb_get(lv_obj_t *obj)
{
    return (lv_obj_t *)obj ? (fb_t *)lv_obj_get_user_data(obj) : NULL;
}

static void fb_item_event(lv_event_t *e);
static void fb_btn_event(lv_event_t *e);
static void fb_list_event(lv_event_t *e);
static void fb_refresh(fb_t *fb);

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

/* 返回上一级目录（含进入前的滚动位置恢复），供右滑手势共用 */
static void fb_back_up(fb_t *fb)
{
    if (fb_is_root(fb)) return;
    if (fb->depth > 0) {
        fb->depth--;
        fb->pending_scroll = fb->stack[fb->depth].scroll_y;
    } else {
        fb->pending_scroll = -1;
    }
    fb_go_up(fb);
    fb_refresh(fb);
}

/* ── 排序比较器（qsort_r，mode �?arg 传入�?── */

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
        else r = strcasecmp(ea->name, eb->name);   /* 同时间按名称，保证确定�?*/
    }
    return cfg->desc ? -r : r;
}

/* ── 列表�?── */

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

    /* 存完整路�?+ 类型（点击时判断进入文件夹或阅读 txt�?*/
    fb_row_data_t *rd = malloc(sizeof(fb_row_data_t));
    if (rd) {
        snprintf(rd->path, sizeof(rd->path), "%s/%s", fb->cur, name);
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

/* 释放列表行数据（lv_obj_clean 不会释放 user_data）后清空列表 */
static void fb_free_rows(lv_obj_t *list)
{
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        fb_row_data_t *rd = lv_obj_get_user_data(child);
        if (rd) {
            free(rd);
            lv_obj_set_user_data(child, NULL);
        }
    }
    lv_obj_clean(list);
}

/* 刷新当前目录：清空列�?+ 重新枚举 + 排序 + 重建 */
static void fb_refresh(fb_t *fb)
{
    fb_free_rows(fb->list);
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

    /* 返回上级时恢复父目录的滚动位�?*/
    if (fb->pending_scroll > 0) {
        lv_obj_scroll_to_y(fb->list, fb->pending_scroll, LV_ANIM_OFF);
        fb->pending_scroll = -1;
    }
}

/* 屏幕旋转后重排：list 高度/按钮宽按新屏尺寸重算�?
 * 阅读器打开时按新分辨率重建（行�?宽度自适应，滚动位置回到开头） */
void file_browser_relayout(lv_obj_t *obj)
{
    fb_t *fb = fb_get(obj);
    if (!fb) return;

    /* 强制布局：set_resolution �?pct(100%) 尺寸惰性未刷新，父�?子宽需先更�?*/
    lv_obj_update_layout(obj);

    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (parent) {
        int list_h = lv_obj_get_height(parent) - FB_PATH_LABEL_H - FB_BTN_BAR_H;
        lv_obj_set_size(fb->list, lv_pct(100), list_h);
    }
    int btn_w = (fb_screen_w() - 6 * 2 - 6 * 2) / 2;
    lv_obj_set_size(fb->btn_root, btn_w, 28);
    lv_obj_set_size(fb->btn_sort, btn_w, 28);
}


/* 判断是否是 .txt（大小写不敏感） */
static bool fb_is_txt(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    return strcasecmp(name + n - 4, ".txt") == 0;
}

/* 判断是否是 .epub */
static bool fb_is_epub(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) return false;
    return strcasecmp(name + n - 5, ".epub") == 0;
}

/* 判断是否是图片（.jpg/.jpeg/.png/.bmp） */
static bool fb_is_image(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    const char *ext = name + n - 4;
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".png") == 0 ||
        strcasecmp(ext, ".bmp") == 0) return true;
    return n >= 5 && strcasecmp(name + n - 5, ".jpeg") == 0;
}

/* 延迟跳转阅读器 APP（一次性定时器，下一帧 LVGL 循环执行，避开 indev 事件上下文） */
static void fb_open_reader_deferred(lv_timer_t *t)
{
    fb_t *fb = t->user_data;
    fb->defer_timer = NULL;
    if (!fb || !fb->pending_reader) return;
    fb->pending_reader = false;
    launcher_app_launch(LAUNCH_APP_READER, fb->pending_reader_path);  /* 压栈跳转，直接打开 txt */
}

/* 延迟打开图片查看器（同机制，压栈保活浏览器） */
static void fb_open_image_deferred(lv_timer_t *t)
{
    fb_t *fb = t->user_data;
    fb->defer_timer = NULL;
    if (!fb || !fb->pending_reader) return;
    fb->pending_reader = false;
    launcher_app_launch(LAUNCH_APP_IMAGEVIEWER, fb->pending_reader_path);
}

/* EPUB：转换到同目录 <name>.txt 后打开（复用 Reader；转换在持锁线程） */
static void fb_open_epub_deferred(lv_timer_t *t)
{
    fb_t *fb = t->user_data;
    fb->defer_timer = NULL;
    if (!fb || !fb->pending_reader) return;
    fb->pending_reader = false;
    if (strlen(fb->pending_reader_path) + 5 >= sizeof(fb->pending_reader_path)) {
        ESP_LOGE("file_browser", "path too long for epub cache");
        return;
    }
    char dst[FB_PATH_MAX];
    snprintf(dst, sizeof(dst), "%s.txt", fb->pending_reader_path);
    esp_err_t er = epub_convert(fb->pending_reader_path, dst);
    if (er == ESP_OK) {
        launcher_app_launch(LAUNCH_APP_READER, dst);
    } else {
        ESP_LOGE("file_browser", "epub convert failed: %d", er);
    }
}


/* ── 阅读界面（txt�?── */

/* 气泡定位：飘在进度条头部（knob）上方（bar 容器内，�?bar 动画同步）�?
 * 进度条值为行号，换算成百分比定位；y 固定 = 进度条上�?4px（bar 内） */
/* ── 事件 ── */

/* 列表项点击：文件夹进入下级；.txt 打开阅读；其他文件无操作 */
static void fb_item_event(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    fb_t *fb = (fb_t *)lv_event_get_user_data(e);   /* 注册时传入的�?fb_t* */
    if (!fb || !row) return;

    fb_row_data_t *rd = lv_obj_get_user_data(row);
    if (!rd) return;

    if (rd->is_dir) {
        /* 保存父目录的滚动位置（返回时恢复�?*/
        if (fb->depth < (int)(sizeof(fb->stack) / sizeof(fb->stack[0]))) {
            strncpy(fb->stack[fb->depth].path, fb->cur, sizeof(fb->stack[fb->depth].path) - 1);
            fb->stack[fb->depth].scroll_y = lv_obj_get_scroll_y(fb->list);
            fb->depth++;
        }
        strncpy(fb->cur, rd->path, sizeof(fb->cur) - 1);
        fb->cur[sizeof(fb->cur) - 1] = '\0';
        free(rd);
        lv_obj_set_user_data(row, NULL);
        fb->pending_scroll = -1;   /* 子目录从头开�?*/
        fb_refresh(fb);
    } else if (fb_is_txt(rd->path)) {
        /* 点 txt → 跳转阅读器 APP（直接打开模式）。保存浏览器滚动位置
         * （压栈保活，返回时对象树未销毁，滚动位置天然保留，这里仅存路径）。
         * 延迟执行避开 indev 事件上下文。
         * 注意：不 free(rd)/不清 user_data——压栈保活的行对象要支持二次点击；
         * 释放统一由 fb_refresh→fb_free_rows 在下次重建列表时完成 */
        fb->pending_scroll = lv_obj_get_scroll_y(fb->list);
        strncpy(fb->pending_reader_path, rd->path, sizeof(fb->pending_reader_path) - 1);
        fb->pending_reader_path[sizeof(fb->pending_reader_path) - 1] = '\0';
        fb->pending_reader = true;
        fb->defer_timer = lv_timer_create(fb_open_reader_deferred, 1, fb);
        lv_timer_set_repeat_count(fb->defer_timer, 1);
    } else if (fb_is_image(rd->path)) {
        /* 点图片 → 图片查看器（直接打开该图），行对象保留（可二次点击） */
        fb->pending_scroll = lv_obj_get_scroll_y(fb->list);
        strncpy(fb->pending_reader_path, rd->path, sizeof(fb->pending_reader_path) - 1);
        fb->pending_reader_path[sizeof(fb->pending_reader_path) - 1] = '\0';
        fb->pending_reader = true;
        fb->defer_timer = lv_timer_create(fb_open_image_deferred, 1, fb);
        lv_timer_set_repeat_count(fb->defer_timer, 1);
    } else if (fb_is_epub(rd->path)) {
        /* 点 epub → 转换 TXT 后进阅读器，行对象保留（可二次点击） */
        fb->pending_scroll = lv_obj_get_scroll_y(fb->list);
        strncpy(fb->pending_reader_path, rd->path, sizeof(fb->pending_reader_path) - 1);
        fb->pending_reader_path[sizeof(fb->pending_reader_path) - 1] = '\0';
        fb->pending_reader = true;
        fb->defer_timer = lv_timer_create(fb_open_epub_deferred, 1, fb);
        lv_timer_set_repeat_count(fb->defer_timer, 1);
    } else {
        free(rd);   /* 其他文件：无操作 */
        lv_obj_set_user_data(row, NULL);
    }
}

/* 底部按钮：根目录 / 排序（浏览器模式；返回上一级已由右滑手势承担） */
static void fb_btn_event(lv_event_t *e)
{
    fb_t *fb = fb_get(lv_event_get_user_data(e));
    if (!fb || lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    lv_obj_t *t = lv_event_get_target_obj(e);

    if (t == fb->btn_root) {
        /* 返回根目录：清空导航栈，回顶�?*/
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

lv_obj_t *file_browser_create(lv_obj_t *parent, file_browser_back_cb_t back_cb, void *ctx)
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
    fb->back_cb = back_cb;
    fb->back_ctx = ctx;
    lv_obj_set_user_data(obj, fb);

    /* 顶部路径（返回按钮右侧） */
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

    /* 底部按钮�?*/
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

    fb->btn_root = lv_button_create(bar);
    fb->btn_sort = lv_button_create(bar);
    lv_obj_t *lbl_root = lv_label_create(fb->btn_root);
    lv_label_set_text(lbl_root, LV_SYMBOL_HOME);
    fb->lbl_sort = lv_label_create(fb->btn_sort);
    lv_label_set_text_fmt(fb->lbl_sort, "%s 1", LV_SYMBOL_REFRESH);

    lv_obj_t *btns[2] = { fb->btn_root, fb->btn_sort };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(btns[i], (fb_screen_w() - 6 * 2 - 6 * 2) / 2, 28);   /* 屏宽自适应均分（横竖屏通用�?*/
        lv_obj_set_style_bg_color(btns[i], FB_BTN_BG, 0);
        lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btns[i], FB_BTN_BORDER, 0);
        lv_obj_set_style_border_width(btns[i], 1, 0);
        lv_obj_set_style_radius(btns[i], 6, 0);
        lv_obj_set_style_text_color(btns[i], FB_BTN_TEXT, 0);
        lv_obj_add_event_cb(btns[i], fb_btn_event, LV_EVENT_CLICKED, obj);
    }


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

/* 右滑返回手势（launcher 分发调用）：
 * 非根目录 → 返回上一级；根目录 → 返回 true（由 launcher 关闭本 APP 回来源/桌面）。
 * 注意：点 txt 已跳转阅读器 APP（压栈），本浏览器不再内嵌阅读器。 */
bool file_browser_swipe_back(lv_obj_t *obj)
{
    fb_t *fb = fb_get(obj);
    if (!fb) return true;
    if (fb_is_root(fb)) {
        ESP_LOGI(FB_TAG, "[SWIPE] at root -> return true (close app)");
        return true;
    }
    ESP_LOGI(FB_TAG, "[SWIPE] not root (cur=%s) -> back up", fb->cur);
    fb_back_up(fb);
    return false;
}

/* 调试事件（测试模块用）：打印内部状态，验证回调链路 */
void file_browser_debug_event(lv_obj_t *obj, int evt)
{
    fb_t *fb = fb_get(obj);
    if (!fb) return;
    ESP_LOGI(FB_TAG, "[DBG] evt=%d cur=%s root=%s depth=%d sort=%d",
             evt, fb->cur, fb->root, fb->depth, fb->sort_mode);
}

void file_browser_destroy(lv_obj_t *obj)
{
    fb_t *fb = fb_get(obj);
    if (!fb) return;
    if (fb->defer_timer) lv_timer_delete(fb->defer_timer);
    /* 释放列表行数据（lv_obj_delete 不会释放 user_data） */
    uint32_t n = lv_obj_get_child_count(fb->list);
    for (uint32_t i = 0; i < n; i++) {
        fb_row_data_t *rd = lv_obj_get_user_data(lv_obj_get_child(fb->list, i));
        if (rd) free(rd);
    }
    free(fb);
    /* 闪屏修复：先隐藏 + 立即刷新一帧（露出桌面），再删除全屏对�?*/
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    lv_obj_delete(obj);
}
