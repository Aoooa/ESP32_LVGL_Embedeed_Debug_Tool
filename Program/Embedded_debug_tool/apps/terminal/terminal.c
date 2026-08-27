/* terminal.c —— 串口终端 APP（LVGL 9）：状态栏 + flow_view 日志 + UART 切换/Pause/Clear 按钮。
 * terminal_task 消费 UART 数据队列（g_display_queue）刷新日志，仅显示活跃 UART。 */

#include "terminal.h"
#include "flow_view.h"
#include "app_uart.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

#define T_STATUS_BAR_H   28
#define T_BTN_BAR_H      40
#define T_SEP_H          1

static const char *TAG = "terminal";

/* ── 状态（terminal_task + LVGL 线程共享，volatile 标志 + 锁保护） ── */
static int s_active_uart;
static volatile int s_pending_clear;
static volatile int s_pending_uart_switch;
static TaskHandle_t s_terminal_task_h;

/* ── LVGL Objects（terminal_create/destroy 管理） ── */
static lv_obj_t *s_status_dot;
static lv_obj_t *s_status_uart;
static lv_obj_t *s_status_info;
static lv_obj_t *s_status_state;
static lv_obj_t *s_log_container;
static lv_obj_t *s_log_view;
static lv_obj_t *s_btn_uart;
static lv_obj_t *s_btn_pause;
static lv_obj_t *s_btn_pause_lbl;
static lv_obj_t *s_btn_clear;

/* ── Status Bar ── */

static void update_status_bar(void)
{
    if (!s_status_dot) return;
    uart_bridge_t *br = g_bridges[s_active_uart];

    lv_obj_set_style_bg_color(s_status_dot,
        br->paused ? lv_color_hex(0xF97316) : lv_color_hex(0x22C55E), 0);

    lv_label_set_text(s_status_uart, s_active_uart ? "2" : "1");

    char info[48];
    snprintf(info, sizeof(info), "TX:IO%d RX:IO%d", br->tx_pin, br->rx_pin);
    lv_label_set_text(s_status_info, info);

    lv_label_set_text(s_status_state, br->paused ? "PAUSED" : "RUN");
    lv_label_set_text(s_btn_pause_lbl, br->paused ? "Resume" : "Pause");
}

/* ── Button Callbacks ── */

static void on_btn_uart(lv_event_t *e)
{
    (void)e;
    s_pending_uart_switch = 1;
    if (s_terminal_task_h) xTaskNotify(s_terminal_task_h, 0, eNoAction);
}

static void on_btn_pause(lv_event_t *e)
{
    (void)e;
    uart_bridge_t *br = g_bridges[s_active_uart];
    br->paused = !br->paused;
    update_status_bar();
}

static void on_btn_clear(lv_event_t *e)
{
    (void)e;
    s_pending_clear = 1;
    if (s_terminal_task_h) xTaskNotify(s_terminal_task_h, 0, eNoAction);
}

/* ── UI 构建 ── */

lv_obj_t *terminal_create(lv_obj_t *parent, terminal_back_cb_t back_cb, void *ctx)
{
    (void)back_cb;
    (void)ctx;
    /* 尺寸跟随当前逻辑分辨率（横/竖屏通用） */
    int sw = lv_display_get_horizontal_resolution(lv_display_get_default());
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_gap(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Status Bar ── */
    lv_obj_t *status_bar = lv_obj_create(root);
    lv_obj_set_size(status_bar, sw, T_STATUS_BAR_H);
    lv_obj_set_style_pad_hor(status_bar, 8, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0xF9FAFB), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_ver(status_bar, 6, 0);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(status_bar, 6, 0);

    s_status_dot = lv_obj_create(status_bar);
    lv_obj_set_size(s_status_dot, 8, 8);
    lv_obj_set_style_radius(s_status_dot, 4, 0);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);

    s_status_uart = lv_label_create(status_bar);
    lv_label_set_text(s_status_uart, "1");
    lv_obj_set_style_text_color(s_status_uart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(s_status_uart, &lv_font_montserrat_12, 0);

    s_status_info = lv_label_create(status_bar);
    lv_label_set_text(s_status_info, "");
    lv_obj_set_style_text_color(s_status_info, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_text_font(s_status_info, &lv_font_montserrat_10, 0);

    lv_obj_t *spacer = lv_obj_create(status_bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    s_status_state = lv_label_create(status_bar);
    lv_label_set_text(s_status_state, "RUN");
    lv_obj_set_style_text_color(s_status_state, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_text_font(s_status_state, &lv_font_montserrat_12, 0);

    /* ── Separator ── */
    lv_obj_t *sep1 = lv_obj_create(root);
    lv_obj_set_size(sep1, sw, T_SEP_H);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(0xE5E7EB), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep1, 0, 0);

    /* ── Log Container（flow_view 滚动内容流视图） ── */
    int log_h = sh - T_STATUS_BAR_H - T_SEP_H * 2 - T_BTN_BAR_H;
    s_log_container = lv_obj_create(root);
    lv_obj_set_size(s_log_container, sw, log_h);
    lv_obj_set_flex_grow(s_log_container, 1);
    lv_obj_set_style_bg_color(s_log_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_log_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_log_container, 0, 0);
    lv_obj_set_style_radius(s_log_container, 0, 0);
    lv_obj_set_style_pad_all(s_log_container, 0, 0);
    lv_obj_clear_flag(s_log_container, LV_OBJ_FLAG_SCROLLABLE);

    s_log_view = flow_view_create(s_log_container);
    lv_obj_align(s_log_view, LV_ALIGN_TOP_LEFT, 0, 0);
    /* 视口行数 = 容器高 / 默认行高（14px）：竖屏 320 高 → 17 行；横屏 240 → 12 行，
     * 避免默认 12 行在竖屏下只占容器一半（底部留白） */
    flow_view_set_visible_lines(s_log_view, log_h / FLOW_VIEW_LINE_HEIGHT_DEF);

    /* ── Separator ── */
    lv_obj_t *sep2 = lv_obj_create(root);
    lv_obj_set_size(sep2, sw, T_SEP_H);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(0xE5E7EB), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);

    /* ── Button Bar ── */
    lv_obj_t *btn_bar = lv_obj_create(root);
    lv_obj_set_size(btn_bar, sw, T_BTN_BAR_H);
    lv_obj_set_style_bg_color(btn_bar, lv_color_hex(0xF9FAFB), 0);
    lv_obj_set_style_bg_opa(btn_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(btn_bar, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(btn_bar, 0, 0);
    lv_obj_set_style_pad_hor(btn_bar, 8, 0);
    lv_obj_set_style_pad_ver(btn_bar, 4, 0);
    lv_obj_set_flex_flow(btn_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_bar, 6, 0);

    int btn_w = (sw - 16 - 12) / 3;
    int btn_h = T_BTN_BAR_H - 8;

    static lv_style_t btn_style;
    lv_style_init(&btn_style);
    lv_style_set_bg_color(&btn_style, lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&btn_style, LV_OPA_COVER);
    lv_style_set_border_color(&btn_style, lv_color_hex(0xD1D5DB));
    lv_style_set_border_width(&btn_style, 1);
    lv_style_set_radius(&btn_style, 6);
    lv_style_set_text_color(&btn_style, lv_color_hex(0x374151));
    lv_style_set_text_font(&btn_style, &lv_font_montserrat_12);

    /* UART 切换按钮 */
    s_btn_uart = lv_button_create(btn_bar);
    lv_obj_add_style(s_btn_uart, &btn_style, 0);
    lv_obj_set_size(s_btn_uart, btn_w, btn_h);
    lv_obj_set_style_min_width(s_btn_uart, btn_w, 0);
    lv_obj_t *lbl_uart = lv_label_create(s_btn_uart);
    lv_label_set_text(lbl_uart, "UART");
    lv_obj_set_width(lbl_uart, btn_w - 4);
    lv_obj_center(lbl_uart);
    lv_obj_add_event_cb(s_btn_uart, on_btn_uart, LV_EVENT_CLICKED, NULL);

    /* 暂停按钮 */
    s_btn_pause = lv_button_create(btn_bar);
    lv_obj_add_style(s_btn_pause, &btn_style, 0);
    lv_obj_set_size(s_btn_pause, btn_w, btn_h);
    lv_obj_set_style_min_width(s_btn_pause, btn_w, 0);
    s_btn_pause_lbl = lv_label_create(s_btn_pause);
    lv_label_set_text(s_btn_pause_lbl, "Pause");
    lv_obj_set_width(s_btn_pause_lbl, btn_w - 4);
    lv_obj_center(s_btn_pause_lbl);
    lv_obj_add_event_cb(s_btn_pause, on_btn_pause, LV_EVENT_CLICKED, NULL);

    /* 清空按钮 */
    s_btn_clear = lv_button_create(btn_bar);
    lv_obj_add_style(s_btn_clear, &btn_style, 0);
    lv_obj_set_size(s_btn_clear, btn_w, btn_h);
    lv_obj_set_style_min_width(s_btn_clear, btn_w, 0);
    lv_obj_t *lbl_clear = lv_label_create(s_btn_clear);
    lv_label_set_text(lbl_clear, "Clear");
    lv_obj_set_width(lbl_clear, btn_w - 4);
    lv_obj_center(lbl_clear);
    lv_obj_add_event_cb(s_btn_clear, on_btn_clear, LV_EVENT_CLICKED, NULL);

    update_status_bar();
    ESP_LOGI(TAG, "terminal UI created (%dx%d)", sw, sh);
    return root;
}

void terminal_destroy(lv_obj_t *root)
{
    /* UART 惰性占用：终端关闭 → 卸载桥接驱动、释放 IO2/4/16/17 */
    app_uart_stop();
    s_log_view = NULL;
    s_log_container = NULL;
    s_status_dot = NULL;
    s_status_uart = NULL;
    s_status_info = NULL;
    s_status_state = NULL;
    s_btn_uart = NULL;
    s_btn_pause = NULL;
    s_btn_clear = NULL;
    s_btn_pause_lbl = NULL;
    if (root) {
        /* 闪屏修复：先隐藏 + 立即刷新一帧（露出下层），再删除全屏对象 */
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(root);
    }
}

/* 右滑返回（launcher 分发）：无内部分级，直接请求关闭回桌面 */
bool terminal_swipe_back(lv_obj_t *root)
{
    (void)root;
    return true;
}

/* 进入完成（launcher 回调）：惰性占用 UART 引脚（驱动装载 + 开始转发）。
 * 上电不占 IO2/4/16/17；仅终端打开期间占用，关闭时 terminal_destroy 释放 */
void terminal_entered(void *app)
{
    (void)app;
    app_uart_start();
    ESP_LOGI(TAG, "terminal entered: UART1/2 drivers loaded");
}

/* 调试事件（测试模块用）：打印终端状态（活跃 UART/暂停/日志行数）供验证 */
void terminal_debug_event(lv_obj_t *root, int evt)
{
    (void)root;
    int lines = s_log_view ? flow_view_get_line_count(s_log_view) : -1;
    uart_bridge_t *br = g_bridges[s_active_uart];
    ESP_LOGI(TAG, "[DBG] evt=%d uart=%d paused=%d lines=%d",
             evt, s_active_uart + 1, br ? (int)br->paused : -1, lines);
}

/* 状态栏刷新（外部通知，如 WebSocket 消息后；无 UI 时无操作） */
void terminal_notify_status(void)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        update_status_bar();
        esp_lv_adapter_unlock();
    }
}

/* ── Terminal Task: 消费 UART 数据队列 → 刷新日志 ── */

static void terminal_task(void *arg)
{
    (void)arg;
    disp_item_t item;

    while (1) {
        /* 等待队列数据，短超时以便及时响应按钮 */
        int got_data = (xQueueReceive(g_display_queue, &item, pdMS_TO_TICKS(10)) == pdTRUE);
        if (got_data) {
            /* flow_view_append 内部加锁（模型更新），渲染由组件定时器在 LVGL 线程完成 */
            if (item.uart_idx == s_active_uart && s_log_view) {
                flow_view_append(s_log_view, (const char *)item.data, item.len);
            }
            while (xQueueReceive(g_display_queue, &item, 0) == pdTRUE) {
                if (item.uart_idx == s_active_uart && s_log_view) {
                    flow_view_append(s_log_view, (const char *)item.data, item.len);
                }
            }
        }

        /* 清除通知（按钮回调可能已发出） */
        xTaskNotifyWait(0, 0, NULL, 0);

        /* 处理按钮请求 */
        int need_status = 0;
        if (s_pending_clear || s_pending_uart_switch) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                if (s_pending_clear) {
                    if (s_log_view) flow_view_clear(s_log_view);
                    need_status = 1;
                }
                if (s_pending_uart_switch) {
                    s_active_uart = !s_active_uart;
                    flow_view_clear(s_log_view);
                    need_status = 1;
                }
                if (need_status) update_status_bar();
                esp_lv_adapter_unlock();
            }
        }

        s_pending_clear = 0;
        s_pending_uart_switch = 0;
    }
}

/* ── 初始化（平台启动时调用，任务常驻） ── */

void terminal_init(void)
{
    s_active_uart = 0;
    BaseType_t ok = xTaskCreateWithCaps(terminal_task, "term_task", 32768, NULL, 5,
                                        &s_terminal_task_h, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "terminal_task create FAILED");
    }
    ESP_LOGI(TAG, "terminal init (task %s)", ok == pdPASS ? "ok" : "FAILED");
}
