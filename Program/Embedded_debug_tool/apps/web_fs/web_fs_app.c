/* web_fs_app.c —— WebFS：SD 文件管理状态页（LVGL 深色风格）。
 * 页面为信息/提示页（web 路由由服务层 app_web_fs 提供，全局常开）。 */

#include "web_fs_app.h"
#include "app_cardreader.h"
#include "app_font.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "web_fs_app";

#define WFBG       lv_color_hex(0x000000)
#define WFTEXT     lv_color_hex(0xFFFFFF)
#define WFDIM      lv_color_hex(0x9CA3AF)
#define WFACCENT   lv_color_hex(0x7DD3FC)
#define WFWARN     lv_color_hex(0xFBBF24)
#define WFBORDER   lv_color_hex(0x374151)

struct web_fs_app {
    lv_obj_t *root;
};

static lv_font_t *wf_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static void wf_row(lv_obj_t *parent, const char *key, const char *value, lv_color_t vc)
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

    /* 顶部栏 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, WFBG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, WFBORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_center(title);
    lv_obj_set_style_text_color(title, WFTEXT, 0);
    lv_obj_set_style_text_font(title, wf_font(), 0);
    lv_label_set_text(title, "WebFS");

    /* 信息区 */
    lv_obj_t *info = lv_obj_create(root);
    lv_obj_set_pos(info, 10, 50);
    lv_obj_set_size(info, lv_pct(100) - 20, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_radius(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_gap(info, 8, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
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
        wf_row(info, "URL", "http://192.168.4.1/fs", WFACCENT);
        wf_row(info, "Permission", "Read / write / delete files under /sdcard", WFTEXT);
        wf_row(info, "USB vs Web", "One or the other: while CardR exposes the disk, WebFS is off.",
               WFDIM);
    }

    ESP_LOGI(TAG, "create: cardreader=%d", (int)cr);
    return app;
}

void web_fs_app_destroy(web_fs_app_t *app)
{
    if (!app) return;
    if (app->root) {
        lv_obj_add_flag(app->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(app->root);
    }
    free(app);
}

bool web_fs_app_swipe_back(web_fs_app_t *app)
{
    (void)app;
    return true;   /* 全屏页：允许跟手拖动返回 */
}