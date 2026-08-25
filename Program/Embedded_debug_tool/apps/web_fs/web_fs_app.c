/* web_fs_app.c —— WebFS：SD 文件管理状态页（LVGL 深色风格）。
 * 页面为信息/提示页（web 路由由服务层 app_web_fs 提供，全局常开）。 */

#include "web_fs_app.h"
#include "app_cardreader.h"
#include "app_web_fs.h"
#include "app_font.h"
#include "esp_heap_caps.h"
#include "misc/lv_timer_private.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

/* 预生成二维码模块矩阵（apps/web_fs/wf_qr.inc，由 gen_qr.py 生成） */
#include "wf_qr.inc"

static const char *TAG = "web_fs_app";

#define WFBG       lv_color_hex(0x000000)
#define WFTEXT     lv_color_hex(0xFFFFFF)
#define WFDIM      lv_color_hex(0x9CA3AF)
#define WFACCENT   lv_color_hex(0x7DD3FC)
#define WFWARN     lv_color_hex(0xFBBF24)
#define WFBORDER   lv_color_hex(0x374151)

struct web_fs_app {
    lv_obj_t *root;
    lv_obj_t *toggle_btn;        /* web 服务开关（默认关） */
    lv_obj_t *toggle_lbl;
    lv_obj_t *url_lbl;           /* URL / off 状态行 */
    lv_obj_t *transfer_lbl;      /* 传输状态行 */
    lv_timer_t *transfer_timer;  /* 轮询 web_fs 传输状态（200ms） */
    lv_obj_t *qr_canvas;         /* 二维码 canvas */
    void *qr_buf;                /* canvas 位图（PSRAM） */
};

static lv_font_t *wf_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static lv_obj_t *wf_row(lv_obj_t *parent, const char *key, const char *value, lv_color_t vc)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, WFBORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, WFDIM, 0);
    lv_obj_set_style_text_font(k, wf_font(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, vc, 0);
    lv_obj_set_style_text_font(v, wf_font(), 0);
    return v;
}

/* 传输状态轮询（200ms）：显示 Idle / Uploading xxx NN% / Downloading xxx NN% */
static void wf_transfer_tick(lv_timer_t *t)
{
    web_fs_app_t *app = t->user_data;
    if (!app || !app->transfer_lbl) return;
    app_web_fs_status_t st;
    if (app_web_fs_get_status(&st) != ESP_OK) return;
    if (!st.busy) {
        lv_label_set_text(app->transfer_lbl, "Idle");
        return;
    }
    const char *act = st.upload ? "Uploading" : "Downloading";
    if (st.total > 0) {
        uint32_t pct = st.done * 100 / st.total;
        if (pct > 100) pct = 100;
        lv_label_set_text_fmt(app->transfer_lbl, "%s %s %d%%", act, st.name, (int)pct);
    } else {
        lv_label_set_text_fmt(app->transfer_lbl, "%s %s", act, st.name);
    }
}

/* 二维码：预生成矩阵 → RGB565 画布（存 flash，运行时只做像素展开） */
static void wf_draw_qr(lv_obj_t *parent, web_fs_app_t *app)
{
    int n = wf_qr_n;
    int scale = 3;
    int px = n * scale;
    uint16_t *buf = heap_caps_malloc((size_t)px * px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    for (int i = 0; i < px * px; i++) buf[i] = 0xFFFF;   /* 白底 */
    const uint8_t *mm = wf_qr_modules;
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            if (!mm[r * n + c]) continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    buf[(r * scale + dy) * px + (c * scale + dx)] = 0x0000;
                }
            }
        }
    }
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, px, px, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(cv, px, px);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);   /* 不拦截列表滚动 */
    app->qr_buf = buf;
    app->qr_canvas = cv;
}

/* 开关：启用/停用 /fs* 路由 */
static void wf_toggle_evt(lv_event_t *e)
{
    web_fs_app_t *app = lv_event_get_user_data(e);
    if (!app) return;
    if (app_web_fs_enabled()) app_web_fs_stop();
    else app_web_fs_start();
    if (app->toggle_lbl) lv_label_set_text(app->toggle_lbl, app_web_fs_enabled() ? "ON" : "OFF");
    if (app->url_lbl) {
        bool on = app_web_fs_enabled();
        lv_label_set_text(app->url_lbl, on ? "http://192.168.4.1/fs" : "Web server off");
        lv_obj_set_style_text_color(app->url_lbl, on ? WFACCENT : WFDIM, 0);
    }
}

web_fs_app_t *web_fs_app_create(lv_obj_t *parent, void (*back_cb)(void *ctx), void *ctx)
{
    (void)back_cb;
    (void)ctx;
    web_fs_app_t *app = calloc(1, sizeof(web_fs_app_t));
    if (!app) return NULL;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, WFBG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    app->root = root;

    /* 顶部栏：标题 + 服务开关 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, WFBG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, WFBORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, WFTEXT, 0);
    lv_obj_set_style_text_font(title, wf_font(), 0);
    lv_label_set_text(title, "WebFS");
    app->toggle_btn = lv_button_create(bar);
    lv_obj_set_width(app->toggle_btn, 56);
    lv_obj_set_height(app->toggle_btn, 28);
    lv_obj_set_style_bg_color(app->toggle_btn, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_color(app->toggle_btn, lv_color_hex(0x374151), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(app->toggle_btn, WFBORDER, 0);
    lv_obj_set_style_border_width(app->toggle_btn, 1, 0);
    lv_obj_set_style_radius(app->toggle_btn, 8, 0);
    lv_obj_set_style_pad_all(app->toggle_btn, 0, 0);
    app->toggle_lbl = lv_label_create(app->toggle_btn);
    lv_obj_center(app->toggle_lbl);
    lv_obj_set_style_text_font(app->toggle_lbl, wf_font(), 0);
    lv_obj_set_style_text_color(app->toggle_lbl, WFTEXT, 0);
    lv_label_set_text(app->toggle_lbl, app_web_fs_enabled() ? "ON" : "OFF");
    lv_obj_add_event_cb(app->toggle_btn, wf_toggle_evt, LV_EVENT_CLICKED, app);

    /* 信息区（可滚动，容纳状态/传输多行） */
    int shh = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_t *info = lv_obj_create(root);
    lv_obj_set_pos(info, 10, 50);
    lv_obj_set_size(info, lv_pct(100) - 20, shh - 50 - 8);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_radius(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_gap(info, 8, 0);
    lv_obj_set_scroll_dir(info, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    cardreader_state_t cr = app_cardreader_get_state();
    if (cr == CARDREADER_EXPOSED) {
        /* 读卡器正暴露给 PC：/sdcard VFS 被卸载，WebFS 不可用 */
        wf_row(info, "SD BUSY BY USB MSC",
               "Card reader is exposing the SD to PC. Exit CardR first, then reopen WebFS.",
               WFWARN);
    } else if (cr == CARDREADER_ERROR) {
        wf_row(info, "SD ERROR",
               "Previous SD operation failed (card missing?). Insert the card and retry.",
               WFWARN);
    } else {
        app->url_lbl = wf_row(info, "URL",
                              app_web_fs_enabled() ? "http://192.168.4.1/fs" : "Web server off",
                              app_web_fs_enabled() ? WFACCENT : WFDIM);
        wf_row(info, "Permission", "Read / write / delete files under /sdcard", WFTEXT);
    }

    /* 传输状态行（大文件上传/下载进度提示） */
    lv_obj_t *trow = lv_obj_create(info);
    lv_obj_set_size(trow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(trow, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(trow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(trow, WFBORDER, 0);
    lv_obj_set_style_border_width(trow, 1, 0);
    lv_obj_set_style_radius(trow, 8, 0);
    lv_obj_set_style_pad_hor(trow, 12, 0);
    lv_obj_set_style_pad_ver(trow, 8, 0);
    lv_obj_set_style_pad_gap(trow, 4, 0);
    lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_COLUMN);   /* 防止两 label 层叠 */
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *tk = lv_label_create(trow);
    lv_label_set_text(tk, "Transfer");
    lv_obj_set_style_text_color(tk, WFDIM, 0);
    lv_obj_set_style_text_font(tk, wf_font(), 0);
    app->transfer_lbl = lv_label_create(trow);
    lv_label_set_long_mode(app->transfer_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(app->transfer_lbl, lv_pct(100));
    lv_label_set_text(app->transfer_lbl, "Idle");
    lv_obj_set_style_text_color(app->transfer_lbl, WFACCENT, 0);
    lv_obj_set_style_text_font(app->transfer_lbl, wf_font(), 0);

    app->transfer_timer = lv_timer_create(wf_transfer_tick, 200, app);

    /* 二维码（扫描连接 WiFi AP 后直达 /fs，服务需先开启） */
    wf_draw_qr(info, app);

    ESP_LOGI(TAG, "create: cardreader=%d", (int)cr);
    return app;
}

void web_fs_app_destroy(web_fs_app_t *app)
{
    if (!app) return;
    if (app->transfer_timer) {
        lv_timer_delete(app->transfer_timer);
        app->transfer_timer = NULL;
    }
    if (app->root) {
        lv_obj_add_flag(app->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(app->root);
    }
    if (app->qr_buf) {
        heap_caps_free(app->qr_buf);   /* canvas 随 root 销毁，位图须手动释放 */
        app->qr_buf = NULL;
    }
    free(app);
}

bool web_fs_app_swipe_back(web_fs_app_t *app)
{
    (void)app;
    return true;   /* 全屏页：允许跟手拖动返回 */
}