/* net_console.c —— 网络服务信息 APP（LVGL 9） */

#include "net_console.h"
#include "app_font.h"
#include "app_web.h"
#include "app_uart.h"
#include "esp_log.h"
#include <stdlib.h>

/* 配色（深色，与书架一致） */
#define NC_BG          lv_color_hex(0x111827)
#define NC_CARD        lv_color_hex(0x1F2937)
#define NC_TEXT        lv_color_hex(0xFFFFFF)
#define NC_DIM         lv_color_hex(0x9CA3AF)
#define NC_ACCENT      lv_color_hex(0x39C5BB)
#define NC_BORDER      lv_color_hex(0x374151)
#define NC_BAR_H       28

struct net_console {
    lv_obj_t *root;
    net_console_back_cb_t back_cb;
    void *back_ctx;
    lv_obj_t *btn_toggle;      /* 服务开启/停止按钮 */
    lv_obj_t *lbl_toggle;
    lv_obj_t *lbl_svc_state;   /* 服务状态显示 */
};

static lv_font_t *nc_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* 服务开关：停止/启动 Web 服务（httpd） */
static void nc_update_service(net_console_t *nc)
{
    const char *state = g_httpd ? "运行中" : "已停止";
    lv_label_set_text(nc->lbl_svc_state, state);
    lv_label_set_text(nc->lbl_toggle, g_httpd ? "停止" : "开启");
}

static void nc_btn_toggle_cb(lv_event_t *e)
{
    net_console_t *nc = lv_event_get_user_data(e);
    if (!nc) return;
    if (g_httpd) {
        app_web_stop();
    } else {
        app_web_start();
    }
    nc_update_service(nc);
}

/* 信息行：key 上行，value 换行完整显示；行高按屏幕高度动态（横/竖屏通用） */
static void nc_row(lv_obj_t *parent, const char *key, const char *value, int idx, int row_h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), row_h);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, NC_BAR_H + 60 + idx * (row_h + 6));
    lv_obj_set_style_bg_color(row, NC_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 12, 4);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, NC_DIM, 0);
    lv_obj_set_style_text_font(k, nc_font(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);   /* 长文本自动换行完整显示 */
    lv_obj_set_width(v, lv_pct(100) - 24);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 12, -4);
    lv_obj_set_style_text_color(v, NC_ACCENT, 0);
    lv_obj_set_style_text_font(v, nc_font(), 0);
}

net_console_t *net_console_create(lv_obj_t *parent, net_console_back_cb_t back_cb, void *ctx)
{
    net_console_t *nc = calloc(1, sizeof(net_console_t));
    if (!nc) return NULL;
    nc->back_cb = back_cb;
    nc->back_ctx = ctx;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, NC_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    nc->root = root;

    /* 顶部栏：← 返回 + 标题 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, lv_pct(100), NC_BAR_H);
    lv_obj_set_style_bg_color(bar, NC_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, NC_BORDER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, "网络服务");
    lv_obj_set_style_text_color(title, NC_TEXT, 0);
    lv_obj_set_style_text_font(title, nc_font(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* 服务状态 + 开启/停止按钮 */
    nc->lbl_svc_state = lv_label_create(root);
    lv_obj_align(nc->lbl_svc_state, LV_ALIGN_TOP_LEFT, 16, NC_BAR_H + 10);
    lv_obj_set_style_text_color(nc->lbl_svc_state, NC_TEXT, 0);
    lv_obj_set_style_text_font(nc->lbl_svc_state, nc_font(), 0);

    nc->btn_toggle = lv_button_create(root);
    lv_obj_set_size(nc->btn_toggle, 120, 36);
    lv_obj_align(nc->btn_toggle, LV_ALIGN_TOP_RIGHT, -16, NC_BAR_H + 6);
    lv_obj_set_style_bg_color(nc->btn_toggle, NC_CARD, 0);
    lv_obj_set_style_bg_opa(nc->btn_toggle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(nc->btn_toggle, NC_BORDER, 0);
    lv_obj_set_style_border_width(nc->btn_toggle, 1, 0);
    lv_obj_set_style_radius(nc->btn_toggle, 6, 0);
    lv_obj_set_style_text_color(nc->btn_toggle, NC_TEXT, 0);
    lv_obj_set_style_text_font(nc->btn_toggle, nc_font(), 0);
    lv_obj_add_event_cb(nc->btn_toggle, nc_btn_toggle_cb, LV_EVENT_CLICKED, nc);
    nc->lbl_toggle = lv_label_create(nc->btn_toggle);
    lv_obj_center(nc->lbl_toggle);

    nc_update_service(nc);

    /* 信息行：行高按屏高均分，横竖屏均不溢出 */
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());
    int row_h = (sh - NC_BAR_H - 60 - 12) / 5;
    if (row_h < 32) row_h = 32;
    nc_row(root, "WiFi AP", "Embedded-debug-tool", 0, row_h);
    nc_row(root, "IP", "192.168.4.1", 1, row_h);
    nc_row(root, "Web", "http://192.168.4.1/", 2, row_h);
    nc_row(root, "UART1 TCP", ":8080 (未启用)", 3, row_h);
    nc_row(root, "UART2 TCP", ":8081 (未启用)", 4, row_h);

    return nc;
}

void net_console_destroy(net_console_t *nc)
{
    if (!nc) return;
    if (nc->root) {
        /* 闪屏修复：先隐藏 + 立即刷新一帧（露出桌面），再删除全屏对象 */
        lv_obj_add_flag(nc->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(nc->root);
    }
    free(nc);
}

/* 右滑返回（launcher 分发）：无内部分级，直接请求关闭回桌面 */
bool net_console_swipe_back(net_console_t *nc)
{
    (void)nc;
    return true;
}

/* 调试事件（测试模块用）：打印内部状态（Web 服务状态）供验证 */
void net_console_debug_event(net_console_t *nc, int evt)
{
    if (!nc) return;
    ESP_LOGI("net_console", "[DBG] evt=%d web=%s", evt, g_httpd ? "running" : "stopped");
}
