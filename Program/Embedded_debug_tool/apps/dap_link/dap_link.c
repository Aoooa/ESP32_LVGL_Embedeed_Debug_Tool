/* dap_link.c —— DAP Link（CMSIS-DAP 烧录器/调试器）APP（LVGL 9） */

#include "dap_link.h"
#include "app_dap.h"
#include "app_font.h"
#include "tinyusb.h"
#include "esp_log.h"

/* 配色（深色，与 card_reader 一致） */
#define DL_BG          lv_color_hex(0x111827)
#define DL_CARD        lv_color_hex(0x1F2937)
#define DL_TEXT        lv_color_hex(0xFFFFFF)
#define DL_DIM         lv_color_hex(0x9CA3AF)
#define DL_ACCENT      lv_color_hex(0x39C5BB)
#define DL_ERR         lv_color_hex(0xEF4444)

typedef void (*dap_link_back_cb_t)(void *ctx);

struct dap_link {
    lv_obj_t *root;
    lv_obj_t *val_if;
    lv_obj_t *val_map;
    lv_obj_t *val_status;
    lv_obj_t *val_hint;
    lv_obj_t *btn_toggle;
    lv_obj_t *lbl_toggle;
    lv_timer_t *timer;
    bool pc_attached;   /* USB 物理连接（tud_connected，LVGL 线程轮询） */
    bool pc_mounted;    /* USB 枚举完成（tud_mounted） */
};

/* APP 句柄（manifest launch 返回值） */
typedef struct dap_link dap_link_t;

static lv_font_t *dl_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* ── 状态 → 界面 ── */

static void dl_update(struct dap_link *dl)
{
    dap_state_t st = app_dap_get_state();

    static const char *st_txt[] = {
        [DAP_STATE_OFF] = "未启用",
        [DAP_STATE_READY] = "运行中",
        [DAP_STATE_ERROR] = "异常",
    };
    lv_label_set_text(dl->val_status, st_txt[st]);
    lv_obj_set_style_text_color(dl->val_status,
                                st == DAP_STATE_READY ? DL_ACCENT :
                                st == DAP_STATE_ERROR ? DL_ERR : DL_DIM, 0);

    const char *hint;
    switch (st) {
    case DAP_STATE_READY:
        hint = dl->pc_mounted
               ? "电脑已识别 2 个 CMSIS-DAP。\nKeil/pyOCD 可选择任一 SWD 口\n分别烧录；两板可依次烧录。"
               : dl->pc_attached
                 ? "USB 已连接，等待电脑枚举……"
                 : "正在等待电脑识别……\nUSB 连接电脑后应显示\n2 个 CMSIS-DAP 设备。";
        break;
    case DAP_STATE_ERROR:
        hint = "启动失败，请查看日志。\n若读卡器正在使用 USB，\n请先关闭读卡器。";
        break;
    case DAP_STATE_OFF:
    default:
        hint = "开启后电脑将显示 2 个 CMSIS-DAP\n调试器（SWD1/SWD2），可分别选择\n烧录不同目标板。";
        break;
    }
    lv_label_set_text(dl->val_hint, hint);

    bool active = (st == DAP_STATE_READY);
    lv_label_set_text(dl->lbl_toggle, active ? "关闭 DAP" : "开启 DAP");
    if (active || st == DAP_STATE_OFF || st == DAP_STATE_ERROR) {
        lv_obj_remove_state(dl->btn_toggle, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(dl->btn_toggle, LV_STATE_DISABLED);
    }
}

static void dl_timer_cb(lv_timer_t *t)
{
    struct dap_link *dl = lv_timer_get_user_data(t);
    if (!dl) return;

    /* DAP 开启时轮询 USB 栈状态（LVGL 线程，栈安全；tud_* 只读全局状态）。
     * connected=物理连接，mounted=PC 枚举完成 */
    if (app_dap_get_state() == DAP_STATE_READY) {
        bool c = tud_connected();
        bool m = tud_mounted();
        if (c != dl->pc_attached || m != dl->pc_mounted) {
            dl->pc_attached = c;
            dl->pc_mounted = m;
        }
    } else {
        dl->pc_attached = false;
        dl->pc_mounted = false;
    }
    dl_update(dl);
}

static void dl_btn_cb(lv_event_t *e)
{
    struct dap_link *dl = lv_event_get_user_data(e);
    if (!dl) return;

    dap_state_t st = app_dap_get_state();
    if (st == DAP_STATE_READY) {
        app_dap_disable();
    } else {
        esp_err_t ret = app_dap_enable();
        if (ret == ESP_ERR_INVALID_STATE) {
            lv_label_set_text(dl->val_hint, "读卡器正在使用 USB，请先\n在 CardR 中关闭读卡器。");
        }
    }
    dl_update(dl);   /* 立即刷新（enable/disable 期间服务状态已变化） */
}

/* 信息行：key（上）/value（下）两行显示 */
static void dl_row(lv_obj_t *parent, const char *key, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, DL_CARD, 0);
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
    lv_obj_set_style_text_color(k, DL_DIM, 0);
    lv_obj_set_style_text_font(k, dl_font(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    lv_obj_set_style_text_color(v, DL_ACCENT, 0);
    lv_obj_set_style_text_font(v, dl_font(), 0);
    *val_out = v;
}

dap_link_t *dap_link_create(lv_obj_t *parent, dap_link_back_cb_t back_cb, void *ctx)
{
    (void)back_cb;
    (void)ctx;
    dap_link_t *dl = lv_malloc(sizeof(dap_link_t));
    if (!dl) return NULL;
    lv_memzero(dl, sizeof(*dl));

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, DL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    dl->root = root;

    /* 信息区：SWD 接口 / 设备状态 / 提示 */
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_t *info = lv_obj_create(root);
    lv_obj_set_pos(info, 8, 10);
    lv_obj_set_size(info, lv_pct(100) - 16, sh - 10 - 8 - 56);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_radius(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_gap(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(info, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);

    dl_row(info, "SWD 接口（接目标板）", &dl->val_if);
    lv_label_set_text(dl->val_if,
                      "SWD1: GPIO11(SWDIO) 12(SWCLK) 13(RST)\n"
                      "SWD2: GPIO14(SWDIO) 15(SWCLK) 18(RST)\n"
                      "GND = 共地（每个目标各一组）");

    dl_row(info, "Keil 设备对应", &dl->val_map);
    lv_label_set_text(dl->val_map,
                      "列表第 1 个 = SWD1\n列表第 2 个 = SWD2\n设备管理器 MI_00/MI_01 同序");
    dl_row(info, "设备状态", &dl->val_status);
    dl_row(info, "提示", &dl->val_hint);

    /* 底部开启/关闭按钮 */
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 160, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(btn, DL_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, DL_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_text_color(btn, DL_TEXT, 0);
    lv_obj_set_style_text_font(btn, dl_font(), 0);
    lv_obj_add_event_cb(btn, dl_btn_cb, LV_EVENT_CLICKED, dl);
    dl->btn_toggle = btn;
    dl->lbl_toggle = lv_label_create(btn);
    lv_obj_center(dl->lbl_toggle);

    dl_update(dl);

    /* 状态轮询：服务事件来自 TinyUSB 任务，经 lv_timer 回到 LVGL 线程刷新 */
    dl->timer = lv_timer_create(dl_timer_cb, 250, dl);

    return dl;
}

void dap_link_destroy(dap_link_t *dl)
{
    if (!dl) return;
    if (app_dap_get_state() == DAP_STATE_READY) {
        ESP_LOGI("dap_link", "exit -> disable DAP");
        app_dap_disable();
    }
    if (dl->timer) lv_timer_delete(dl->timer);
    lv_obj_add_flag(dl->root, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    lv_obj_delete(dl->root);
    lv_free(dl);
}

bool dap_link_swipe_back(dap_link_t *dl)
{
    (void)dl;
    return true;
}
