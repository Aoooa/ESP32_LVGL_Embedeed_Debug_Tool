/* dap_link.c —— DAP Link（CMSIS-DAP 烧录器/调试器）APP（LVGL 9） */

#include "dap_link.h"
#include "app_dap.h"
#include "app_display.h"
#include "esp_lv_adapter.h"
#include <stdio.h>
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
    lv_obj_t *val_if;      /* SWD 接口 */
    lv_obj_t *val_status;  /* USB/无线合并状态 */
    lv_obj_t *val_hint;    /* 操作提示 */
    lv_obj_t *btn_toggle;
    lv_obj_t *lbl_toggle;
    lv_obj_t *btn_wifi;
    lv_obj_t *lbl_wifi;
    lv_timer_t *timer;
    bool pc_attached;   /* USB 物理连接（tud_connected，LVGL 线程轮询） */
    bool pc_mounted;    /* USB 枚举完成（tud_mounted） */
};

/* 无线开启任务句柄（WiFi init 需内部 RAM 栈；非 NULL = 启动中） */
static TaskHandle_t s_wifi_task;

static void dl_wifi_enable_task(void *arg);

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

    dap_wifi_state_t wst = app_dap_wifi_get_state();

    /* 状态行：USB / 无线 合并显示（二选一） */
    char buf[48];
    snprintf(buf, sizeof(buf), "USB: %s · 无线: %s",
             st == DAP_STATE_READY ? "运行中" :
             st == DAP_STATE_ERROR ? "异常" : "关闭",
             wst == DAP_WIFI_ON ? "运行中" :
             wst == DAP_WIFI_ERROR ? "异常" : "关闭");
    lv_label_set_text(dl->val_status, buf);
    lv_obj_set_style_text_color(dl->val_status,
                                (st == DAP_STATE_READY || wst == DAP_WIFI_ON)
                                ? DL_ACCENT : DL_DIM, 0);

    /* 提示行：按当前模式给一句操作指引 */
    const char *hint;
    if (wst == DAP_WIFI_ON) {
        hint = "PC 连 Embedded-debug-tool AP 后:\nusbip --tcp-port 872 attach\n-r 192.168.4.1 -b 1-1";
    } else if (st == DAP_STATE_READY) {
        hint = dl->pc_mounted
               ? "USB 已就绪，Keil 选 CMSIS-DAP\n（列表第 1 个 = SWD1）"
               : "等待 PC 识别（设备管理器\n应出现 CMSIS-DAP）……";
    } else if (st == DAP_STATE_ERROR || wst == DAP_WIFI_ERROR) {
        hint = "启动失败，查看日志。读卡器\n占用 USB 时先关闭 CardR。";
    } else {
        hint = "USB 与无线二选一开启\nSWD1: 11/12/13 · SWD2: 14/15/18";
    }
    lv_label_set_text(dl->val_hint, hint);

    bool active = (st == DAP_STATE_READY);
    lv_label_set_text(dl->lbl_toggle, active ? "关闭 DAP" : "开启 DAP");
    if (active || st == DAP_STATE_OFF || st == DAP_STATE_ERROR) {
        lv_obj_remove_state(dl->btn_toggle, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(dl->btn_toggle, LV_STATE_DISABLED);
    }
    lv_label_set_text(dl->lbl_wifi, wst == DAP_WIFI_ON ? "关闭无线" : "开启无线");
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

static void dl_wifi_btn_cb(lv_event_t *e)
{
    struct dap_link *dl = lv_event_get_user_data(e);
    if (!dl) return;

    if (app_dap_wifi_get_state() == DAP_WIFI_ON) {
        app_dap_wifi_disable();
        esp_lv_adapter_lock(-1);
        app_display_restore_buffers();
        esp_lv_adapter_unlock();
        dl_update(dl);
        return;
    }

    /* WiFi 初始化（esp_wifi_init）要求调用任务栈在内部 RAM（cache freeze
     * 断言），而 LVGL 任务栈在 PSRAM——放到独立任务执行 */
    if (s_wifi_task) return;   /* 已在启动中 */
    xTaskCreate(dl_wifi_enable_task, "dap_wifi", 4096, NULL, 5, &s_wifi_task);
    dl_update(dl);
}

static void dl_wifi_enable_task(void *arg)
{
    (void)arg;
    /* WiFi 启动需要较多内部 RAM（实测仅剩 ~18KB，WiFi 需 ~40KB）：
     * 无线期间临时收缩显示缓冲（双→单，腾 ~15KB），关闭无线时
     * dl_wifi_btn_cb 自动恢复双缓冲——仅无线烧录期间生效，不影响日常显示 */
    esp_lv_adapter_lock(-1);
    app_display_shrink_buffers();
    esp_lv_adapter_unlock();
    app_dap_wifi_enable();
    s_wifi_task = NULL;
    vTaskDelete(NULL);
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

    dl_row(info, "SWD 接口", &dl->val_if);
    lv_label_set_text(dl->val_if,
                      "SWD1: 11/12/13 · SWD2: 14/15/18\nGND 共地（Keil 第 1 个 = SWD1）");
    dl_row(info, "状态", &dl->val_status);
    dl_row(info, "提示", &dl->val_hint);

    /* 底部：USB 开关 + 无线开关 */
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

    lv_obj_t *wbtn = lv_button_create(root);
    lv_obj_set_size(wbtn, 160, 34);
    lv_obj_align(wbtn, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_set_style_bg_color(wbtn, DL_CARD, 0);
    lv_obj_set_style_bg_opa(wbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wbtn, DL_ACCENT, 0);
    lv_obj_set_style_border_width(wbtn, 1, 0);
    lv_obj_set_style_radius(wbtn, 8, 0);
    lv_obj_set_style_text_color(wbtn, DL_TEXT, 0);
    lv_obj_set_style_text_font(wbtn, dl_font(), 0);
    lv_obj_add_event_cb(wbtn, dl_wifi_btn_cb, LV_EVENT_CLICKED, dl);
    dl->btn_wifi = wbtn;
    dl->lbl_wifi = lv_label_create(wbtn);
    lv_obj_center(dl->lbl_wifi);

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
    if (app_dap_wifi_get_state() == DAP_WIFI_ON) {
        ESP_LOGI("dap_link", "exit -> disable wireless DAP");
        app_dap_wifi_disable();
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
