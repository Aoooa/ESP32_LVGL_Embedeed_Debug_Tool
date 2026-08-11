#include "app_display.h"
#include "drv_display.h"
#include "app_uart.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_display";

/* ── Canvas 终端 ──
 * 正常模式（跟随底部）：数据先解析到 pending 行缓冲（锁外），攒批后
 * memmove 位图窗口 + 批量绘制（锁内），只失效底部行带 —— 93FPS 验证过的路径。
 * 触摸拖动：切到历史模式，从行文本历史（PSRAM）全量重绘窗口。
 * 不调用 esp_cache_msync：纯 CPU 读写 cache 自动一致（官方源码确认 sw 路径无 cache 操作）。 */
#define TERM_LINE_H      14      /* montserrat_12 行高 */
#define TERM_CURLINE_MAX 60      /* 当前行字符上限 */
#define TERM_VISIBLE_MAX 12      /* 可见行数（12×14=168px） */
#define TERM_HISTORY_MAX 256     /* 历史行数（环形文本） */

static lv_obj_t *s_term_canvas;
static uint8_t *s_term_buf;
static char   (*s_hist)[TERM_CURLINE_MAX + 1];
static char     s_cur_line[TERM_CURLINE_MAX];
static int      s_cur_len;
static int32_t  s_cur_w;
static int      s_row_count;
static int      s_new_lines;
static int      s_view_top;
static int      s_follow = 1;
static int      s_touch_y;

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
static lv_obj_t *s_fps_label;
static lv_obj_t *s_scrollbar;
static lv_obj_t *s_btn_uart;
static lv_obj_t *s_btn_pause;
static lv_obj_t *s_btn_pause_lbl;
static lv_obj_t *s_btn_clear;

/* ── Status Bar ── */

static void update_status_bar(void)
{
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
}

static void term_update_scrollbar(void);   /* 前向声明 */

static void term_clear(void)
{
    memset(s_term_buf, 0xFF, (size_t)SCREEN_W * TERM_VISIBLE_MAX * TERM_LINE_H * 2);
    s_cur_len = 0;
    s_cur_w = 0;
    s_row_count = 0;
    s_new_lines = 0;
    s_view_top = 0;
    s_follow = 1;
    lv_obj_invalidate(s_term_canvas);
    if (s_scrollbar) term_update_scrollbar();
}

/* 把一行文本绘制到 canvas 的 y 行（LVGL lock 内） */
static void term_draw_line(const char *text, int y)
{
    if (text[0] == '\0') return;

    lv_layer_t layer;
    lv_canvas_init_layer(s_term_canvas, &layer);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = &lv_font_montserrat_12;
    dsc.color = lv_color_hex(0x1F2937);
    dsc.text = text;
    lv_area_t coords = { 4, y, SCREEN_W - 4, y + TERM_LINE_H - 1 };
    lv_draw_label(&layer, &dsc, &coords);
    /* finish_layer 内部会 lv_obj_invalidate(canvas) 全幅失效——抑制后手动精确失效 */
    lv_display_enable_invalidation(lv_display_get_default(), false);
    lv_canvas_finish_layer(s_term_canvas, &layer);
    lv_display_enable_invalidation(lv_display_get_default(), true);
}

/* 位图窗口上移 n 行（锁外：纯内存操作）。
 * 注意：n 可能超过窗口行数（一批多行），此时窗口直接清空重来 */
static void term_scroll(int n)
{
    size_t row_bytes = (size_t)SCREEN_W * 2;
    size_t win_h = (size_t)TERM_VISIBLE_MAX * TERM_LINE_H;
    if (n >= TERM_VISIBLE_MAX) {
        memset(s_term_buf, 0xFF, win_h * row_bytes);
        return;
    }
    size_t rows = (size_t)n * TERM_LINE_H;
    memmove(s_term_buf, s_term_buf + rows * row_bytes, (win_h - rows) * row_bytes);
    memset(s_term_buf + (win_h - rows) * row_bytes, 0xFF, rows * row_bytes);
}

/* 更新右侧滚动指示条（锁内） */
static void term_update_scrollbar(void)
{
    int max_top = s_row_count - TERM_VISIBLE_MAX;
    if (max_top <= 0 || s_row_count <= TERM_VISIBLE_MAX) {
        lv_obj_add_flag(s_scrollbar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_scrollbar, LV_OBJ_FLAG_HIDDEN);
    int vis_h = TERM_VISIBLE_MAX * TERM_LINE_H;
    int thumb_h = vis_h * vis_h / (s_row_count * TERM_LINE_H);
    if (thumb_h < 8) thumb_h = 8;
    if (thumb_h > vis_h) thumb_h = vis_h;
    int y = (vis_h - thumb_h) * s_view_top / max_top;
    lv_obj_set_size(s_scrollbar, 4, thumb_h);
    lv_obj_set_pos(s_scrollbar, SCREEN_W - 4, y);
}

/* 锁内：从 s_hist 全量重绘可见窗口（最近 TERM_VISIBLE_MAX 行）。
 * 不用 memmove 增量：memmove 改变整个窗口内容，必须全量重绘才正确。
 * 文本源唯一（s_hist），无跨批状态。 */
static void term_render(void)
{
    int n = s_new_lines;
    if (n <= 0) return;

    memset(s_term_buf, 0xFF, (size_t)SCREEN_W * TERM_VISIBLE_MAX * TERM_LINE_H * 2);
    int start = s_row_count - TERM_VISIBLE_MAX;
    if (start < 0) start = 0;
    for (int k = 0; k < TERM_VISIBLE_MAX; k++) {
        if (start + k >= s_row_count) break;
        term_draw_line(s_hist[(start + k) % TERM_HISTORY_MAX], k * TERM_LINE_H);
    }
    lv_obj_invalidate(s_term_canvas);

    s_new_lines = 0;
    s_view_top = s_row_count - TERM_VISIBLE_MAX;
    if (s_view_top < 0) s_view_top = 0;
    term_update_scrollbar();
}

/* 从文本历史重绘整个可见窗口（触摸滚动历史时调用，LVGL lock 内） */
static void term_redraw_window(void)
{
    memset(s_term_buf, 0xFF, (size_t)SCREEN_W * TERM_VISIBLE_MAX * TERM_LINE_H * 2);
    for (int k = 0; k < TERM_VISIBLE_MAX; k++) {
        int row = s_view_top + k;
        if (row < 0 || row >= s_row_count) continue;
        term_draw_line(s_hist[row % TERM_HISTORY_MAX], k * TERM_LINE_H);
    }
    lv_obj_invalidate(s_term_canvas);
    term_update_scrollbar();
}

/* 触摸拖动查看历史（canvas 事件，LVGL 线程内） */
/* 触摸拖动查看历史（canvas 事件，LVGL 线程内）。
 * 拖动灵敏度：7px = 1 行（LINE_H/2）。滑到最底部立即恢复跟随。 */
static void on_term_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_touch_y = p.y;
    } else if (code == LV_EVENT_PRESSING) {
        int dy = p.y - s_touch_y;
        s_touch_y = p.y;
        s_follow = 0;
        s_view_top -= dy * 2 / TERM_LINE_H;
        int max_top = s_row_count - TERM_VISIBLE_MAX;
        if (max_top < 0) max_top = 0;
        if (s_view_top < 0) s_view_top = 0;
        if (s_view_top > max_top) s_view_top = max_top;
        if (s_view_top >= max_top) s_follow = 1;   /* 滑到底 → 恢复跟随 */
        term_redraw_window();
    } else if (code == LV_EVENT_RELEASED) {
        if (s_view_top >= s_row_count - TERM_VISIBLE_MAX) {
            s_follow = 1;
            s_new_lines = 0;
            term_redraw_window();   /* 回到最新 */
        }
    }
}

/* 完成一行（锁外：只写历史，不碰 LVGL） */
static void term_newline(void)
{
    if (s_cur_len > 0) {
        memcpy(s_hist[s_row_count % TERM_HISTORY_MAX], s_cur_line, s_cur_len);
        s_hist[s_row_count % TERM_HISTORY_MAX][s_cur_len] = '\0';
        s_row_count++;
        s_new_lines++;
    }
    s_cur_len = 0;
    s_cur_w = 0;
}

/* 数据逐字符解析（锁外调用：只读字体宽度 + 写历史，不碰 LVGL API） */
static void term_process(const uint8_t *data, int len)
{
    const lv_font_t *font = &lv_font_montserrat_12;
    for (int i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            term_newline();
            continue;
        }
        if (c < 0x20) continue;   /* 其他控制字符跳过 */
        if (s_cur_len >= TERM_CURLINE_MAX) term_newline();
        uint32_t next = (uint32_t)(unsigned char)data[i + 1];
        uint16_t w = lv_font_get_glyph_width(font, (uint32_t)(unsigned char)c, next);
        if (w == 0) continue;
        if (s_cur_w + (int32_t)w > SCREEN_W - 8) term_newline();
        s_cur_line[s_cur_len++] = c;
        s_cur_w += w;
    }
}

/* ── FPS 角标 ── */

static void fps_timer_cb(lv_timer_t *timer)
{
    (void)timer;
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    uint32_t fps = 0;
    if (esp_lv_adapter_get_fps(NULL, &fps) == ESP_OK) {
        lv_label_set_text_fmt(s_fps_label, "%lu FPS", (unsigned long)fps);
    }
#endif
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

    /* ── Log Container（canvas 终端宿主，触摸拖动查看历史） ── */
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

    s_term_canvas = lv_canvas_create(s_log_container);
    lv_obj_set_size(s_term_canvas, SCREEN_W, TERM_VISIBLE_MAX * TERM_LINE_H);
    lv_canvas_set_buffer(s_term_canvas, s_term_buf, SCREEN_W,
                          TERM_VISIBLE_MAX * TERM_LINE_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(s_term_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_term_canvas, LV_OBJ_FLAG_SCROLLABLE);
    term_clear();
    /* 触摸事件：LVGL filter 是单个事件码（不支持位或），用 LV_EVENT_ALL 接收全部 */
    lv_obj_add_event_cb(s_term_canvas, on_term_touch, LV_EVENT_ALL, NULL);

    /* 右侧滚动指示条 */
    s_scrollbar = lv_obj_create(s_log_container);
    lv_obj_set_size(s_scrollbar, 4, 8);
    lv_obj_set_style_bg_color(s_scrollbar, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_bg_opa(s_scrollbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scrollbar, 0, 0);
    lv_obj_set_style_radius(s_scrollbar, 2, 0);
    lv_obj_set_pos(s_scrollbar, SCREEN_W - 4, 0);

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

    /* ── FPS 角标（日志区右上角，1Hz 刷新） ── */
    s_fps_label = lv_label_create(scr);
    lv_label_set_text(s_fps_label, "");
    lv_obj_add_flag(s_fps_label, LV_OBJ_FLAG_IGNORE_LAYOUT);   /* 不被 flex 布局移动 */
    lv_obj_set_style_text_color(s_fps_label, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_text_font(s_fps_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(s_fps_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_fps_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_fps_label, 2, 0);
    lv_obj_set_pos(s_fps_label, SCREEN_W - 60, STATUS_BAR_H + SEP_H + 4);
    lv_timer_create(fps_timer_cb, 1000, NULL);
}

/* ── Display Task: 收到数据立刻刷新 ── */

static void display_task(void *arg)
{
    (void)arg;
    disp_item_t item;

    while (1) {
        /* 等待队列数据，短超时以便及时响应按钮和降低显示延迟 */
        int got_data = (xQueueReceive(g_display_queue, &item, pdMS_TO_TICKS(10)) == pdTRUE);
        if (got_data) {
            /* 数据解析+位图滚动不涉及 LVGL，锁外处理；绘制/失效在锁内 */
            if (item.uart_idx == s_active_uart) {
                term_process(item.data, item.len);
            }
            while (xQueueReceive(g_display_queue, &item, 0) == pdTRUE) {
                if (item.uart_idx == s_active_uart) {
                    term_process(item.data, item.len);
                }
            }
            if (s_new_lines > 0) {
                if (esp_lv_adapter_lock(-1) == ESP_OK) {
                    if (s_follow) {
                        term_render();          /* 跟随：显示最新窗口 */
                    } else {
                        term_update_scrollbar(); /* 触摸暂停：数据入历史，只更新位置指示 */
                    }
                    s_new_lines = 0;
                    esp_lv_adapter_unlock();
                }
            }
        }

        /* 清除通知（按钮回调可能已发出） */
        xTaskNotifyWait(0, 0, NULL, 0);

        /* 处理按钮请求（term_clear 含 LVGL API，必须在 lock 内） */
        int need_status = 0;
        if (s_pending_clear || s_pending_uart_switch) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                if (s_pending_clear) {
                    term_clear();
                    need_status = 1;
                }
                if (s_pending_uart_switch) {
                    s_active_uart = !s_active_uart;
                    term_clear();
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

/*
 * CST816S ignores swap_xy/mirror flags in its get_xy callback,
 * so touch rotation must be done in software. Panel is rotated
 * via swap_xy + mirror_y (=270° CW effective), requiring:
 *   display_x = (V_RES-1) - raw_y
 *   display_y = raw_x
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
        points[0].x = (DRV_LCD_V_RES - 1) - raw_y;
        points[0].y = raw_x;
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
                DRV_LCD_V_RES, DRV_LCD_H_RES, ESP_LV_ADAPTER_ROTATE_0),
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

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    ESP_ERROR_CHECK(esp_lv_adapter_fps_stats_enable(lv_disp, true));
#endif

    esp_lv_adapter_touch_config_t tp_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lv_disp, disp.touch);
    tp_cfg.callbacks.custom_touch_read = touch_rotated_read;
    lv_indev_t *lv_tp = esp_lv_adapter_register_touch(&tp_cfg);
    assert(lv_tp != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    s_active_uart = 0;
    s_cur_len = 0;
    s_cur_w = 0;

    /* canvas 终端位图（12 行窗口）+ 行文本历史放 PSRAM（纯 CPU 访问，无 DMA 限制） */
    s_term_buf = heap_caps_aligned_alloc(64, (size_t)SCREEN_W * TERM_VISIBLE_MAX * TERM_LINE_H * 2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_term_buf != NULL);
    s_hist = heap_caps_malloc((size_t)TERM_HISTORY_MAX * (TERM_CURLINE_MAX + 1),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_hist != NULL);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        build_ui();
        update_status_bar();
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
