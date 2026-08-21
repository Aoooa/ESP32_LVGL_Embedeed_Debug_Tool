/* scope_app.c —— 示波器 APP（LVGL 9）：顶栏通道/状态 + 波形 canvas + 测量栏 + 底栏。
 * 采集走 scope/drv_scope（官方 adc_continuous DMA）。 */

#include "scope_app.h"
#include "drv_scope.h"
#include "num_input.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define S_TAG "scope"

/* ── 配色（深色 + Miku 绿，与 wave_gen 一致风格） ── */
#define SC_BG            0x0A0A12
#define SC_BAR_BORDER    0x1F2A36
#define SC_WAVE1         0x39C5BB   /* CH1 波形绿 */
#define SC_WAVE2         0xFFB86C   /* CH2 波形橙 */
#define SC_GRID          0x15303A   /* 网格暗青 */
#define SC_GRID_MID      0x1F4A50   /* 中心横轴 */
#define SC_TRIG          0xE5484D   /* 触发线红 */
#define SC_RUN           0x22C55E   /* RUN 状态点绿 */
#define SC_STOP          0x6B7280   /* STOP 状态点灰 */
#define SC_TEXT          0xE8E8F0
#define SC_MEAS          0x94A3B8
#define SC_BTN_ON        0x39C5BB   /* 键按下文字色 */

/* ── 布局 ── */
#define SC_TOP_H    28
#define SC_MEAS_H   32
#define SC_BAR_H    40

/* ── 采样率档位（官方合法 611..83333） ── */
static const int s_rates[] = { 80000, 40000, 20000, 10000, 5000, 2000, 1000 };
#define SC_RATE_N   (sizeof(s_rates) / sizeof(s_rates[0]))

/* ── 通道输入 GPIO（ADC1 空闲脚） ── */
#define SC_IO_CH1   5
#define SC_IO_CH2   6

typedef struct {
    lv_obj_t *root;
    lv_obj_t *ch_btn, *ch_lbl;
    lv_obj_t *state_dot, *state_lbl;
    lv_obj_t *canvas;
    lv_color_t *canvas_buf;
    int canvas_w, canvas_h;
    lv_obj_t *m_lbl1, *m_lbl2;
    lv_obj_t *btn[4];
    lv_obj_t *lbl[4];
    int ch_mode;
    scope_cfg_t cfg;
    bool running;
    uint32_t last_frameno;
    uint32_t last_meas_tick;
    scope_back_cb_t back_cb;
    void *back_ctx;
    lv_timer_t *tick;
    scope_frame_t *frame;        /* 帧缓冲（PSRAM，避免 8KB 上 LVGL 任务栈） */
} scope_t;

static scope_t *s_scope;

static int sc_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_horizontal_resolution(d) : 240;
}

static int sc_screen_h(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_vertical_resolution(d) : 320;
}

/* ── canvas 波形绘制（100ms 节流，全量重绘） ── */
static void scope_draw(const scope_frame_t *f)
{
    scope_t *s = s_scope;
    if (!s || !s->canvas || !s->canvas_buf) return;
    int cw = s->canvas_w, chh = s->canvas_h;

    lv_canvas_fill_bg(s->canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(s->canvas, &layer);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 1;

    /* 网格 10x8 */
    line.color = lv_color_hex(SC_GRID);
    for (int i = 1; i < 10; i++) {
        line.p1.x = i * cw / 10; line.p1.y = 0;
        line.p2.x = i * cw / 10; line.p2.y = chh - 1;
        lv_draw_line(&layer, &line);
    }
    for (int j = 1; j < 8; j++) {
        line.p1.x = 0; line.p1.y = j * chh / 8;
        line.p2.x = cw - 1; line.p2.y = j * chh / 8;
        lv_draw_line(&layer, &line);
    }
    /* 中心横轴（零参考）亮一档 */
    line.color = lv_color_hex(SC_GRID_MID);
    line.p1.x = 0; line.p1.y = chh / 2;
    line.p2.x = cw - 1; line.p2.y = chh / 2;
    lv_draw_line(&layer, &line);

    /* 波形（峰值检测：每列 min/max 竖线，防漏峰） */
    if (f && f->frameno && f->points > 0) {
        for (int c = 0; c < f->channels && c < SCOPE_CH_MAX; c++) {
            line.color = lv_color_hex(c == 0 ? SC_WAVE1 : SC_WAVE2);
            for (int col = 0; col < cw; col++) {
                int i0 = col * f->points / cw;
                int i1 = (col + 1) * f->points / cw;
                if (i1 <= i0) i1 = i0 + 1;
                uint32_t mn = 4096, mx = 0;
                for (int i = i0; i < i1 && i < f->points; i++) {
                    if (f->ch[c][i] < mn) mn = f->ch[c][i];
                    if (f->ch[c][i] > mx) mx = f->ch[c][i];
                }
                int y_hi = chh - 1 - (int)(mx * (uint32_t)chh / 4095);
                int y_lo = chh - 1 - (int)(mn * (uint32_t)chh / 4095);
                if (y_lo < y_hi) y_lo = y_hi;
                line.p1.x = col; line.p1.y = y_hi;
                line.p2.x = col; line.p2.y = y_lo;
                lv_draw_line(&layer, &line);
            }
        }
    }

    /* 触发线（红实线） */
    line.color = lv_color_hex(SC_TRIG);
    int ty = chh - 1 - (int)((uint32_t)s->cfg.trigger_level * chh / 4095);
    if (ty < 0) ty = 0;
    if (ty >= chh) ty = chh - 1;
    line.p1.x = 0; line.p1.y = ty;
    line.p2.x = cw - 1; line.p2.y = ty;
    lv_draw_line(&layer, &line);

    lv_canvas_finish_layer(s->canvas, &layer);
    lv_obj_invalidate(s->canvas);
}

/* ── 测量栏文本（500ms 节流） ── */
static void scope_update_meas(const scope_frame_t *f)
{
    scope_t *s = s_scope;
    if (!s || !f || !f->frameno) return;
    uint32_t now = lv_tick_get();
    if (now - s->last_meas_tick < 500) return;
    s->last_meas_tick = now;

    char b1[40], b2[40];
    if (f->freq_hz >= 1000.0f) {
        snprintf(b1, sizeof(b1), "FREQ %.2fk  Vpp %.2fV", f->freq_hz / 1000.0f, f->vpp);
    } else {
        snprintf(b1, sizeof(b1), "FREQ %.0fHz  Vpp %.2fV", f->freq_hz, f->vpp);
    }
    snprintf(b2, sizeof(b2), "DUTY %.1f%%  PW %.3fms", f->duty_pct, f->pw_ms);
    lv_label_set_text(s->m_lbl1, b1);
    lv_label_set_text(s->m_lbl2, b2);
}

/* ── 状态点 + 键文本刷新 ── */
static const char *const s_rate_strs[SC_RATE_N] = {
    "80k", "40k", "20k", "10k", "5k", "2k", "1k",
};

static void scope_refresh_status(void)
{
    scope_t *s = s_scope;
    if (!s) return;
    lv_obj_set_style_bg_color(s->state_dot, lv_color_hex(s->running ? SC_RUN : SC_STOP), 0);
    lv_label_set_text(s->state_lbl, s->running ? "RUN" : "STOP");
    lv_label_set_text(s->ch_lbl, s->ch_mode == 0 ? "CH1" : (s->ch_mode == 1 ? "CH2" : "Dual"));

    int rate_idx = 0;
    for (size_t i = 0; i < SC_RATE_N; i++) {
        if (s_rates[i] == s->cfg.sample_rate_hz) { rate_idx = (int)i; break; }
    }
    lv_label_set_text(s->lbl[1], s->cfg.trig_mode == SCOPE_TRIG_AUTO ? "AUTO"
                                     : (s->cfg.trig_mode == SCOPE_TRIG_NORM ? "NORM" : "SINGLE"));
    lv_label_set_text(s->lbl[2], s_rate_strs[rate_idx]);
}

/* ── 重启采集（配置变化后，停旧启新） ── */
static void scope_apply_cfg(void)
{
    scope_t *s = s_scope;
    if (!s) return;
    if (s->running) {
        drv_scope_start(&s->cfg);
    }
    scope_refresh_status();
}

/* ── 事件 ── */

static void on_ch_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    s->ch_mode = (s->ch_mode + 1) % 3;
    if (s->ch_mode == 2) {
        s->cfg.io[0] = SC_IO_CH1;
        s->cfg.io[1] = SC_IO_CH2;
    } else {
        s->cfg.io[0] = (s->ch_mode == 0) ? SC_IO_CH1 : SC_IO_CH2;
        s->cfg.io[1] = -1;
    }
    scope_apply_cfg();
}

static void on_run_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    if (s->running) {
        drv_scope_stop();
        s->running = false;
        s->last_frameno = 0;
    } else {
        if (drv_scope_start(&s->cfg) == ESP_OK) {
            s->running = true;
            s->last_frameno = 0;
            s->last_meas_tick = 0;
        }
    }
    scope_refresh_status();
}

static void on_trig_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    s->cfg.trig_mode = (scope_trig_mode_t)(((int)s->cfg.trig_mode + 1) % 3);
    scope_apply_cfg();
}

static void on_base_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    int idx = 0;
    for (size_t i = 0; i < SC_RATE_N; i++) {
        if (s_rates[i] == s->cfg.sample_rate_hz) { idx = (int)i; break; }
    }
    idx = (idx + 1) % SC_RATE_N;
    s->cfg.sample_rate_hz = s_rates[idx];
    scope_apply_cfg();
}

static void on_v_done(void *ctx, bool ok, int value)
{
    scope_t *s = ctx;
    if (!s) return;
    if (ok) {
        s->cfg.trigger_level = value;
        scope_apply_cfg();
    }
}

static void on_v_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    num_input_show(s->root, s->cfg.trigger_level, 0, 4095, false, on_v_done, s);
}

/* ── 定时器：100ms 波形帧 + 500ms 测量帧 ── */
static void scope_tick(lv_timer_t *t)
{
    (void)t;
    scope_t *s = s_scope;
    if (!s) return;
    if (num_input_is_active()) return;

    if (drv_scope_get_frame(s->frame) != ESP_OK) return;
    const scope_frame_t *f = s->frame;

    if (f->frameno != s->last_frameno) {
        s->last_frameno = f->frameno;
        if (f->running != s->running) {
            s->running = f->running;
            scope_refresh_status();
        }
        scope_draw(f);
        scope_update_meas(f);
    }
}

/* ── 构建 ── */

static lv_obj_t *sc_make_btn(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(SC_BAR_BORDER), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_pad_all(b, 0, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(SC_TEXT), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(SC_BTN_ON), LV_STATE_PRESSED);
    lv_obj_center(l);
    return b;
}

static void scope_relayout(void)
{
    scope_t *s = s_scope;
    if (!s) return;
    int sw = sc_screen_w(), sh = sc_screen_h();

    lv_obj_set_size(s->root, sw, sh);

    lv_obj_set_pos(s->ch_btn, 8, 2);
    lv_obj_set_size(s->ch_btn, 64, SC_TOP_H - 4);
    lv_obj_align(s->state_dot, LV_ALIGN_TOP_RIGHT, -56, 9);
    lv_obj_align(s->state_lbl, LV_ALIGN_TOP_RIGHT, -8, 7);

    int cw = sw, chh = sh - SC_TOP_H - SC_MEAS_H - SC_BAR_H;
    if (cw != s->canvas_w || chh != s->canvas_h) {
        if (s->canvas_buf) heap_caps_free(s->canvas_buf);
        s->canvas_buf = heap_caps_aligned_alloc(128, (size_t)cw * chh * 2,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s->canvas_w = cw;
        s->canvas_h = chh;
        if (s->canvas_buf && s->canvas) {
            lv_canvas_set_buffer(s->canvas, s->canvas_buf, cw, chh, LV_COLOR_FORMAT_RGB565);
        }
    }
    lv_obj_set_pos(s->canvas, 0, SC_TOP_H);

    lv_obj_set_pos(s->m_lbl1, 8, SC_TOP_H + 4);
    lv_obj_set_pos(s->m_lbl2, 8, SC_TOP_H + 18);

    int bw = (sw - 12 - 15) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_set_size(s->btn[i], bw, SC_BAR_H - 12);
        if (i == 0) {
            lv_obj_set_pos(s->btn[i], 6, sh - SC_BAR_H + 6);
        } else {
            lv_obj_align_to(s->btn[i], s->btn[i - 1], LV_ALIGN_OUT_RIGHT_MID, 5, 0);
        }
    }
    lv_obj_set_pos(s->root, 0, 0);
}

/* ── Public API ── */

lv_obj_t *scope_create(lv_obj_t *parent, scope_back_cb_t back_cb, void *ctx)
{
    scope_t *s = calloc(1, sizeof(scope_t));
    if (!s) return NULL;
    s->frame = heap_caps_malloc(sizeof(scope_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->frame) {
        free(s);
        return NULL;
    }
    memset(s->frame, 0, sizeof(scope_frame_t));
    s->back_cb = back_cb;
    s->back_ctx = ctx;
    s->ch_mode = 0;
    s->cfg.sample_rate_hz = 40000;
    s->cfg.io[0] = SC_IO_CH1;
    s->cfg.io[1] = -1;
    s->cfg.trig_mode = SCOPE_TRIG_AUTO;
    s->cfg.edge = SCOPE_EDGE_RISING;
    s->cfg.trigger_level = 2048;
    s->running = false;
    s_scope = s;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(SC_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    s->root = root;

    lv_obj_t *ch_btn = sc_make_btn(root, "CH1");
    lv_obj_add_event_cb(ch_btn, on_ch_btn, LV_EVENT_CLICKED, NULL);
    s->ch_btn = ch_btn;
    s->ch_lbl = lv_obj_get_child(ch_btn, 0);

    lv_obj_t *dot = lv_obj_create(root);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(SC_STOP), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    s->state_dot = dot;

    lv_obj_t *sl = lv_label_create(root);
    lv_label_set_text(sl, "STOP");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(SC_TEXT), 0);
    s->state_lbl = sl;

    lv_obj_t *cv = lv_canvas_create(root);
    s->canvas = cv;
    s->canvas_w = s->canvas_h = 0;
    lv_obj_set_pos(cv, 0, SC_TOP_H);

    lv_obj_t *ml1 = lv_label_create(root);
    lv_label_set_text(ml1, "FREQ --  Vpp --");
    lv_obj_set_style_text_font(ml1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ml1, lv_color_hex(SC_WAVE1), 0);
    s->m_lbl1 = ml1;
    lv_obj_t *ml2 = lv_label_create(root);
    lv_label_set_text(ml2, "DUTY --  PW --");
    lv_obj_set_style_text_font(ml2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ml2, lv_color_hex(SC_MEAS), 0);
    s->m_lbl2 = ml2;

    const char *btns[] = { "RUN", "AUTO", "40k", "V" };
    lv_event_cb_t cbs[] = { on_run_btn, on_trig_btn, on_base_btn, on_v_btn };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = sc_make_btn(root, btns[i]);
        lv_obj_add_event_cb(b, cbs[i], LV_EVENT_CLICKED, NULL);
        s->btn[i] = b;
        s->lbl[i] = lv_obj_get_child(b, 0);
    }

    scope_relayout();

    s->tick = lv_timer_create(scope_tick, 100, NULL);

    if (drv_scope_start(&s->cfg) == ESP_OK) {
        s->running = true;
    }
    scope_refresh_status();

    ESP_LOGI(S_TAG, "scope UI created (%dx%d)", sc_screen_w(), sc_screen_h());
    return root;
}

void scope_destroy(lv_obj_t *root)
{
    scope_t *s = s_scope;
    if (s) {
        drv_scope_stop();
        if (s->tick) lv_timer_delete(s->tick);
        if (s->canvas_buf) heap_caps_free(s->canvas_buf);
        s->canvas_buf = NULL;
        if (s->frame) heap_caps_free(s->frame);
        s->frame = NULL;
    }
    s_scope = NULL;
    if (root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(root);
    }
    free(s);
}

bool scope_swipe_back(lv_obj_t *root)
{
    (void)root;
    if (num_input_is_active()) {
        num_input_cancel();
        return false;
    }
    return true;
}

void scope_rotate(lv_obj_t *root, int deg)
{
    (void)deg;
    scope_t *s = s_scope;
    if (!s) return;
    if (num_input_is_active()) num_input_cancel();
    scope_relayout();
}

void scope_debug_event(lv_obj_t *root, int evt)
{
    (void)root;
    (void)evt;
    scope_t *s = s_scope;
    if (!s) return;
    ESP_LOGI(S_TAG, "[DBG] ch=%d rate=%d trig=%d level=%d running=%d", s->ch_mode,
             s->cfg.sample_rate_hz, s->cfg.trig_mode, s->cfg.trigger_level, s->running);
}
