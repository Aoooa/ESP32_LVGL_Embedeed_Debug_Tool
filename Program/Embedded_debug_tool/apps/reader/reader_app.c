/* reader_app.c —— 阅读器 APP（LVGL 9）：书架页（扫描 SD 列 txt）+ 阅读页（reader_view）。
 * 由 launcher 统一管理：arg=NULL 书架模式；arg=路径 直接打开指定 txt。 */

#include "reader_app.h"
#include "drv_sdcard.h"
#include "app_sdcard.h"
#include "app_font.h"
#include "reader_view.h"
#include "launcher.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "misc/lv_timer_private.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#define RA_PATH_MAX     128
#define RA_ENTRY_MAX    128      /* 扫描收集上限（128×208B≈26KB 内部 RAM） */
#define RA_NAME_MAX     80       /* 显示名（去后缀） */
#define RA_TOP_BAR_H    28
#define RA_ROW_H        34
#define RA_DIRQ_MAX     128      /* 待扫描目录队列上限（BFS，PSRAM 分配） */
#define RA_MAX_DEPTH    8        /* 扫描最大深度（广度优先，栈需求恒定） */
#define RA_SCAN_DEPTH   1        /* 根目录 + 一级子目录 */

/* 配色（与 file_browser 深色一致） */
#define RA_BG           lv_color_hex(0x000000)
#define RA_ROW          lv_color_hex(0x000000)
#define RA_ROW_PRESSED  lv_color_hex(0x374151)
#define RA_TEXT         lv_color_hex(0xFFFFFF)
#define RA_EMPTY        lv_color_hex(0x9CA3AF)
#define RA_BTN_BORDER   lv_color_hex(0x374151)

typedef struct {
    char path[RA_PATH_MAX];   /* 完整路径（打开阅读用） */
    char name[RA_NAME_MAX];   /* 显示名（去 .txt 后缀） */
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
    (void)mtime;
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
    app->entry_count++;
}

static int ra_cmp(const void *a, const void *b)
{
    const ra_entry_t *ea = a, *eb = b;
    return strcasecmp(ea->name, eb->name);
}

/* 无 SD 卡：显示提示后自动返回 */
static void ra_no_sd_back(lv_timer_t *t)
{
    reader_app_t *app = t->user_data;
    if (!app) return;
    app->no_sd_timer = NULL;
    if (app->back_cb) app->back_cb(app->back_ctx);
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

    /* 广度优先遍历：根目录 + 子目录（队列迭代，任意深度栈需求恒定） */
    bool sd_ok = true;
    for (int qh = 0; qh < app->dirq_tail; qh++) {
        strncpy(app->scan_dir, app->dirq[qh].path, sizeof(app->scan_dir) - 1);
        app->scan_depth = app->dirq[qh].depth;
        esp_err_t err = app_sdcard_list_dir(app->dirq[qh].path, ra_entry_cb, app);
        if (err != ESP_OK && qh == 0) {
            sd_ok = false;   /* 根目录打不开 = SD 未就绪 */
        }
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

    if (app->entry_count == 0) {
        lv_obj_t *t = lv_label_create(app->list);
        lv_label_set_text(t, "(没有 TXT 文件)");
        lv_obj_set_style_text_color(t, RA_EMPTY, 0);
    } else {
        qsort(app->entries, app->entry_count, sizeof(ra_entry_t), ra_cmp);
        for (int i = 0; i < app->entry_count; i++) {
            lv_obj_t *row = lv_button_create(app->list);
            lv_obj_set_size(row, lv_pct(100), RA_ROW_H);
            lv_obj_set_style_pad_hor(row, 10, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_bg_color(row, RA_ROW, 0);
            lv_obj_set_style_bg_color(row, RA_ROW_PRESSED, LV_STATE_PRESSED);
            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, app->entries[i].name);
            lv_obj_set_style_text_color(lbl, RA_TEXT, 0);
            lv_obj_set_style_text_font(lbl, ra_ui_font(), 0);
            lv_obj_set_user_data(row, (void *)(intptr_t)i);
            lv_obj_add_event_cb(row, ra_item_event, LV_EVENT_CLICKED, app);
        }
    }

    lv_label_set_text_fmt(app->lbl_count, "%d 本", app->entry_count);
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

    /* 中部列表 */
    int list_h = lv_obj_get_height(parent) - RA_TOP_BAR_H;
    app->list = lv_list_create(root);
    lv_obj_set_size(app->list, lv_pct(100), list_h);
    lv_obj_align(app->list, LV_ALIGN_TOP_LEFT, 0, RA_TOP_BAR_H);
    lv_obj_set_style_bg_color(app->list, RA_BG, 0);
    lv_obj_set_style_border_width(app->list, 0, 0);
    lv_obj_set_style_radius(app->list, 0, 0);
    lv_obj_set_style_pad_all(app->list, 0, 0);
    lv_obj_set_style_pad_row(app->list, 2, 0);
    lv_obj_set_style_text_color(app->list, RA_TEXT, 0);

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
    /* 阅读页打开时：右滑返回手势先问阅读器——状态栏显示中则隐藏栏并拦截
     * （返回 false，取消拖动）；栏已隐藏则按入口决定返回目标 */
    if (app->rv && reader_view_active(app->rv)) {
        if (reader_view_handle_back(app->rv)) return false;   /* 栏显示→隐藏栏，拦截 */
        /* 栏已隐藏：
         *   书架模式 → 直接关闭阅读层回书架（拦截 launcher 的 root 拖动，
         *             否则拖动整个 reader root 会先露出下层桌面而非书架）
         *   direct 模式 → 放行，进入跟随右滑返回上一级（file_browser） */
        if (!app->direct_mode) {
            ESP_LOGI("reader_app", "[SWIPE] shelf-mode reader -> close to shelf");
            reader_view_close(app->rv);
            return false;
        }
        return true;
    }
    return true;
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
        reader_view_close(app->rv);
        lv_obj_set_x(app->root, 0);   /* 滑出动画把 root 推到屏外，复位回书架 */
        lv_obj_update_layout(app->root);
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
