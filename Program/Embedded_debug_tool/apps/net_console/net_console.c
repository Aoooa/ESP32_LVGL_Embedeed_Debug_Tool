/* net_console.c —— 网络服务信息 APP（LVGL 9） */

#include "net_console.h"
#include "app_font.h"
#include "app_wifi.h"
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
    lv_obj_t *btn_toggle;      /* AP 开启/停止按钮 */
    lv_obj_t *lbl_toggle;
    lv_obj_t *lbl_status_dot;  /* 状态指示灯：绿=AP 运行，红=AP 停止 */
};

static lv_font_t *nc_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* AP 状态刷新：指示灯 + 按钮文字 */
static void nc_update_service(net_console_t *nc)
{
    bool up = app_wifi_is_up();
    lv_obj_set_style_bg_color(nc->lbl_status_dot, up ? lv_color_hex(0x22C55E) : lv_color_hex(0xEF4444), 0);
    lv_label_set_text(nc->lbl_toggle, up ? "停止" : "开启");
}

static void nc_btn_toggle_cb(lv_event_t *e)
{
    net_console_t *nc = lv_event_get_user_data(e);
    if (!nc) return;
    if (app_wifi_is_up()) {
        app_wifi_stop();
    } else {
        app_wifi_start();
    }
    nc_update_service(nc);
}

/* 信息行：两行显示——上行 key、下行 value（flex column）。
 * 行放入 flex column 容器后 flex_grow=1 均分高度，横竖屏均不溢出/重叠。
 * value 长文本自动换行完整显示 */
static void nc_row(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, NC_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_gap(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, NC_DIM, 0);
    lv_obj_set_style_text_font(k, nc_font(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);   /* 长文本换行完整显示 */
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

    /* 顶部栏：状态指示灯（左）+ 开启/停止按钮（右），无标题 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, lv_pct(100), NC_BAR_H);
    lv_obj_set_style_bg_color(bar, NC_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, NC_BORDER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bar, 10, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 状态指示灯：圆点，绿=AP 运行，红=AP 停止 */
    nc->lbl_status_dot = lv_obj_create(bar);
    lv_obj_set_size(nc->lbl_status_dot, 12, 12);
    lv_obj_set_style_radius(nc->lbl_status_dot, 6, 0);
    lv_obj_set_style_bg_color(nc->lbl_status_dot, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_bg_opa(nc->lbl_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nc->lbl_status_dot, 0, 0);

    /* 占位撑开（按钮右对齐） */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    /* AP 开启/停止按钮 */
    nc->btn_toggle = lv_button_create(bar);
    lv_obj_set_size(nc->btn_toggle, 80, 22);
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

    /* 默认 AP 关闭（main 不启动），显示红点 + "开启" */
    nc_update_service(nc);

    /* 信息区：可滚动容器 + 行按内容高度自适应（value 完整显示不被裁剪）。
     * 5 行两行显示（key 上/value 下），超屏可滚动 */
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_t *info = lv_obj_create(root);
    lv_obj_set_pos(info, 8, NC_BAR_H + 10);
    lv_obj_set_size(info, lv_pct(100) - 16, sh - NC_BAR_H - 10 - 8);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_radius(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_gap(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(info, LV_DIR_VER);   /* 内容超界可滚动 */
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);

    static const char *rows[][2] = {
        { "WiFi AP",  "Embedded-debug-tool" },
        { "IP",       "192.168.4.1" },
        { "Web",      "http://192.168.4.1/" },
        { "UART1 TCP", "8080" },
        { "UART2 TCP", "8081" },
    };
    for (int i = 0; i < 5; i++) {
        nc_row(info, rows[i][0], rows[i][1]);
    }

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

/* 调试事件（测试模块用）：打印内部状态（AP 状态）供验证 */
void net_console_debug_event(net_console_t *nc, int evt)
{
    if (!nc) return;
    ESP_LOGI("net_console", "[DBG] evt=%d ap=%s", evt, app_wifi_is_up() ? "up" : "down");
}
