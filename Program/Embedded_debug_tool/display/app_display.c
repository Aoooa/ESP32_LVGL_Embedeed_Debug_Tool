#include "app_display.h"
#include "drv_display.h"
#include "app_uart.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_display";

/* ── Ring Buffer ── */
#define RING_MAX_LINES   200
#define RING_LINE_LEN    32
#define RING_LINE_BUF    (RING_LINE_LEN + 1)

static char s_lines[RING_MAX_LINES][RING_LINE_BUF];
static int  s_line_idx;
static int  s_line_count;
static int  s_cur_col;

/* ── Layout ── */
#define SCREEN_W         320
#define SCREEN_H         240
#define STATUS_BAR_H     28
#define BTN_BAR_H        40
#define SEP_H            1
#define DISP_LINES       15

/* ── State ── */
static int s_active_uart;

/* ── Flags (button callbacks → display_task) ── */
static volatile int s_pending_clear;
static volatile int s_pending_uart_switch;
static TaskHandle_t s_display_task_h;

/* ── LVGL Objects ── */
static lv_obj_t *s_status_dot;
static lv_obj_t *s_status_uart;
static lv_obj_t *s_status_info;
static lv_obj_t *s_status_state;
static lv_obj_t *s_log_container;
static lv_obj_t *s_log_label;
static lv_obj_t *s_btn_uart;
static lv_obj_t *s_btn_pause;
static lv_obj_t *s_btn_pause_lbl;
static lv_obj_t *s_btn_clear;

static void ring_push_char(char c)
{
    if (c == '\n' || c == '\r') {
        if (s_cur_col > 0) {
            s_lines[s_line_idx][s_cur_col] = '\0';
            s_line_idx = (s_line_idx + 1) % RING_MAX_LINES;
            if (s_line_count < RING_MAX_LINES) s_line_count++;
            s_cur_col = 0;
        }
        return;
    }
    if (s_cur_col == 0) {
        memset(s_lines[s_line_idx], 0, RING_LINE_BUF);
    }
    if (s_cur_col < RING_LINE_LEN) {
        s_lines[s_line_idx][s_cur_col++] = c;
    }
}

static void ring_push_data(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        ring_push_char((char)data[i]);
    }
}

static void ring_clear(void)
{
    s_line_idx = 0;
    s_line_count = 0;
    s_cur_col = 0;
    memset(s_lines, 0, sizeof(s_lines));
}

/* ── Status Bar ── */

static void update_status_bar(void)
{
    uart_bridge_t *br = g_bridges[s_active_uart];

    lv_obj_set_style_bg_color(s_status_dot,
        br->paused ? lv_color_hex(0xF97316) : lv_color_hex(0x22C55E), 0);

    lv_label_set_text(s_status_uart, s_active_uart ? "2" : "1");

    char info[48];
    snprintf(info, sizeof(info), "192.168.4.1:%d TX:IO%d RX:IO%d",
             br->tcp_port, br->tx_pin, br->rx_pin);
    lv_label_set_text(s_status_info, info);

    lv_label_set_text(s_status_state, br->paused ? "PAUSED" : "RUN");
    lv_label_set_text(s_btn_pause_lbl, br->paused ? "Resume" : "Pause");
}

/* ── Log Label 刷新（在 LVGL lock 内调用） ── */

static void refresh_log_label(void)
{
    int total = s_line_count;
    int show = (total < DISP_LINES) ? total : DISP_LINES;
    if (show == 0) {
        lv_label_set_text(s_log_label, "");
        return;
    }

    int start = (s_line_idx - show + RING_MAX_LINES) % RING_MAX_LINES;
    static char s_disp_buf[DISP_LINES * (RING_LINE_LEN + 2)];
    int pos = 0;
    for (int i = 0; i < show; i++) {
        int idx = (start + i) % RING_MAX_LINES;
        int len = strlen(s_lines[idx]);
        if (pos + len + 1 < (int)sizeof(s_disp_buf)) {
            memcpy(s_disp_buf + pos, s_lines[idx], len);
            pos += len;
            s_disp_buf[pos++] = '\n';
        }
    }
    s_disp_buf[pos] = '\0';

    lv_label_set_text(s_log_label, s_disp_buf);
    lv_obj_scroll_to_y(s_log_container, LV_COORD_MAX, LV_ANIM_OFF);
}

/* ── Button Callbacks ── */

static void on_btn_uart(lv_event_t *e)
{
    (void)e;
    s_pending_uart_switch = 1;
    if (s_display_task_h) xTaskNotify(s_display_task_h, 0, eNoAction);
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
    if (s_display_task_h) xTaskNotify(s_display_task_h, 0, eNoAction);
}

/* ── UI Construction ── */

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_gap(scr, 0, 0);

    /* ── Status Bar ── */
    lv_obj_t *status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, SCREEN_W, STATUS_BAR_H);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0xF9FAFB), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_hor(status_bar, 8, 0);
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
    lv_obj_t *sep1 = lv_obj_create(scr);
    lv_obj_set_size(sep1, SCREEN_W, SEP_H);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(0xE5E7EB), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep1, 0, 0);

    /* ── Log Container ── */
    int log_h = SCREEN_H - STATUS_BAR_H - SEP_H * 2 - BTN_BAR_H;
    s_log_container = lv_obj_create(scr);
    lv_obj_set_size(s_log_container, SCREEN_W, log_h);
    lv_obj_set_flex_grow(s_log_container, 1);
    lv_obj_set_style_bg_color(s_log_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_log_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_log_container, 0, 0);
    lv_obj_set_style_radius(s_log_container, 0, 0);
    lv_obj_set_style_pad_all(s_log_container, 4, 0);
    lv_obj_set_scroll_dir(s_log_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_log_container, LV_SCROLLBAR_MODE_OFF);

    s_log_label = lv_label_create(s_log_container);
    lv_obj_set_width(s_log_label, SCREEN_W - 8);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_log_label, "");
    lv_obj_set_style_text_color(s_log_label, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(s_log_label, 2, 0);

    /* ── Separator ── */
    lv_obj_t *sep2 = lv_obj_create(scr);
    lv_obj_set_size(sep2, SCREEN_W, SEP_H);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(0xE5E7EB), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);

    /* ── Button Bar ── */
    lv_obj_t *btn_bar = lv_obj_create(scr);
    lv_obj_set_size(btn_bar, SCREEN_W, BTN_BAR_H);
    lv_obj_set_style_bg_color(btn_bar, lv_color_hex(0xF9FAFB), 0);
    lv_obj_set_style_bg_opa(btn_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(btn_bar, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(btn_bar, 0, 0);
    lv_obj_set_style_pad_hor(btn_bar, 8, 0);
    lv_obj_set_style_pad_ver(btn_bar, 4, 0);
    lv_obj_set_flex_flow(btn_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_bar, 6, 0);

    int btn_w = (SCREEN_W - 16 - 12) / 3;
    int btn_h = BTN_BAR_H - 8;

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
}

/* ── Display Task: 收到数据立刻刷新 ── */

static void display_task(void *arg)
{
    (void)arg;
    disp_item_t item;

    while (1) {
        /* 等待队列数据，超时 50ms 以便及时响应按钮 */
        int got_data = (xQueueReceive(g_display_queue, &item, pdMS_TO_TICKS(50)) == pdTRUE);
        if (got_data) {
            if (item.uart_idx == s_active_uart) {
                ring_push_data(item.data, item.len);
            }
            /* 一次性排空队列中所有积压数据 */
            while (xQueueReceive(g_display_queue, &item, 0) == pdTRUE) {
                if (item.uart_idx == s_active_uart) {
                    ring_push_data(item.data, item.len);
                }
            }
        }

        /* 清除通知（按钮回调可能已发出） */
        xTaskNotifyWait(0, 0, NULL, 0);

        /* 处理按钮请求 */
        int need_status = 0;
        if (s_pending_clear) {
            ring_clear();
            need_status = 1;
        }
        if (s_pending_uart_switch) {
            s_active_uart = !s_active_uart;
            ring_clear();
            need_status = 1;
        }

        if (got_data || need_status) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                refresh_log_label();
                if (need_status) update_status_bar();
                esp_lv_adapter_unlock();
            }
        }

        s_pending_clear = 0;
        s_pending_uart_switch = 0;
    }
}

/* ── Touch rotation callback (CST816S raw → rotated 90° CW) ── */

static esp_err_t touch_rotated_read(esp_lcd_touch_handle_t tp,
                                     esp_lcd_touch_point_data_t *points,
                                     uint8_t *count, uint8_t max_count,
                                     void *user_ctx)
{
    (void)user_ctx;
    esp_lcd_touch_read_data(tp);
    esp_err_t ret = esp_lcd_touch_get_data(tp, points, count, max_count);
    if (ret == ESP_OK) {
        for (uint8_t i = 0; i < *count; i++) {
            int16_t raw_x = points[i].x;
            int16_t raw_y = points[i].y;
            points[i].x = raw_y;
            points[i].y = (DRV_LCD_H_RES - 1) - raw_x;
        }
    }
    return ret;
}

/* ── Public API ── */

void app_display_start(void)
{
    drv_display_t disp = {0};
    drv_display_init(&disp);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
            disp.panel, disp.io,
            DRV_LCD_V_RES, DRV_LCD_H_RES,
            ESP_LV_ADAPTER_ROTATE_0);
    lv_display_t *lv_disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(lv_disp != NULL);

    esp_lv_adapter_touch_config_t tp_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lv_disp, disp.touch);
    tp_cfg.callbacks.custom_touch_read = touch_rotated_read;
    lv_indev_t *lv_tp = esp_lv_adapter_register_touch(&tp_cfg);
    assert(lv_tp != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    s_active_uart = 0;
    ring_clear();

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        build_ui();
        update_status_bar();
        esp_lv_adapter_unlock();
    }

    xTaskCreate(display_task, "disp_task", 4096, NULL, 5, &s_display_task_h);
    ESP_LOGI(TAG, "Serial monitor UI ready");
}

void app_display_set_info(const char *ip, int uart1_port, int uart2_port)
{
    (void)ip;
    (void)uart1_port;
    (void)uart2_port;
}

void app_display_notify_status(void)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        update_status_bar();
        esp_lv_adapter_unlock();
    }
}
