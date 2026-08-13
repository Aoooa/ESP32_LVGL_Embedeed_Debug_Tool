#include "app_display.h"
#include "drv_display.h"
#include "app_uart.h"
#include "esp_lv_adapter.h"
#include "flow_view.h"
#include "file_browser.h"
#include "app_font.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

/* ── 界面开关（代码保留，宏控制） ── */
#ifndef APP_UI_TERMINAL_ENABLED
#define APP_UI_TERMINAL_ENABLED 0   /* 1=串口终端界面，0=SD 文件浏览器 */
#endif

static const char *TAG = "app_display";

/* ── flow_view 内存/锁钩子 ── */
static void *fv_psram_alloc(size_t size)
{
    return heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void fv_acquire(void)
{
    esp_lv_adapter_lock(-1);
}

/* ── Layout ── */
#define SCREEN_W         320
#define SCREEN_H         240
#define STATUS_BAR_H     28
#define BTN_BAR_H        40
#define SEP_H            1

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
static lv_obj_t *s_log_view;
static lv_obj_t *s_file_browser;
static lv_obj_t *s_btn_uart;
static lv_obj_t *s_btn_pause;
static lv_obj_t *s_btn_pause_lbl;
static lv_obj_t *s_btn_clear;

/* ── Status Bar ── */

static void update_status_bar(void)
{
#if APP_UI_TERMINAL_ENABLED
    uart_bridge_t *br = g_bridges[s_active_uart];

    lv_obj_set_style_bg_color(s_status_dot,
        br->paused ? lv_color_hex(0xF97316) : lv_color_hex(0x22C55E), 0);

    lv_label_set_text(s_status_uart, s_active_uart ? "2" : "1");

    char info[48];
    snprintf(info, sizeof(info), "TX:IO%d RX:IO%d",
             br->tx_pin, br->rx_pin);
    lv_label_set_text(s_status_info, info);

    lv_label_set_text(s_status_state, br->paused ? "PAUSED" : "RUN");
    lv_label_set_text(s_btn_pause_lbl, br->paused ? "Resume" : "Pause");
#endif
}

/* ── 硬件滚动接口（保留：驱动层 0x33/0x37 命令；完整滚动（flush 映射）留待未来优化） ──
 * 注意：需组合 A（硬件竖屏 swap=false）时 0x37 轴 = 屏幕垂直。
 * app_display_hw_scroll_set：同步 0x37 偏移；app_display_hw_scroll_enable：设置滚动区。
 * 当前阅读器未启用硬件滚动（软件滚动），接口供后续优化使用。 */
void app_display_hw_scroll_set(int offset, int height)
{
    int h = height > 0 ? height : 240;
    drv_display_set_scroll_start(offset % h);
}

void app_display_hw_scroll_enable(bool en)
{
    if (en) {
        drv_display_set_scroll_area(0, 240);
    }
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
#if APP_UI_TERMINAL_ENABLED
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

    /* ── Log Container（flow_view 滚动内容流视图） ── */
    int log_h = SCREEN_H - STATUS_BAR_H - SEP_H * 2 - BTN_BAR_H;
    s_log_container = lv_obj_create(scr);
    lv_obj_set_size(s_log_container, SCREEN_W, log_h);
    lv_obj_set_flex_grow(s_log_container, 1);
    lv_obj_set_style_bg_color(s_log_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_log_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_log_container, 0, 0);
    lv_obj_set_style_radius(s_log_container, 0, 0);
    lv_obj_set_style_pad_all(s_log_container, 0, 0);
    lv_obj_clear_flag(s_log_container, LV_OBJ_FLAG_SCROLLABLE);

    s_log_view = flow_view_create(s_log_container);
    lv_obj_align(s_log_view, LV_ALIGN_TOP_LEFT, 0, 0);

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

#else
    /* SD 文件浏览器界面（终端 UI 代码保留，由宏控制切换） */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_file_browser = file_browser_create(scr);
#endif
}

/* ── Display Task: 收到数据立刻刷新 ── */

static void display_task(void *arg)
{
    (void)arg;
    disp_item_t item;

    while (1) {
        /* 等待队列数据，短超时以便及时响应按钮 */
        int got_data = (xQueueReceive(g_display_queue, &item, pdMS_TO_TICKS(10)) == pdTRUE);
        if (got_data) {
#if APP_UI_TERMINAL_ENABLED
            /* flow_view_append 内部加锁（模型更新），渲染由组件定时器在 LVGL 线程完成 */
            if (item.uart_idx == s_active_uart) {
                flow_view_append(s_log_view, (const char *)item.data, item.len);
            }
            while (xQueueReceive(g_display_queue, &item, 0) == pdTRUE) {
                if (item.uart_idx == s_active_uart) {
                    flow_view_append(s_log_view, (const char *)item.data, item.len);
                }
            }
#endif
        }

        /* 清除通知（按钮回调可能已发出） */
        xTaskNotifyWait(0, 0, NULL, 0);

#if APP_UI_TERMINAL_ENABLED
        /* 处理按钮请求 */
        int need_status = 0;
        if (s_pending_clear || s_pending_uart_switch) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                if (s_pending_clear) {
                    flow_view_clear(s_log_view);
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
#endif
    }
}

/*
 * 触摸坐标旋转：CST816S 原始坐标 → LVGL 逻辑坐标。
 * 按当前 LVGL 分辨率自动选择映射（横竖屏通用）：
 * - 横屏（320x240）：x = (V_RES-1) - raw_y, y = raw_x（CST816S 竖屏原始坐标旋转）
 * - 竖屏（240x320）：直连（raw_x, raw_y）
 */
static esp_err_t touch_rotated_read(esp_lcd_touch_handle_t tp,
                                     esp_lcd_touch_point_data_t *points,
                                     uint8_t *count, uint8_t max_count,
                                     void *user_ctx)
{
    (void)user_ctx;
    (void)max_count;

    uint16_t raw_x = 0, raw_y = 0;
    uint8_t cnt = 0;

    esp_lcd_touch_read_data(tp);
    esp_lcd_touch_get_coordinates(tp, &raw_x, &raw_y, NULL, &cnt, 1);

    if (cnt > 0) {
        *count = 1;
        lv_display_t *d = lv_display_get_default();
        bool landscape = d && lv_display_get_horizontal_resolution(d) > lv_display_get_vertical_resolution(d);
        if (landscape) {
            /* 横屏逻辑 + 驱动旋转 270（交叉组合）：LVGL x = (V_RES-1) - raw_y, y = raw_x */
            points[0].x = (DRV_LCD_V_RES - 1) - raw_y;
            points[0].y = raw_x;
        } else {
            points[0].x = raw_x;
            points[0].y = raw_y;
        }
        points[0].strength = 1;
    } else {
        *count = 0;
    }
    return ESP_OK;
}

/* ── Public API ── */

void app_display_start(void)
{
    drv_display_t disp = {0};
    drv_display_init(&disp);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_CONFIG(
            disp.panel, disp.io,
            ESP_LV_ADAPTER_DISPLAY_PROFILE_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
                APP_DISPLAY_ORIENTATION ? DRV_LCD_V_RES : DRV_LCD_H_RES,
                APP_DISPLAY_ORIENTATION ? DRV_LCD_H_RES : DRV_LCD_V_RES,
                ESP_LV_ADAPTER_ROTATE_0),
            ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
            ESP_LV_ADAPTER_TE_SYNC_DISABLED());
    /* PSRAM 缓冲会让 spi_master 在 ISR 中分配内部 DMA priv buffer（每次 flush），
     * 实测分配失败（ESP_ERR_NO_MEM）→ 显示中断。改用内部 RAM 双缓冲。
     * WIFI/LWIP 缓冲已放 PSRAM（CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP），
     * 40 行双缓冲 = 320×40×2×2 = 51.2KB 内部 RAM */
    disp_cfg.profile.buffer_height = 40;
    disp_cfg.profile.require_double_buffer = true;
    lv_display_t *lv_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (lv_disp == NULL) {   /* 内存不足时回退到 24 行 */
        disp_cfg.profile.buffer_height = 24;
        lv_disp = esp_lv_adapter_register_display(&disp_cfg);
    }
    assert(lv_disp != NULL);

    /* 编译期方向（宏 APP_DISPLAY_ORIENTATION）：竖屏 = swap(false)，横屏 = swap(true)。
     * 使用 adapter 原 flush（颜色字节序正确）；注册后重设硬件旋转（adapter 可能 reset panel）。 */
    drv_display_set_hw_rotation(APP_DISPLAY_ORIENTATION ? true : false, false,
                                APP_DISPLAY_ORIENTATION ? true : false);

    esp_lv_adapter_touch_config_t tp_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lv_disp, disp.touch);
    tp_cfg.callbacks.custom_touch_read = touch_rotated_read;
    lv_indev_t *lv_tp = esp_lv_adapter_register_touch(&tp_cfg);
    assert(lv_tp != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    s_active_uart = 0;

    /* flow_view 内存钩子：位图/行历史放 PSRAM（纯 CPU 访问，无 DMA 限制） */
    flow_view_malloc = fv_psram_alloc;
    flow_view_free = heap_caps_free;

    /* flow_view 锁钩子：与 LVGL 渲染共享同一把递归锁 */
    static const flow_view_lock_t fv_lock_cb = {
        .lock = fv_acquire,
        .unlock = esp_lv_adapter_unlock,
    };
    flow_view_set_lock(&fv_lock_cb);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        build_ui();
#if APP_UI_TERMINAL_ENABLED
        update_status_bar();
#endif
        esp_lv_adapter_unlock();
    }

    /* 任务栈放 PSRAM（内部 RAM 被显示双缓冲占用） */
    BaseType_t ok = xTaskCreateWithCaps(display_task, "disp_task", 32768, NULL, 5, &s_display_task_h, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "disp_task create FAILED, free heap=%u",
                 (unsigned)esp_get_free_heap_size());
    }
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

/* SD 挂载后异步加载中文字体（LVGL 线程执行，加载完成后重绘列表显示中文） */
static void sd_font_retry_timer_cb(lv_timer_t *t)
{
    (void)t;
    app_font_retry();
    app_font_get(16);
    file_browser_refresh(s_file_browser);
}

void app_display_notify_sd_ready(void)
{
#if !APP_UI_TERMINAL_ENABLED
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        /* 列表先刷新可用（"SD not ready"立即消失）；
         * 字体 2.3MB 从 SD 读取较慢，移到 LVGL 定时器异步加载，不阻塞本锁 */
        file_browser_refresh(s_file_browser);
        lv_timer_t *t = lv_timer_create(sd_font_retry_timer_cb, 1, NULL);
        lv_timer_set_repeat_count(t, 1);
        lv_timer_set_auto_delete(t, true);
        esp_lv_adapter_unlock();
    }
#else
    (void)0;
#endif
}
