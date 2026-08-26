/* wave_gen.c —— 波形输出 APP（LVGL 9）：多通道列表 + 设置对话框。
 *
 * 主界面：可滚动通道选项卡列表（✕删除 / IO+模式+参数摘要 / 双色指示灯
 * start-stop 按钮）+ 底部"添加通道"。无顶部状态栏，返回靠右滑手势。
 * 设置对话框：左列配置选项（MODE/IO/参数，按模式动态，单选高亮），
 * 右侧显示选项内容（模式按钮/IO列表/数值选择 1Hz 步进），底部说明+实时
 * 示意图（字符画，零绘制风险）。指示灯状态 100ms 轮询驱动同步。 */

#include "wave_gen.h"
#include "drv_wave.h"
#include "app_font.h"
#include "num_input.h"
#include "io_picker.h"
#include "esp_lv_adapter.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define WG_TAG "wave_gen"

/* 配色（Miku 绿·冷峻科技风） */
#define WG_BG          0x000000
#define WG_PANEL       0x000000
#define WG_CARD        0x000000
#define WG_BORDER      0x000000
#define WG_TEXT        0xE6F0EE
#define WG_DIM         0x94A3B8
#define WG_GREEN       0x39C5BB
#define WG_GREEN_HI    0x7FE5DC
#define WG_GREEN_DEEP  0x062018
#define WG_RUN         0x22C55E
#define WG_STOP        0xE5484D

/* 可用输出 IO（避开已占用：LCD/触摸/SD/UART/USB/PSRAM；GPIO17 被 UART2 占用） */
static const int s_io_list[7] = { 5, 6, 7, 8, 18, 37, 44 };
#define WG_IO_COUNT ((int)(sizeof(s_io_list) / sizeof(s_io_list[0])))
#define WG_MAX_CH 7

/* 对话框左列选项 */
enum {
    WG_OPT_MODE = 0,
    WG_OPT_IO,
    WG_OPT_FREQ,
    WG_OPT_DUTY,
    WG_OPT_AMP,
    WG_OPT_PULSE_MS,
    WG_OPT_COUNT,
};

typedef struct {
    lv_obj_t *root;
    wave_gen_back_cb_t back_cb;
    void *ctx;

    bool ch_used[WG_MAX_CH];          /* 通道槽是否在用 */
    wave_ch_cfg_t cfg[WG_MAX_CH];     /* 每通道配置 */

    lv_obj_t *list_area;              /* 滚动列表容器 */
    lv_obj_t *led_objs[WG_MAX_CH];    /* 每通道指示灯按钮（LED 刷新用） */
    lv_timer_t *refresh_timer;        /* 100ms 指示灯状态刷新 */

    lv_obj_t *modal;                  /* 设置对话框（NULL=无） */
    int edit_ch;                      /* 对话框编辑的通道槽 */
    wave_ch_cfg_t edit_cfg;           /* 编辑副本（OK 才应用） */
    int sel_opt;                      /* 左列选中项 */
    int num_opt;                      /* 当前正在编辑的数值选项（num_input 回调） */
    lv_obj_t *m_left;                 /* 对话框左列（引用，避免魔法索引） */
    lv_obj_t *m_right;                /* 对话框右侧区（透明，高度随内容） */
    lv_obj_t *m_bottom;               /* 对话框底部两栏区（透明） */
    lv_obj_t *m_desc;                 /* 底部左栏参数值标签（多行） */
    lv_obj_t *m_canvas;               /* 底部右栏波形 canvas */
    lv_color_t *m_canvas_buf;         /* canvas buffer（PSRAM 128 对齐） */
    int m_canvas_w, m_canvas_h;
} wg_t;

static wg_t *s_wg;

static lv_font_t *wg_font16(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* ── 工具 ── */

static const char *wg_opt_name(int opt)
{
    switch (opt) {
    case WG_OPT_MODE:      return "MODE";
    case WG_OPT_IO:        return "IO";
    case WG_OPT_FREQ:      return "FREQ";
    case WG_OPT_DUTY:      return "DUTY";
    case WG_OPT_AMP:       return "AMP";
    case WG_OPT_PULSE_MS:  return "PULSE";
    default:               return "?";
    }
}

static void wg_fmt_freq(char *buf, int size, int hz)
{
    if (hz >= 1000) snprintf(buf, size, "%.3f kHz", hz / 1000.0);
    else            snprintf(buf, size, "%d Hz", hz);
}

/* 按模式给出参数摘要（主界面第二行） */
static void wg_param_summary(const wave_ch_cfg_t *c, char *buf, int size)
{
    switch (c->type) {
    case WAVE_PWM:
    case WAVE_SQUARE: {
        char f[24];
        wg_fmt_freq(f, sizeof(f), c->freq_hz);
        snprintf(buf, size, "%s | %d%%", f, c->duty_pct);
        break;
    }
    case WAVE_SINE: {
        char f[24];
        wg_fmt_freq(f, sizeof(f), c->freq_hz);
        snprintf(buf, size, "%s | amp %d%%", f, c->duty_pct);
        break;
    }
    case WAVE_PULSE:
        snprintf(buf, size, "%d ms", c->pulse_ms);
        break;
    default:
        snprintf(buf, size, "--");
        break;
    }
}

/* 模式是否可用（LEDC 定时器池余量：当前通道换模式会先释放自己的定时器） */
static bool wg_mode_available(const wg_t *w, int ch, wave_type_t t)
{
    int cur = 0;
    if (ch >= 0 && w->ch_used[ch] && drv_wave_type_uses_timer(w->cfg[ch].type)) cur = 1;
    int need = drv_wave_type_uses_timer(t) ? 1 : 0;
    return (drv_wave_timers_in_use() - cur + need) <= 4;
}

/* ── 主界面：通道选项卡 ── */

static void wg_open_modal(wg_t *w, int ch);
static void wg_refresh_list(wg_t *w);
static void wg_rebuild_modal(wg_t *w);
static void wg_update_footer(wg_t *w);

static void wg_del_click(lv_event_t *e)
{
    wg_t *w = s_wg;
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!w || ch < 0 || ch >= WG_MAX_CH || !w->ch_used[ch]) return;
    drv_wave_ch_stop(ch);
    w->ch_used[ch] = false;
    w->cfg[ch].io = -1;
    w->cfg[ch].type = WAVE_OFF;
    wg_refresh_list(w);
}

static void wg_run_click(lv_event_t *e)
{
    wg_t *w = s_wg;
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!w || ch < 0 || ch >= WG_MAX_CH || !w->ch_used[ch]) return;
    if (drv_wave_ch_active(ch)) {
        drv_wave_ch_stop(ch);
    } else {
        esp_err_t err = drv_wave_ch_apply(ch, &w->cfg[ch]);
        if (err != ESP_OK) ESP_LOGW(WG_TAG, "ch%d start failed %d", ch, err);
    }
    wg_refresh_list(w);
}

static void wg_card_click(lv_event_t *e)
{
    wg_t *w = s_wg;
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!w || ch < 0 || ch >= WG_MAX_CH || !w->ch_used[ch]) return;
    wg_open_modal(w, ch);
}

static void wg_update_led(const wg_t *w, lv_obj_t *led, int ch)
{
    (void)w;
    bool on = drv_wave_ch_active(ch);
    lv_obj_set_style_bg_color(led, lv_color_hex(on ? WG_RUN : WG_STOP), 0);
}

static void wg_refresh_list(wg_t *w)
{
    lv_obj_clean(w->list_area);
    for (int i = 0; i < WG_MAX_CH; i++) w->led_objs[i] = NULL;

    int used = 0;
    for (int i = 0; i < WG_MAX_CH; i++) if (w->ch_used[i]) used++;

    if (used == 0) {
        lv_obj_t *lbl = lv_label_create(w->list_area);
        lv_label_set_text(lbl, "no channel\npress + to add");
        lv_obj_set_style_text_color(lbl, lv_color_hex(WG_DIM), 0);
        lv_obj_set_style_text_font(lbl, wg_font16(), 0);
        lv_obj_center(lbl);
        return;
    }

    for (int i = 0; i < WG_MAX_CH; i++) {
        if (!w->ch_used[i]) continue;
        const wave_ch_cfg_t *c = &w->cfg[i];

        lv_obj_t *card = lv_button_create(w->list_area);
        lv_obj_set_size(card, lv_pct(100), 60);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(card, 6, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(WG_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(WG_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, wg_card_click, LV_EVENT_CLICKED, w);

        /* ✕ 删除按钮（最左，无红框） */
        lv_obj_t *del = lv_button_create(card);
        lv_obj_set_size(del, 34, 44);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x1A222C), 0);
        lv_obj_set_style_border_color(del, lv_color_hex(WG_BORDER), 0);
        lv_obj_set_style_border_width(del, 1, 0);
        lv_obj_set_style_radius(del, 6, 0);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text(dl, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(dl, lv_color_hex(WG_DIM), 0);
        lv_obj_center(dl);
        lv_obj_set_user_data(del, (void *)(intptr_t)i);
        lv_obj_add_event_cb(del, wg_del_click, LV_EVENT_CLICKED, w);

        /* 中间信息区：占满卡片内容区，两行文字垂直+水平居中 */
        lv_obj_t *mid = lv_obj_create(card);
        lv_obj_set_flex_grow(mid, 1);
        lv_obj_set_size(mid, lv_pct(100), lv_pct(100));
        lv_obj_set_style_pad_all(mid, 0, 0);
        lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mid, 0, 0);
        lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(mid, 2, 0);
        lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        char line1[40], line2[40];
        if (c->type == WAVE_OFF) {
            /* 未配置：通道号 + 提示点击配置 */
            snprintf(line1, sizeof(line1), "CH%d GPIO%d", i, c->io);
            snprintf(line2, sizeof(line2), "tap to configure");
        } else {
            snprintf(line1, sizeof(line1), "CH%d GPIO%d %s", i, c->io, drv_wave_type_str(c->type));
            wg_param_summary(c, line2, sizeof(line2));
        }
        lv_obj_t *l1 = lv_label_create(mid);
        lv_label_set_text(l1, line1);
        lv_obj_set_style_text_color(l1, lv_color_hex(WG_GREEN_HI), 0);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_12, 0);

        lv_obj_t *l2 = lv_label_create(mid);
        lv_label_set_text(l2, line2);
        lv_obj_set_style_text_color(l2, lv_color_hex(c->type == WAVE_OFF ? WG_DIM : WG_DIM), 0);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);

        /* 圆形 start/stop 指示灯（最右，既是按钮也是状态灯；默认 stop=红） */
        lv_obj_t *led = lv_button_create(card);
        lv_obj_set_size(led, 36, 36);
        lv_obj_set_style_radius(led, 18, 0);   /* 正圆 */
        lv_obj_set_style_border_color(led, lv_color_hex(WG_BORDER), 0);
        lv_obj_set_style_border_width(led, 1, 0);
        lv_obj_set_style_pad_all(led, 0, 0);
        lv_obj_set_user_data(led, (void *)(intptr_t)i);
        lv_obj_add_event_cb(led, wg_run_click, LV_EVENT_CLICKED, w);
        wg_update_led(w, led, i);
        w->led_objs[i] = led;
    }
}

/* 添加通道：找空槽 + 分配最小可用 IO */
static void wg_add_channel(lv_event_t *e)
{
    (void)e;
    wg_t *w = s_wg;
    int slot = -1;
    for (int i = 0; i < WG_MAX_CH; i++) {
        if (!w->ch_used[i]) { slot = i; break; }
    }
    if (slot < 0) return;

    int io = -1;
    for (int k = 0; k < WG_IO_COUNT; k++) {
        bool taken = false;
        for (int i = 0; i < WG_MAX_CH; i++) {
            if (w->ch_used[i] && w->cfg[i].io == s_io_list[k]) { taken = true; break; }
        }
        if (!taken) { io = s_io_list[k]; break; }
    }
    if (io < 0) return;

    w->ch_used[slot] = true;
    w->cfg[slot].io = io;
    w->cfg[slot].type = WAVE_OFF;   /* 未配置：点击卡片打开设置对话框 */
    w->cfg[slot].freq_hz = 1000;
    w->cfg[slot].duty_pct = 50;
    w->cfg[slot].pulse_ms = 200;
    wg_refresh_list(w);
}

/* 指示灯状态轮询（脉冲自动停止后同步变色；只更新 LED 颜色，不重建列表） */
static void wg_tick(lv_timer_t *t)
{
    (void)t;
    wg_t *w = s_wg;
    if (!w) return;
    for (int i = 0; i < WG_MAX_CH; i++) {
        if (w->led_objs[i]) wg_update_led(w, w->led_objs[i], i);
    }
}

/* ── 设置对话框 ── */

static void wg_modal_close(wg_t *w)
{
    if (w->modal) {
        lv_obj_delete(w->modal);
        w->modal = NULL;
    }
    if (w->m_canvas_buf) {
        heap_caps_free(w->m_canvas_buf);
        w->m_canvas_buf = NULL;
    }
    w->m_canvas = NULL;
    w->m_desc = NULL;
}

/* 数值输入（复用公共组件 num_input：点击输入框 → 弹数字键盘） */

static void wg_num_confirm(void *ctx, bool ok, int value)
{
    wg_t *w = ctx;
    if (!ok) return;   /* 用户取消，不修改 */
    switch (w->num_opt) {
    case WG_OPT_FREQ:     w->edit_cfg.freq_hz = value; break;
    case WG_OPT_DUTY:     w->edit_cfg.duty_pct = value; break;
    case WG_OPT_AMP:      w->edit_cfg.duty_pct = value; break;
    case WG_OPT_PULSE_MS: w->edit_cfg.pulse_ms = value; break;
    default: break;
    }
    wg_update_footer(w);
    wg_rebuild_modal(w);   /* 刷新右区输入框显示 */
}

static void wg_num_open(lv_event_t *e)
{
    wg_t *w = s_wg;
    int opt = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    w->num_opt = opt;
    int initial = 0, min = 0, max = 0;
    bool dec = false;
    switch (opt) {
    case WG_OPT_FREQ:
        initial = w->edit_cfg.freq_hz;
        min = (w->edit_cfg.type == WAVE_SINE) ? 1 : 5;
        max = (w->edit_cfg.type == WAVE_SINE) ? 200 : 1000000;
        dec = true;
        break;
    case WG_OPT_DUTY:
        initial = w->edit_cfg.duty_pct;
        min = 1; max = 99;
        break;
    case WG_OPT_AMP:
        initial = w->edit_cfg.duty_pct;
        min = 0; max = 50;
        break;
    case WG_OPT_PULSE_MS:
        initial = w->edit_cfg.pulse_ms;
        min = 10; max = 5000;
        break;
    default:
        return;
    }
    num_input_show(w->root, initial, min, max, dec, 0, wg_num_confirm, w);
}

/* 右侧区：数值输入框（按钮显示当前值，点击弹数字键盘） */
static void wg_build_num(lv_obj_t *parent, wg_t *w, int opt)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    lv_obj_t *nl = lv_label_create(parent);
    lv_label_set_text(nl, wg_opt_name(opt));
    lv_obj_set_style_text_color(nl, lv_color_hex(WG_DIM), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_12, 0);

    lv_obj_t *ib = lv_button_create(parent);
    lv_obj_set_size(ib, lv_pct(100), 40);
    lv_obj_set_style_bg_color(ib, lv_color_hex(WG_PANEL), 0);
    lv_obj_set_style_border_color(ib, lv_color_hex(WG_GREEN), 0);
    lv_obj_set_style_border_width(ib, 1, 0);
    lv_obj_set_style_radius(ib, 6, 0);
    lv_obj_set_style_pad_all(ib, 0, 0);

    lv_obj_t *vl = lv_label_create(ib);
    char buf[24];
    if (opt == WG_OPT_FREQ) {
        wg_fmt_freq(buf, sizeof(buf), w->edit_cfg.freq_hz);
    } else if (opt == WG_OPT_PULSE_MS) {
        snprintf(buf, sizeof(buf), "%d ms", w->edit_cfg.pulse_ms);
    } else {
        snprintf(buf, sizeof(buf), "%d%%", w->edit_cfg.duty_pct);
    }
    lv_label_set_text(vl, buf);
    lv_obj_set_style_text_color(vl, lv_color_hex(WG_GREEN_HI), 0);
    lv_obj_set_style_text_font(vl, &lv_font_montserrat_16, 0);
    lv_obj_center(vl);
    lv_obj_t *ar = lv_label_create(ib);
    lv_label_set_text(ar, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_color(ar, lv_color_hex(WG_DIM), 0);
    lv_obj_align(ar, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_set_user_data(ib, (void *)(intptr_t)opt);
    lv_obj_add_event_cb(ib, wg_num_open, LV_EVENT_CLICKED, w);
}

/* 右侧区：MODE 选择 */
static void wg_mode_pick(lv_event_t *e)
{
    wg_t *w = s_wg;
    wave_type_t t = (wave_type_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!wg_mode_available(w, w->edit_ch, t)) return;
    if (w->edit_cfg.type == t) return;   /* 同模式不重置 */
    w->edit_cfg.type = t;
    /* 切换模式：重置该通道配置为默认值（参考图要求） */
    switch (t) {
    case WAVE_PWM:
    case WAVE_SQUARE:
        w->edit_cfg.freq_hz = 1000;
        w->edit_cfg.duty_pct = 50;
        break;
    case WAVE_SINE:
        w->edit_cfg.freq_hz = 50;
        w->edit_cfg.duty_pct = 30;   /* 幅度 */
        break;
    case WAVE_PULSE:
        w->edit_cfg.pulse_ms = 200;
        break;
    default:
        break;
    }
    wg_rebuild_modal(w);   /* 参数列表随模式变化，示意图实时更新 */
}

/* 右侧区：IO 选择（弹出 io_picker，单选即回） */
static void wg_io_picked(void *ctx, int io)
{
    wg_t *w = ctx;
    if (!w) return;
    if (io < 0) return;   /* 取消 */
    for (int i = 0; i < WG_MAX_CH; i++) {
        if (i != w->edit_ch && w->ch_used[i] && w->cfg[i].io == io) return;   /* 其它通道占用，忽略 */
    }
    w->edit_cfg.io = io;
    wg_rebuild_modal(w);
}

static void wg_io_sel_evt(lv_event_t *e)
{
    wg_t *w = lv_event_get_user_data(e);
    if (!w) return;
    io_picker_show(w->root, IO_CAPS_ANY, wg_io_picked, w);
}

/* 左列选中 */
static void wg_opt_click(lv_event_t *e)
{
    wg_t *w = s_wg;
    int opt = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (opt == w->sel_opt) return;
    w->sel_opt = opt;
    wg_rebuild_modal(w);
}

/* OK：仅保存配置（不启动输出——保持 STOP，由指示灯按钮控制 start） */
static void wg_modal_ok(lv_event_t *e)
{
    (void)e;
    wg_t *w = s_wg;
    if (!w) return;
    int ch = w->edit_ch;
    if (w->ch_used[ch]) {
        drv_wave_ch_stop(ch);          /* 停止旧输出，释放 IO/定时器 */
        w->cfg[ch] = w->edit_cfg;      /* 只保存配置，不 apply */
    }
    wg_modal_close(w);
    wg_refresh_list(w);
}

/* 构建对话框右侧区（按左列选中项） */
static void wg_build_right(lv_obj_t *right, wg_t *w)
{
    lv_obj_clean(right);

    if (w->sel_opt == WG_OPT_MODE) {
        lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(right, 4, 0);
        static const wave_type_t types[] = { WAVE_PWM, WAVE_SQUARE, WAVE_SINE, WAVE_PULSE };
        for (int i = 0; i < 4; i++) {
            bool avail = wg_mode_available(w, w->edit_ch, types[i]);
            bool sel = (types[i] == w->edit_cfg.type);
            lv_obj_t *b = lv_button_create(right);
            lv_obj_set_size(b, 62, 28);   /* 紧凑：2x2 并排 */
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(sel ? WG_GREEN : WG_PANEL), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(WG_GREEN), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_t *bl = lv_label_create(b);
            lv_label_set_text(bl, drv_wave_type_str(types[i]));
            lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(bl, lv_color_hex(sel ? WG_GREEN_DEEP : WG_TEXT), 0);
            lv_obj_center(bl);
            lv_obj_set_user_data(b, (void *)(intptr_t)types[i]);
            if (avail) {
                lv_obj_add_event_cb(b, wg_mode_pick, LV_EVENT_CLICKED, w);
            } else {
                lv_obj_add_state(b, LV_STATE_DISABLED);
            }
        }
    } else if (w->sel_opt == WG_OPT_IO) {
        lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(right, 6, 0);
        /* 单个选择按钮：显示当前 IO，点击弹出 io_picker */
        lv_obj_t *b = lv_button_create(right);
        lv_obj_set_size(b, 130, 30);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(WG_PANEL), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(WG_GREEN), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text_fmt(bl, w->edit_cfg.io >= 0 ? "IO%d" : "Select IO", w->edit_cfg.io);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(w->edit_cfg.io >= 0 ? WG_GREEN_HI : WG_TEXT), 0);
        lv_obj_center(bl);
        lv_obj_add_event_cb(b, wg_io_sel_evt, LV_EVENT_CLICKED, w);
        lv_obj_t *t = lv_label_create(right);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(WG_DIM), 0);
        lv_label_set_text(t, "tap to pick a free pin");
    } else {
        /* 数值输入框（点击弹数字键盘） */
        wg_build_num(right, w, w->sel_opt);
    }
}

/* 底部右栏：canvas 绘制输出示意（透明容器，canvas 黑底） */
static void wg_draw_art(wg_t *w)
{
    if (!w->m_canvas || !w->m_canvas_buf) return;
    int cw = w->m_canvas_w, ch = w->m_canvas_h;
    lv_canvas_fill_bg(w->m_canvas, lv_color_hex(0x000000), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(w->m_canvas, &layer);

    int mid = ch / 2;
    int top = 3, bot = ch - 4;
    static lv_point_precise_t pts[96];
    int npts = 0;

    switch (w->edit_cfg.type) {
    case WAVE_PWM:
    case WAVE_SQUARE: {
        int per = cw / 4;                       /* 4 个周期 */
        if (per < 4) per = 4;
        int hi = per * w->edit_cfg.duty_pct / 100;
        if (hi < 1) hi = 1;
        if (hi > per - 1) hi = per - 1;
        for (int cyc = 0; cyc < 4 && npts < 92; cyc++) {
            int x0 = cyc * per;
            pts[npts].x = x0;            pts[npts].y = top; npts++;
            pts[npts].x = x0 + hi;       pts[npts].y = top; npts++;
            pts[npts].x = x0 + hi;       pts[npts].y = bot; npts++;
            pts[npts].x = x0 + per;      pts[npts].y = bot; npts++;
        }
        break;
    }
    case WAVE_SINE: {
        int n = 32;
        for (int i = 0; i <= n && npts < 95; i++) {
            double ph = 2.0 * M_PI * i / n;
            pts[npts].x = i * cw / n;
            pts[npts].y = mid - (int)((double)(mid - 4) * sin(ph));
            npts++;
        }
        break;
    }
    case WAVE_PULSE: {
        int hi_pix = cw / 3;
        pts[npts].x = 0;             pts[npts].y = top; npts++;
        pts[npts].x = hi_pix;        pts[npts].y = top; npts++;
        pts[npts].x = hi_pix;        pts[npts].y = bot; npts++;
        pts[npts].x = 2 * hi_pix;    pts[npts].y = bot; npts++;
        pts[npts].x = 2 * hi_pix;    pts[npts].y = top; npts++;
        pts[npts].x = cw;            pts[npts].y = top; npts++;
        break;
    }
    default:
        pts[npts].x = 0; pts[npts].y = mid; npts++;
        pts[npts].x = cw; pts[npts].y = mid; npts++;
        break;
    }

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(WG_GREEN_HI);
    line.width = 2;
    for (int i = 0; i < npts - 1; i++) {
        line.p1 = pts[i];
        line.p2 = pts[i + 1];
        lv_draw_line(&layer, &line);
    }
    lv_canvas_finish_layer(w->m_canvas, &layer);
    lv_obj_invalidate(w->m_canvas);
}

/* 更新对话框底部两栏：左=参数值标签（多行），右=canvas 示意图（实时） */
static void wg_update_footer(wg_t *w)
{
    if (!w->modal || !w->m_desc) return;
    char buf[96];
    switch (w->edit_cfg.type) {
    case WAVE_PWM:
    case WAVE_SQUARE:
        snprintf(buf, sizeof(buf), "f: %dHz\nduty: %d%%",
                 w->edit_cfg.freq_hz, w->edit_cfg.duty_pct);
        break;
    case WAVE_SINE:
        snprintf(buf, sizeof(buf), "f: %dHz\namp: %d%%",
                 w->edit_cfg.freq_hz, w->edit_cfg.duty_pct);
        break;
    case WAVE_PULSE:
        snprintf(buf, sizeof(buf), "width: %dms", w->edit_cfg.pulse_ms);
        break;
    default:
        buf[0] = '\0';
        break;
    }
    lv_label_set_text(w->m_desc, buf);
    wg_draw_art(w);   /* canvas 实时重绘 */
}

/* 重建对话框（顶部状态栏 + 左列 + 右区 + 底部两栏） */
static void wg_rebuild_modal(wg_t *w)
{
    if (!w->modal) return;
    lv_obj_t *right = w->m_right;
    lv_obj_clean(right);   /* 右区每次重建（内容随选中选项变化） */

    wg_build_right(right, w);

    /* 左列重建（贴左、窄 64、项高 26） */
    lv_obj_t *left = w->m_left;
    lv_obj_clean(left);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(left, 2, 0);

    struct { int opt; } opts[6];
    int n = 0;
    opts[n++].opt = WG_OPT_MODE;
    opts[n++].opt = WG_OPT_IO;
    if (w->edit_cfg.type == WAVE_PWM || w->edit_cfg.type == WAVE_SQUARE) {
        opts[n++].opt = WG_OPT_FREQ;
        opts[n++].opt = WG_OPT_DUTY;
    } else if (w->edit_cfg.type == WAVE_SINE) {
        opts[n++].opt = WG_OPT_FREQ;
        opts[n++].opt = WG_OPT_AMP;
    } else if (w->edit_cfg.type == WAVE_PULSE) {
        opts[n++].opt = WG_OPT_PULSE_MS;
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_button_create(left);
        lv_obj_set_size(row, lv_pct(100), 26);
        lv_obj_set_style_radius(row, 3, 0);
        bool sel = (opts[i].opt == w->sel_opt);
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? WG_GREEN : WG_PANEL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, wg_opt_name(opts[i].opt));
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? WG_GREEN_DEEP : WG_DIM), 0);
        lv_obj_center(lbl);
        lv_obj_set_user_data(row, (void *)(intptr_t)opts[i].opt);
        lv_obj_add_event_cb(row, wg_opt_click, LV_EVENT_CLICKED, w);
    }

    /* bottom 结构在 open_modal 创建（标签列 + canvas），此处只更新内容 */
    wg_update_footer(w);
}

static void wg_modal_x(lv_event_t *e)
{
    (void)e;
    wg_modal_close(s_wg);
}

static void wg_open_modal(wg_t *w, int ch)
{
    wg_modal_close(w);
    w->edit_ch = ch;
    w->edit_cfg = w->cfg[ch];
    /* 未配置通道（新创建）：进入配置默认 PWM，canvas 显示默认 PWM 波形 */
    if (w->edit_cfg.type == WAVE_OFF) {
        w->edit_cfg.type = WAVE_PWM;
        w->edit_cfg.freq_hz = 1000;
        w->edit_cfg.duty_pct = 50;
        w->edit_cfg.pulse_ms = 200;
    }
    w->sel_opt = WG_OPT_MODE;

    lv_obj_t *m = lv_obj_create(w->root);
    lv_obj_set_size(m, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(m, 0, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(WG_BG), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 2, 0);   /* 贴边：左列紧贴左边框 */
    lv_obj_set_style_pad_gap(m, 2, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    w->modal = m;

    /* 顶部状态栏（24px）：左=标题，右=Cancel/OK 小按钮 */
    lv_obj_t *bar = lv_obj_create(m);
    lv_obj_set_size(bar, lv_pct(100), 24);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(bar);
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "CH%d GPIO%d", ch, w->cfg[ch].io);
    lv_label_set_text(tl, tbuf);
    lv_obj_set_style_text_color(tl, lv_color_hex(WG_GREEN_HI), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);

    lv_obj_t *btnrow = lv_obj_create(bar);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btnrow, 4, 0);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_clear_flag(btnrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_button_create(btnrow);
    lv_obj_set_size(cancel, 48, 20);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(WG_PANEL), 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(WG_STOP), 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_radius(cancel, 4, 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(WG_STOP), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_10, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, wg_modal_x, LV_EVENT_CLICKED, w);

    lv_obj_t *okb = lv_button_create(btnrow);
    lv_obj_set_size(okb, 40, 20);
    lv_obj_set_style_bg_color(okb, lv_color_hex(WG_GREEN), 0);
    lv_obj_set_style_bg_opa(okb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(okb, 4, 0);
    lv_obj_set_style_pad_all(okb, 0, 0);
    lv_obj_t *okl = lv_label_create(okb);
    lv_label_set_text(okl, "OK");
    lv_obj_set_style_text_color(okl, lv_color_hex(WG_GREEN_DEEP), 0);
    lv_obj_set_style_text_font(okl, &lv_font_montserrat_10, 0);
    lv_obj_center(okl);
    lv_obj_add_event_cb(okb, wg_modal_ok, LV_EVENT_CLICKED, w);

    /* body：左列 + 右区（扁平：全部透明无边框，右区高度随内容） */
    lv_obj_t *body = lv_obj_create(m);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_size(body, lv_pct(100), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, 4, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = lv_obj_create(body);
    lv_obj_set_size(left, 64, lv_pct(100));
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_scroll_dir(left, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(left, LV_SCROLLBAR_MODE_AUTO);
    w->m_left = left;

    lv_obj_t *right = lv_obj_create(body);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_size(right, lv_pct(100), LV_SIZE_CONTENT);   /* 高度随配置内容 */
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);        /* 透明，无框 */
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    w->m_right = right;

    /* 底部两栏容器（透明，固定高）：左=参数值标签（多行），右=canvas 示意图 */
    lv_obj_t *bottom = lv_obj_create(m);
    lv_obj_set_size(bottom, lv_pct(100), 80);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);        /* 透明，显示背景黑 */
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bottom, 8, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
    w->m_bottom = bottom;

    /* 左：参数值标签（多行，透明） */
    lv_obj_t *pl = lv_obj_create(bottom);
    lv_obj_set_size(pl, 86, lv_pct(100));
    lv_obj_set_flex_flow(pl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(pl, 3, 0);
    lv_obj_set_style_bg_opa(pl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pl, 0, 0);
    lv_obj_clear_flag(pl, LV_OBJ_FLAG_SCROLLABLE);
    w->m_desc = lv_label_create(pl);
    lv_label_set_long_mode(w->m_desc, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(w->m_desc, lv_color_hex(WG_GREEN_HI), 0);
    lv_obj_set_style_text_font(w->m_desc, &lv_font_montserrat_14, 0);

    /* 右：canvas 波形示意图（透明容器，canvas 黑底，128 对齐 PSRAM） */
    w->m_canvas_w = 138;
    w->m_canvas_h = 72;
    w->m_canvas_buf = heap_caps_aligned_alloc(128, (size_t)w->m_canvas_w * w->m_canvas_h * 2,
                                              MALLOC_CAP_SPIRAM);
    if (w->m_canvas_buf) {
        lv_obj_t *cv = lv_canvas_create(bottom);
        lv_canvas_set_buffer(cv, w->m_canvas_buf, w->m_canvas_w, w->m_canvas_h,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(cv, LV_OBJ_FLAG_EVENT_BUBBLE);
        w->m_canvas = cv;
    } else {
        ESP_LOGE(WG_TAG, "art canvas alloc failed");
        w->m_canvas = NULL;
    }

    wg_rebuild_modal(w);
}

/* ── Public API ── */

lv_obj_t *wave_gen_create(lv_obj_t *parent, wave_gen_back_cb_t back_cb, void *ctx)
{
    int sw = lv_display_get_horizontal_resolution(lv_display_get_default());
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());

    wg_t *w = malloc(sizeof(wg_t));
    if (!w) return NULL;
    memset(w, 0, sizeof(wg_t));
    w->back_cb = back_cb;
    w->ctx = ctx;
    s_wg = w;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, sw, sh);
    lv_obj_set_style_bg_color(root, lv_color_hex(WG_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    w->root = root;

    /* 列表区（可滚动） */
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_set_size(list, sw, sh - 48);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_gap(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    w->list_area = list;

    /* 底部添加按钮栏 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, sw, 48);
    lv_obj_set_pos(bar, 0, sh - 48);
    lv_obj_set_style_bg_color(bar, lv_color_hex(WG_CARD), 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(WG_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *add = lv_button_create(bar);
    lv_obj_set_size(add, sw - 16, 38);
    lv_obj_set_style_bg_color(add, lv_color_hex(WG_PANEL), 0);
    lv_obj_set_style_border_color(add, lv_color_hex(WG_GREEN), 0);
    lv_obj_set_style_border_width(add, 1, 0);
    lv_obj_set_style_radius(add, 6, 0);
    lv_obj_t *al = lv_label_create(add);
    lv_label_set_text(al, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(al, lv_color_hex(WG_GREEN_HI), 0);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_28, 0);
    lv_obj_center(al);
    lv_obj_add_event_cb(add, wg_add_channel, LV_EVENT_CLICKED, w);

    wg_refresh_list(w);

    /* 指示灯状态刷新（100ms 轮询） */
    w->refresh_timer = lv_timer_create(wg_tick, 100, NULL);

    ESP_LOGI(WG_TAG, "wave_gen UI created (%dx%d)", sw, sh);
    return root;
}

void wave_gen_destroy(lv_obj_t *root)
{
    wg_t *w = s_wg;
    if (w) {
        if (w->refresh_timer) lv_timer_delete(w->refresh_timer);
        for (int i = 0; i < WG_MAX_CH; i++) {
            if (w->ch_used[i]) drv_wave_ch_stop(i);
        }
    }
    s_wg = NULL;
    if (root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(root);
    }
    free(w);
}

bool wave_gen_swipe_back(lv_obj_t *root)
{
    (void)root;
    /* 右滑逐级返回：键盘 → 设置对话框 → 关闭 APP */
    if (num_input_is_active()) {
        num_input_cancel();   /* 关键盘（取消输入，回配置页） */
        return false;
    }
    if (s_wg && s_wg->modal) {
        wg_modal_close(s_wg);
        return false;
    }
    return true;
}

static void wave_gen_relayout(wg_t *w)
{
    int sw = lv_display_get_horizontal_resolution(lv_display_get_default());
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());
    wg_modal_close(w);
    lv_obj_set_size(w->root, sw, sh);
    lv_obj_set_size(w->list_area, sw, sh - 48);
    lv_obj_set_pos(w->list_area, 0, 0);
    wg_refresh_list(w);
}

void wave_gen_rotate(lv_obj_t *root, int deg)
{
    (void)deg;
    if (s_wg) wave_gen_relayout(s_wg);
}

void wave_gen_debug_event(lv_obj_t *root, int evt)
{
    (void)root;
    (void)evt;
    wg_t *w = s_wg;
    if (!w) return;
    for (int i = 0; i < WG_MAX_CH; i++) {
        if (!w->ch_used[i]) continue;
        ESP_LOGI(WG_TAG, "[DBG] ch%d %s io=%d freq=%d duty=%d%% ms=%d on=%d",
                 i, drv_wave_type_str(w->cfg[i].type), w->cfg[i].io, w->cfg[i].freq_hz,
                 w->cfg[i].duty_pct, w->cfg[i].pulse_ms, drv_wave_ch_active(i));
    }
}
