/* bookshelf.c —— TXT 书架界面（LVGL 9）：扫描 SD 收集 txt → 列表 → 点书进入阅读 */

#include "bookshelf.h"
#include "drv_sdcard.h"
#include "app_sdcard.h"
#include "app_font.h"
#include "reader_view.h"
#include "esp_heap_caps.h"
#include "misc/lv_timer_private.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>


#define BS_PATH_MAX     128
#define BS_ENTRY_MAX    128      /* 扫描收集上限（128×208B≈26KB 内部 RAM） */
#define BS_NAME_MAX     80       /* 显示名（去后缀） */
#define BS_TOP_BAR_H    28
#define BS_ROW_H        34
#define BS_DIRQ_MAX     128      /* 待扫描目录队列上限（BFS，PSRAM 分配） */
#define BS_MAX_DEPTH    8        /* 扫描最大深度（广度优先，栈需求恒定） */



/* 扫描范围：根目录 + 一级子目录。FATFS 深层路径解析实测限制为
 * 2 个组件（1 级目录+文件名），且扫描必须与 LCD 渲染同线程
 * （SD 与 LCD 共享 SPI2 总线，跨任务并发会触发 SPI 断言） */
#define BS_SCAN_DEPTH   1

/* 配色（与 file_browser 深色一致） */
#define BS_BG           lv_color_hex(0x111827)
#define BS_ROW          lv_color_hex(0x1F2937)
#define BS_ROW_PRESSED  lv_color_hex(0x374151)
#define BS_TEXT         lv_color_hex(0xFFFFFF)
#define BS_EMPTY        lv_color_hex(0x9CA3AF)
#define BS_BTN_BG       lv_color_hex(0x1F2937)
#define BS_BTN_BORDER   lv_color_hex(0x374151)

typedef struct {
    char path[BS_PATH_MAX];   /* 完整路径（打开阅读用） */
    char name[BS_NAME_MAX];   /* 显示名（去 .txt 后缀） */
} bs_entry_t;

struct bookshelf {
    lv_obj_t *root;
    lv_obj_t *list;
    lv_obj_t *lbl_count;
    bookshelf_back_cb_t back_cb;
    void *back_ctx;
    reader_view_t *rv;
    lv_timer_t *no_sd_timer;   /* 无 SD 自动返回定时器 */

    bs_entry_t *entries;
    int entry_count;
    int pending_idx;          /* 待打开的书索引（事件回调置位，定时器执行） */
    lv_timer_t *defer_timer;  /* 延迟打开一次性定时器 */
    /* 扫描上下文（回调内使用） */
    char scan_dir[BS_PATH_MAX];
    int scan_depth;
    /* 广度优先目录队列（PSRAM，回调里入队） */
    struct { char path[BS_PATH_MAX]; int depth; } *dirq;
    int dirq_tail;
    bool dirq_oom;              /* 队列满标志 */
};

static int bs_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_horizontal_resolution(d) : 320;
}

static lv_font_t *bs_ui_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static bool bs_is_txt(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    return strcasecmp(name + n - 4, ".txt") == 0;
}

/* 书名点击：打开阅读（reader_view 覆盖层，返回回书架） */
/* 延迟打开阅读（一次性定时器，下一帧 LVGL 循环执行，避开 indev 事件上下文） */
static void bs_open_deferred(lv_timer_t *t)
{
    bookshelf_t *bs = t->user_data;
    bs->defer_timer = NULL;
    if (!bs || bs->pending_idx < 0) return;
    int idx = bs->pending_idx;
    bs->pending_idx = -1;
    if (bs->entries && idx < bs->entry_count && !reader_view_active(bs->rv)) {
        reader_view_open(bs->rv, bs->entries[idx].path);
    }
}

static void bs_item_event(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    bookshelf_t *bs = lv_event_get_user_data(e);
    if (!bs || !row || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    if (idx < 0 || idx >= bs->entry_count || reader_view_active(bs->rv)) return;
    bs->pending_idx = idx;
    bs->defer_timer = lv_timer_create(bs_open_deferred, 1, bs);
    lv_timer_set_repeat_count(bs->defer_timer, 1);
}

/* ── 扫描（LVGL 线程同步执行，非递归：根目录 + 一级子目录）。
 *    与 LCD 渲染同线程 → 共享 SPI2 总线串行访问，避免跨任务并发冲突；
 *    非递归 + 深度 1 → 栈需求小，不超出 LVGL 8KB 任务栈。 ── */

static void bs_entry_cb(void *ctx, const char *name, bool is_dir, long size, time_t mtime)
{
    (void)size;
    (void)mtime;
    bookshelf_t *bs = ctx;
    if (is_dir) {
        if (bs->scan_depth < BS_MAX_DEPTH && bs->dirq_tail < BS_DIRQ_MAX) {
            if (snprintf(bs->dirq[bs->dirq_tail].path,
                         sizeof(bs->dirq[bs->dirq_tail].path),
                         "%s/%s", bs->scan_dir, name)
                < (int)sizeof(bs->dirq[bs->dirq_tail].path)) {
                bs->dirq[bs->dirq_tail].depth = bs->scan_depth + 1;
                bs->dirq_tail++;
            }
        } else if (bs->dirq_tail >= BS_DIRQ_MAX) {
            bs->dirq_oom = true;
        }
        return;
    }
    if (bs->entry_count >= BS_ENTRY_MAX || !bs_is_txt(name)) return;
    bs_entry_t *e = &bs->entries[bs->entry_count];
    if (snprintf(e->path, sizeof(e->path), "%s/%s", bs->scan_dir, name) >= (int)sizeof(e->path)) {
        return;   /* 路径过长跳过 */
    }
    size_t n = strlen(name);
    size_t nl = n - 4;   /* 去 .txt 后缀长度 */
    if (nl >= sizeof(e->name)) nl = sizeof(e->name) - 1;
    memcpy(e->name, name, nl);
    e->name[nl] = '\0';
    bs->entry_count++;
}

static int bs_cmp(const void *a, const void *b)
{
    const bs_entry_t *ea = a, *eb = b;
    return strcasecmp(ea->name, eb->name);
}

/* 无 SD 卡：显示提示后自动返回 */
static void bs_no_sd_back(lv_timer_t *t)
{
    bookshelf_t *bs = t->user_data;
    if (!bs) return;
    bs->no_sd_timer = NULL;
    if (bs->back_cb) bs->back_cb(bs->back_ctx);
}

static void bs_scan(bookshelf_t *bs)
{
    lv_obj_clean(bs->list);
    bs->entry_count = 0;

    /* 条目缓冲（PSRAM，释放内部 RAM） */
    if (bs->entries) {
        heap_caps_free(bs->entries);
        bs->entries = NULL;
    }
    bs->entries = heap_caps_calloc(BS_ENTRY_MAX, sizeof(bs_entry_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bs->entries) {
        lv_obj_t *t = lv_label_create(bs->list);
        lv_label_set_text(t, "(no memory)");
        lv_obj_set_style_text_color(t, BS_EMPTY, 0);
        return;
    }

    /* 目录队列（PSRAM，广度优先，栈需求恒定） */
    if (bs->dirq) {
        heap_caps_free(bs->dirq);
        bs->dirq = NULL;
    }
    bs->dirq = heap_caps_calloc(BS_DIRQ_MAX, sizeof(*bs->dirq),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bs->dirq) {
        heap_caps_free(bs->entries);
        bs->entries = NULL;
        lv_obj_t *t = lv_label_create(bs->list);
        lv_label_set_text(t, "(no memory)");
        lv_obj_set_style_text_color(t, BS_EMPTY, 0);
        return;
    }
    bs->dirq_tail = 0;
    bs->dirq_oom = false;
    snprintf(bs->dirq[0].path, sizeof(bs->dirq[0].path), "%s", DRV_SDCARD_MOUNT_POINT);
    bs->dirq[0].depth = 0;
    bs->dirq_tail = 1;   /* 根目录入队（漏置 1 会导致 BFS 循环不执行） */

    /* 广度优先遍历：根目录 + 子目录（FATFS 路径解析本身支持深层，
     * 递归会因 LFN 栈缓冲（DEF_NAMBUF）叠加导致 LVGL 8KB 栈溢出，
     * 因此用队列迭代，任意深度栈需求恒定） */
    bool sd_ok = true;
    for (int qh = 0; qh < bs->dirq_tail; qh++) {
        strncpy(bs->scan_dir, bs->dirq[qh].path, sizeof(bs->scan_dir) - 1);
        bs->scan_depth = bs->dirq[qh].depth;
        esp_err_t err = app_sdcard_list_dir(bs->dirq[qh].path, bs_entry_cb, bs);
        if (err != ESP_OK && qh == 0) {
            sd_ok = false;   /* 根目录打不开 = SD 未就绪 */
        }
    }

    if (!sd_ok) {
        /* SD 未就绪：提示 + 延迟自动返回 */
        heap_caps_free(bs->entries);
        bs->entries = NULL;
        heap_caps_free(bs->dirq);
        bs->dirq = NULL;
        lv_obj_t *t = lv_label_create(bs->list);
        lv_label_set_text(t, "无 SD 卡");
        lv_obj_set_style_text_color(t, BS_EMPTY, 0);
        lv_obj_set_style_text_font(t, bs_ui_font(), 0);
        if (bs->no_sd_timer) lv_timer_delete(bs->no_sd_timer);
        bs->no_sd_timer = lv_timer_create(bs_no_sd_back, 1000, bs);
        lv_timer_set_repeat_count(bs->no_sd_timer, 1);
        lv_label_set_text(bs->lbl_count, "0 本");
        return;
    }

    if (bs->no_sd_timer) {
        lv_timer_delete(bs->no_sd_timer);
        bs->no_sd_timer = NULL;
    }


    if (bs->entry_count == 0) {
        lv_obj_t *t = lv_label_create(bs->list);
        lv_label_set_text(t, "(没有 TXT 文件)");
        lv_obj_set_style_text_color(t, BS_EMPTY, 0);
    } else {
        qsort(bs->entries, bs->entry_count, sizeof(bs_entry_t), bs_cmp);
        for (int i = 0; i < bs->entry_count; i++) {
            lv_obj_t *row = lv_button_create(bs->list);
            lv_obj_set_size(row, lv_pct(100), BS_ROW_H);
            lv_obj_set_style_pad_hor(row, 10, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_bg_color(row, BS_ROW, 0);
            lv_obj_set_style_bg_color(row, BS_ROW_PRESSED, LV_STATE_PRESSED);
            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, bs->entries[i].name);
            lv_obj_set_style_text_color(lbl, BS_TEXT, 0);
            lv_obj_set_style_text_font(lbl, bs_ui_font(), 0);
            lv_obj_set_user_data(row, (void *)(intptr_t)i);
            lv_obj_add_event_cb(row, bs_item_event, LV_EVENT_CLICKED, bs);
        }
    }

    lv_label_set_text_fmt(bs->lbl_count, "%d 本", bs->entry_count);
}


/* ── Public API ── */

bookshelf_t *bookshelf_create(lv_obj_t *parent, bookshelf_back_cb_t back_cb, void *ctx)
{
    bookshelf_t *bs = calloc(1, sizeof(bookshelf_t));
    if (!bs) return NULL;
    bs->back_cb = back_cb;
    bs->back_ctx = ctx;
    bs->pending_idx = -1;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, BS_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    bs->root = root;

    /* 顶部栏：← 返回 | 书架（居中） | N 本（右上） */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, lv_pct(100), BS_TOP_BAR_H);
    lv_obj_set_style_bg_color(bar, BS_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, BS_BTN_BORDER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_color(title, BS_TEXT, 0);
    lv_obj_set_style_text_font(title, bs_ui_font(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "书架");

    bs->lbl_count = lv_label_create(bar);
    lv_obj_set_width(bs->lbl_count, 56);
    lv_obj_set_style_text_color(bs->lbl_count, BS_TEXT, 0);
    lv_obj_set_style_text_font(bs->lbl_count, bs_ui_font(), 0);
    lv_obj_set_style_text_align(bs->lbl_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_right(bs->lbl_count, 8, 0);
    lv_label_set_text(bs->lbl_count, "0 本");

    /* 中部列表 */
    int list_h = lv_obj_get_height(parent) - BS_TOP_BAR_H;
    bs->list = lv_list_create(root);
    lv_obj_set_size(bs->list, lv_pct(100), list_h);
    lv_obj_align(bs->list, LV_ALIGN_TOP_LEFT, 0, BS_TOP_BAR_H);
    lv_obj_set_style_bg_color(bs->list, BS_BG, 0);
    lv_obj_set_style_border_width(bs->list, 0, 0);
    lv_obj_set_style_radius(bs->list, 0, 0);
    lv_obj_set_style_pad_all(bs->list, 0, 0);
    lv_obj_set_style_pad_row(bs->list, 2, 0);
    lv_obj_set_style_text_color(bs->list, BS_TEXT, 0);

    /* 阅读器（全屏覆盖层，返回回书架） */
    bs->rv = reader_view_create(root);
    if (bs->rv) {
        reader_view_set_back_cb(bs->rv, NULL, NULL);   /* 返回 = 仅关闭覆盖层，留在书架 */
    }

    bs_scan(bs);
    return bs;
}

void bookshelf_destroy(bookshelf_t *bs)
{
    if (!bs) return;
    if (bs->no_sd_timer) lv_timer_delete(bs->no_sd_timer);
    if (bs->defer_timer) lv_timer_delete(bs->defer_timer);
    if (bs->rv) reader_view_destroy(bs->rv);
    if (bs->entries) heap_caps_free(bs->entries);
    if (bs->dirq) heap_caps_free(bs->dirq);
    /* 闪屏修复：先隐藏 + 立即刷新一帧（露出桌面），再删除全屏对象 */
    if (bs->root) {
        lv_obj_add_flag(bs->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(bs->root);
    }
    free(bs);
}

void bookshelf_refresh(bookshelf_t *bs)
{
    if (bs) bs_scan(bs);
}

