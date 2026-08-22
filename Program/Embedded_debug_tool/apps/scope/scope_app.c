/* scope_app.c —— 示波器 APP（LVGL 9）：顶栏通道/状态 + 波形 canvas + 测量栏 + 底栏。
 * 采集走 scope/drv_scope（官方 adc_continuous DMA）。 */

#include "scope_app.h"
#include "drv_scope.h"
#include "num_input.h"
#include "gesture.h"
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
#define SC_GRID          0x3A3A44   /* 网格浅灰 */
#define SC_GRID_MID      0x4A4A58   /* 中心横轴（略亮） */
#define SC_TRIG          0xE5484D   /* 触发线红 */
#define SC_SEL           0xFFC857   /* 时间轴滑块（琥珀，50% 半透明混合） */
#define SC_RUN           0x22C55E   /* RUN 状态点绿 */
#define SC_STOP          0x6B7280   /* STOP 状态点灰 */
#define SC_TEXT          0xE8E8F0
#define SC_MEAS          0x94A3B8
#define SC_BTN_ON        0x39C5BB   /* 键按下文字色 */

/* ── 布局 ── */
#define SC_TOP_H    28
#define SC_MEAS_H   32
#define SC_BAR_H    40

/* ── 采样率：num_input 输入（官方合法 611..83333 SPS）。
 * 显示窗口固定 2048 点 → 横轴总时长 = 2048/采样率（时间轴缩放）。
 * 默认 80000（接近官方上限，需更长时间窗时输入更小值）。 */
#define SC_RATE_MIN   611
#define SC_RATE_MAX   83333
#define SC_RATE_DEF   80000

/* ── 水平缩放档位（窗口切片点数 = 2048 >> idx；触发点为中心） ── */
static const char *const s_hzoom_strs[] = { "x1", "x2", "x4", "x8" };
#define SC_HZOOM_N  (sizeof(s_hzoom_strs) / sizeof(s_hzoom_strs[0]))

/* 0V 标签按钮：热区容器（大触摸面积）+ 视觉标签居中（视觉不变大） */
#define Z0_HOT_W   44
#define Z0_HOT_H   30

/* 前向声明（scope_apply_cfg 在定义前调用） */
static void scope_refresh_z0_btns(void);
static void scope_update_z0_pos(void);

/* ── 通道输入 GPIO（ADC1 空闲脚；IO5 板面未引出，改用 9/10） ── */
#define SC_IO_CH1   9
#define SC_IO_CH2   10

/* 垂直范围档位（满量程 raw 值；波形 y 映射按此缩放，双向）：
 *   12V/6V 档 > 4095 → 波形压缩（缩小显示）；
 *   3.1V 默认；1.5V 以下 → 波形放大（0.2V 放大 16 倍） */
static const int s_vranges[] = { 4095, 2047, 1023, 511, 255, 8190, 16380 };
static const char *const s_vrange_strs[] = { "3.1V", "1.5V", "0.8V", "0.4V", "0.2V", "6V", "12V" };
#define SC_VRANGE_N  (sizeof(s_vranges) / sizeof(s_vranges[0]))

/* 通道模式（顶栏循环键） */
typedef enum {
    SC_CH_MODE_CH1 = 0,
    SC_CH_MODE_CH2,
    SC_CH_MODE_DUAL,
} scope_ch_mode_t;

static const char *const s_ch_mode_strs[] = { "CH1", "CH2", "Dual" };
#define SC_CH_MODE_N  (sizeof(s_ch_mode_strs) / sizeof(s_ch_mode_strs[0]))

/* 采样率显示文本：>=1000 → "80k"，否则原值 */
static void sc_rate_label(int hz, char *buf, size_t len)
{
    if (hz >= 1000) {
        snprintf(buf, len, "%dk", hz / 1000);
    } else {
        snprintf(buf, len, "%d", hz);
    }
}

typedef struct {
    lv_obj_t *root;
    /* 顶栏 */
    lv_obj_t *ch_btn, *ch_lbl;
    lv_obj_t *vr_btn, *vr_lbl;      /* 垂直范围档位键 */
    lv_obj_t *hz_btn, *hz_lbl;      /* 水平缩放键（窗口切片） */
    lv_obj_t *state_dot, *state_lbl;
    /* 波形 canvas */
    lv_obj_t *canvas;
    lv_color_t *canvas_buf;
    int canvas_w, canvas_h;
    /* 测量栏 */
    lv_obj_t *m_lbl1, *m_lbl2;
    /* 底栏 */
    lv_obj_t *btn[4];
    lv_obj_t *lbl[4];
    scope_ch_mode_t ch_mode;
    int vr_idx;                      /* 垂直范围档位索引（0=满量程） */
    int hz_idx;                      /* 水平缩放索引（0=×1 全窗口） */
    int ch_off[SCOPE_CH_MAX];        /* 每通道垂直偏移（像素，0 点移动） */
    lv_obj_t *z0_btn[SCOPE_CH_MAX];  /* 0V 标签按钮（热区容器，拖动移通道） */
    int z0_drag_y[SCOPE_CH_MAX];     /* 0V 标签拖动起点 y */
    int hz_pan;                      /* 水平平移（采样点，H 缩放后拖动查看） */
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

/* ── canvas 波形绘制（100ms 节流，全量重绘）
 * 直接写 PSRAM buffer（memset + 像素直写）——lv_draw_line 逐条软件渲染
 * 240 条竖线实测单帧 300-600ms，把 LVGL 任务占死导致触摸无响应；
 * 直写 buffer 单帧 <1ms，invalidate 后由 LVGL 统一 blit 到 LCD。 ── */
static void scope_draw(const scope_frame_t *f)
{
    scope_t *s = s_scope;
    if (!s || !s->canvas || !s->canvas_buf) return;
    int cw = s->canvas_w, chh = s->canvas_h;
    uint16_t *buf = (uint16_t *)s->canvas_buf;
    const uint16_t c_grid = (uint16_t)lv_color_to_u16(lv_color_hex(SC_GRID));
    const uint16_t c_mid  = (uint16_t)lv_color_to_u16(lv_color_hex(SC_GRID_MID));
    const uint16_t c_trig = (uint16_t)lv_color_to_u16(lv_color_hex(SC_TRIG));
    const uint16_t c_w1   = (uint16_t)lv_color_to_u16(lv_color_hex(SC_WAVE1));
    const uint16_t c_w2   = (uint16_t)lv_color_to_u16(lv_color_hex(SC_WAVE2));
    const uint16_t c_bar  = (uint16_t)lv_color_to_u16(lv_color_hex(0x2A3A46));   /* 时间轴横线 */
    /* 滑块：琥珀色 50% 混合黑底 → 半透明观感，与波形色区分 */
    const uint16_t c_sel  = (uint16_t)lv_color_to_u16(
        lv_color_mix(lv_color_hex(SC_SEL), lv_color_hex(0x000000), 128));

    memset(buf, 0, (size_t)cw * chh * 2);   /* 清黑底 */

    /* 垂直余量：有效量程映射到中间（上下各留 margin，避免贴边显示）。
     * 0V → y = chh-margin-1，满量程 → y = margin */
    int margin = chh / 12;
    int usable = chh - 2 * margin;

    /* 网格 10x8（垂直线 9 + 水平线 7，像素直写） */
    for (int i = 1; i < 10; i++) {
        int x = i * cw / 10;
        for (int y = 0; y < chh; y++) buf[y * cw + x] = c_grid;
    }
    for (int j = 1; j < 8; j++) {
        int y = j * chh / 8;
        for (int x = 0; x < cw; x++) buf[y * cw + x] = c_grid;
    }
    /* 中心横轴（1.55V 参考）+ 0V 基准线（主通道 0V 位置，随 ch_off[0] 移动） */
    for (int x = 0; x < cw; x++) buf[(chh / 2) * cw + x] = c_mid;
    int z0_y = margin + usable - 1 + s->ch_off[0];
    if (z0_y < 0) z0_y = 0;
    if (z0_y >= chh) z0_y = chh - 1;
    for (int x = 0; x < cw; x++) buf[z0_y * cw + x] = c_mid;

    /* 波形（峰值检测：每列 min/max 竖线，防漏峰；y 按垂直范围档位 + 余量映射）
     * 单通道时颜色跟随当前通道（CH1=青绿 CH2=橙），Dual 时双色。
     * 水平缩放：显示窗口切片（点数 = 全窗口 >> hz_idx，触发点为中心） */
    int vfull = s_vranges[s->vr_idx];
    if (f && f->frameno && f->points > 0) {
        int npts = f->points >> s->hz_idx;
        if (npts < 1) npts = 1;
        int wstart = (f->points / 4) - npts / 2 + s->hz_pan;   /* 触发点为中心 + 平移 */
        if (wstart < 0) wstart = 0;
        if (wstart + npts > f->points) wstart = f->points - npts;
        int wend = wstart + npts;

        for (int c = 0; c < f->channels && c < SCOPE_CH_MAX; c++) {
            uint16_t cc;
            if (f->channels == 1 && s->ch_mode == SC_CH_MODE_CH2) {
                cc = c_w2;                       /* 单通道 CH2 */
            } else {
                cc = (c == 0) ? c_w1 : c_w2;     /* CH1 或 Dual */
            }
            for (int col = 0; col < cw; col++) {
                int i0 = wstart + col * npts / cw;
                int i1 = wstart + (col + 1) * npts / cw;
                if (i1 <= i0) i1 = i0 + 1;
                if (i0 >= wend) break;
                if (i1 > wend) i1 = wend;
                uint32_t mn = 4096, mx = 0;
                for (int i = i0; i < i1; i++) {
                    if (f->ch[c][i] < mn) mn = f->ch[c][i];
                    if (f->ch[c][i] > mx) mx = f->ch[c][i];
                }
                int y_hi = margin + (usable - 1) - (int)(mx * (uint32_t)(usable - 1) / vfull)
                           + s->ch_off[c];
                int y_lo = margin + (usable - 1) - (int)(mn * (uint32_t)(usable - 1) / vfull)
                           + s->ch_off[c];
                if (y_lo < 0 || y_hi >= chh) continue;   /* 波形完全出屏：跳过（防满屏竖线） */
                if (y_hi < 0) y_hi = 0;          /* 部分出屏 clip */
                if (y_lo >= chh) y_lo = chh - 1;
                if (y_lo < y_hi) y_lo = y_hi;
                uint16_t *p = buf + y_hi * cw + col;
                for (int y = y_hi; y <= y_lo; y++, p += cw) *p = cc;
            }
        }

        /* 右下角时间轴定位条（仅 H 放大后显示，x1 无平移意义时隐藏）：
         * 横线=缩放前全窗口(2048 点)，滑块=当前缩放窗口，
         * 宽度 ∝ 显示点数/全窗口，位置 ∝ 切片起点/全窗口（拖动时实时移动） */
        if (s->hz_idx > 0) {
            int tb_y = chh - 3;
            int tb_w = cw * 45 / 100;
            int tb_x = cw - tb_w - 4;
            for (int x = tb_x; x < tb_x + tb_w; x++) buf[tb_y * cw + x] = c_bar;
            int sw = tb_w * npts / f->points;
            if (sw < 2) sw = 2;
            int sx = tb_x + tb_w * wstart / f->points;
            if (sx + sw > tb_x + tb_w) sx = tb_x + tb_w - sw;
            for (int y = tb_y - 2; y <= tb_y; y++) {
                for (int x = sx; x < sx + sw; x++) buf[y * cw + x] = c_sel;
            }
        }
    }

    /* 触发线：仅左侧短标记（不横贯全屏，避免与波形混淆为第二通道） */
    int ty = margin + (usable - 1) - (int)((uint32_t)s->cfg.trigger_level * (usable - 1) / vfull);
    if (ty < 0) ty = 0;
    if (ty >= chh) ty = chh - 1;
    for (int x = 0; x < 14 && x < cw; x++) buf[ty * cw + x] = c_trig;

    /* 右上角 V 轴指示器（仅放大档 vfull<4095 显示；3.1V 默认与缩小档隐藏）：
     * 琥珀竖线 = 未缩放全范围(0..4095) 的 60% 高（垂直居中，小巧），
     * 琥珀滑块 = 当前显示电压段 [v_lo, v_hi]（随偏移按钮移动） */
    if (vfull < 4095) {
        int vx = cw - 5;                       /* 竖线 x */
        int v_h = (chh - 2 * margin) * 60 / 100;   /* 竖线高 60%，居中 */
        if (v_h < 8) v_h = 8;
        int v_top = margin + (chh - 2 * margin - v_h) / 2;
        int v_bot = v_top + v_h - 1;
        for (int y = v_top; y <= v_bot; y++) buf[y * cw + vx] = c_bar;
        /* ch_off[0] 像素 → 显示段起点电压 v_lo（raw）。
         * 方向：波形上移(ch_off 减小) → v_lo 减小 → 滑块下移（与波形跟手一致） */
        int v_lo = (s->ch_off[0] * vfull / usable);
        int v_hi = v_lo + vfull;
        int y_hi_v = v_bot - v_hi * v_h / 4095;   /* 高电压 → 小 y */
        int y_lo_v = v_bot - v_lo * v_h / 4095;
        if (y_hi_v < v_top) y_hi_v = v_top;
        if (y_lo_v > v_bot) y_lo_v = v_bot;
        if (y_lo_v < y_hi_v) y_lo_v = y_hi_v;
        for (int y = y_hi_v; y <= y_lo_v; y++) {
            for (int x = vx - 1; x <= vx + 2 && x < cw; x++) buf[y * cw + x] = c_sel;
        }
    }

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
static void scope_refresh_status(void)
{
    scope_t *s = s_scope;
    if (!s) return;
    lv_obj_set_style_bg_color(s->state_dot, lv_color_hex(s->running ? SC_RUN : SC_STOP), 0);
    lv_label_set_text(s->state_lbl, s->running ? "RUN" : "STOP");
    lv_label_set_text(s->ch_lbl, s_ch_mode_strs[s->ch_mode]);
    /* 通道键文字色跟随当前通道波形色（CH1=青绿 CH2=橙） */
    lv_obj_set_style_text_color(s->ch_lbl,
                                lv_color_hex(s->ch_mode == SC_CH_MODE_CH2 ? SC_WAVE2 : SC_WAVE1), 0);
    lv_label_set_text(s->vr_lbl, s_vrange_strs[s->vr_idx]);
    lv_label_set_text(s->hz_lbl, s_hzoom_strs[s->hz_idx]);
    lv_label_set_text(s->lbl[0], s->running ? "STOP" : "RUN");   /* 底栏首键 = 当前状态 */

    lv_label_set_text(s->lbl[1], s->cfg.trig_mode == SCOPE_TRIG_AUTO ? "AUTO"
                                     : (s->cfg.trig_mode == SCOPE_TRIG_NORM ? "NORM" : "SINGLE"));
    char rb[16];
    sc_rate_label(s->cfg.sample_rate_hz, rb, sizeof(rb));
    lv_label_set_text(s->lbl[2], rb);
}

/* ── 重启采集（配置变化后，停旧启新） ── */
static void scope_apply_cfg(void)
{
    scope_t *s = s_scope;
    if (!s) return;
    s->last_frameno = 0xFFFFFFFF;   /* 强制下一帧重绘（清掉旧通道/旧档位画面） */
    scope_refresh_z0_btns();        /* 0V 标签可见性跟随通道模式 */
    if (s->running) {
        drv_scope_start(&s->cfg);
    }
    scope_refresh_status();
}

/* ── 事件 ── */

/* 通道键：CH1 → CH2 → Dual 循环（切换后无条件重启采集，刷新波形、
 * 重新开始当前测量模式——SINGLE 重新等触发）。
 * Dual 自动布局：V 档缩到 6V + CH1 0V 基线移上部 1/4、CH2 移下部 3/4，
 * 两波形上下分离不重叠；切回单通道复位偏移 */
static void on_ch_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    ESP_LOGI(S_TAG, "btn: CH cycle");
    s->ch_mode = (scope_ch_mode_t)((s->ch_mode + 1) % SC_CH_MODE_N);
    if (s->ch_mode == SC_CH_MODE_DUAL) {
        s->cfg.io[0] = SC_IO_CH1;
        s->cfg.io[1] = SC_IO_CH2;
        s->vr_idx = 5;   /* 6V 档（s_vranges[5]=8190，波形压缩） */
        int usable = s->canvas_h - 2 * (s->canvas_h / 12);
        s->ch_off[0] = -(usable * 3 / 4);   /* CH1 0V 基线 → 上部 1/4 */
        s->ch_off[1] = -(usable * 1 / 4);   /* CH2 0V 基线 → 下部 3/4 */
    } else {
        s->cfg.io[0] = (s->ch_mode == SC_CH_MODE_CH1) ? SC_IO_CH1 : SC_IO_CH2;
        s->cfg.io[1] = -1;
        s->ch_off[0] = s->ch_off[1] = 0;   /* 单通道复位自动偏移 */
    }
    s->last_frameno = 0;   /* 等新帧 */
    scope_draw(NULL);      /* 立即清空 canvas（空白网格），避免旧通道波形残留 */
    scope_refresh_z0_btns();
    if (drv_scope_start(&s->cfg) == ESP_OK) {   /* 无条件重启 */
        s->running = true;
        s->last_meas_tick = 0;
    }
    scope_refresh_status();
}

/* 垂直范围键：3.1V → 1.5V → 0.8V 循环 */
static void on_vr_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    ESP_LOGI(S_TAG, "btn: V-range cycle");
    s->vr_idx = (s->vr_idx + 1) % SC_VRANGE_N;
    scope_apply_cfg();
}

/* 水平缩放键：x1 → x2 → x4 → x8 循环（显示窗口切片，采集不动）。
 * 放大时禁用全局右/左滑手势（避免拖动被手势 wait_release 中断），
 * 贴边返回不受影响；x1 恢复 */
static void on_hz_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    ESP_LOGI(S_TAG, "btn: H-zoom cycle");
    s->hz_idx = (s->hz_idx + 1) % SC_HZOOM_N;
    s->hz_pan = 0;                  /* 换档重新居中（触发点为中心） */
    gesture_set_global_swipe(s->hz_idx == 0);
    scope_apply_cfg();
}

/* 0V 标签按钮拖动：仅 V 缩小（vfull>4095）时有效——移动整个波形。
 * V 放大时禁用（显示段移动用 canvas 直接滚动；拖 0V 标签会把波形移出屏
 * 触发满屏竖线渲染 bug） */
static void on_z0_press(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target_obj(e);
    int ch = (int)(intptr_t)lv_obj_get_user_data(b);
    scope_t *s = s_scope;
    if (!s || ch < 0 || ch >= SCOPE_CH_MAX) return;
    if (s_vranges[s->vr_idx] < 4095) return;   /* V 放大：0V 标签不响应 */
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s->z0_drag_y[ch] = p.y;
    } else if (lv_event_get_code(e) == LV_EVENT_PRESSING && p.y != s->z0_drag_y[ch]) {
        int dy = p.y - s->z0_drag_y[ch];
        s->ch_off[ch] += dy;
        int lim = s->canvas_h;
        if (s->ch_off[ch] > lim) s->ch_off[ch] = lim;
        if (s->ch_off[ch] < -lim) s->ch_off[ch] = -lim;
        s->z0_drag_y[ch] = p.y;
        scope_draw(s->frame);        /* 同步重绘跟手 */
        scope_update_z0_pos();       /* 标签跟随拖动（不等待 tick） */
    }
}

/* 0V 标签可见性跟随通道模式（单通道只显示对应侧） */
static void scope_refresh_z0_btns(void)
{
    scope_t *s = s_scope;
    if (!s) return;
    for (int ch = 0; ch < 2; ch++) {
        bool vis = (s->ch_mode == SC_CH_MODE_DUAL) ||
                   (s->ch_mode == SC_CH_MODE_CH1 && ch == 0) ||
                   (s->ch_mode == SC_CH_MODE_CH2 && ch == 1);
        if (vis) {
            lv_obj_remove_flag(s->z0_btn[ch], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s->z0_btn[ch], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 0V 标签按钮位置跟随各自通道 0V 线（ch_off 移动显示段/波形） */
static void scope_update_z0_pos(void)
{
    scope_t *s = s_scope;
    if (!s || s->canvas_h <= 0) return;
    int usable = s->canvas_h - 2 * (s->canvas_h / 12);
    int z0_base = s->canvas_h / 12 + usable - 1;
    for (int ch = 0; ch < SCOPE_CH_MAX; ch++) {
        int z0_y = z0_base + s->ch_off[ch];
        if (z0_y < 0) z0_y = 0;
        if (z0_y >= s->canvas_h) z0_y = s->canvas_h - 1;
        int zx = (ch == 0) ? 2 : s->canvas_w - Z0_HOT_W - 2;
        lv_obj_set_pos(s->z0_btn[ch], zx, SC_TOP_H + z0_y - Z0_HOT_H / 2);
    }
}

/* canvas 拖动：水平仅 H 放大后可平移；垂直仅 V 放大（显示段移动）时响应，
 * V 缩小/默认时垂直拖动无效（移动波形只能拖 0V 标签）。拖动重绘节流防 LVGL 满载 */
static int s_canvas_drag_x, s_canvas_drag_y;
static uint32_t s_canvas_last_draw;

static void on_canvas_press(lv_event_t *e)
{
    scope_t *s = s_scope;
    if (!s) return;
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_canvas_drag_x = p.x;
        s_canvas_drag_y = p.y;
        s_canvas_last_draw = 0;
    } else if (lv_event_get_code(e) == LV_EVENT_PRESSING) {
        bool changed = false;
        int vfull = s_vranges[s->vr_idx];
        /* 垂直：仅 V 放大时移动显示段（vfull<4095）；缩小/默认禁用 */
        if (vfull < 4095 && p.y != s_canvas_drag_y) {
            int dy = p.y - s_canvas_drag_y;
            s->ch_off[0] += dy;
            int lim = s->canvas_h;
            if (s->ch_off[0] > lim) s->ch_off[0] = lim;
            if (s->ch_off[0] < -lim) s->ch_off[0] = -lim;
            s_canvas_drag_y = p.y;
            changed = true;
        }
        /* 水平：仅 H 放大后可平移 */
        if (s->hz_idx > 0 && p.x != s_canvas_drag_x) {
            int npts = (s->frame && s->frame->points) ? (s->frame->points >> s->hz_idx) : 0;
            if (npts >= 1 && s->canvas_w > 0) {
                int per_px = (npts + s->canvas_w - 1) / s->canvas_w;   /* 像素 → 采样点 */
                s->hz_pan += (s_canvas_drag_x - p.x) * per_px;   /* 右拖看更早（跟手） */
                int base = s->frame->points / 4;
                int max = s->frame->points - npts;
                if (s->hz_pan < -base) s->hz_pan = -base;
                if (s->hz_pan > max - base) s->hz_pan = max - base;
                s_canvas_drag_x = p.x;
                changed = true;
            }
        }
        if (changed) {
            /* 重绘节流（≥50ms≈20fps）：PSRAM canvas blit ~20-40ms，重绘频率
             * 超过渲染能力会让 LVGL 任务持续满载 → IDLE0 饿死 → WDT */
            uint32_t now = lv_tick_get();
            if (now - s_canvas_last_draw >= 50) {
                s_canvas_last_draw = now;
                scope_draw(s->frame);
                scope_update_z0_pos();   /* 0V 标签跟随拖动 */
            }
        }
    }
}

static void on_run_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    ESP_LOGI(S_TAG, "btn: RUN/STOP");
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
    ESP_LOGI(S_TAG, "btn: TRIG cycle");
    s->cfg.trig_mode = (scope_trig_mode_t)(((int)s->cfg.trig_mode + 1) % 3);
    scope_apply_cfg();
}

/* BASE 键：num_input 输入采样率（611..83333，默认最大值 80000） */
static void on_rate_done(void *ctx, bool ok, int value)
{
    scope_t *s = ctx;
    if (!s) return;
    if (ok) {
        ESP_LOGI(S_TAG, "btn: BASE done rate=%d", value);
        s->cfg.sample_rate_hz = value;
        scope_apply_cfg();
    }
}

static void on_rate_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    ESP_LOGI(S_TAG, "btn: BASE (sample rate)");
    num_input_show(s->root, s->cfg.sample_rate_hz, SC_RATE_MIN, SC_RATE_MAX, false,
                   on_rate_done, s);
}

static void on_v_done(void *ctx, bool ok, int value)
{
    scope_t *s = ctx;
    if (!s) return;
    if (ok) {
        ESP_LOGI(S_TAG, "btn: V done level=%d", value);
        s->cfg.trigger_level = value;
        scope_apply_cfg();
    }
}

static void on_v_btn(lv_event_t *e)
{
    (void)e;
    scope_t *s = s_scope;
    if (!s) return;
    ESP_LOGI(S_TAG, "btn: V (trigger level)");
    num_input_show(s->root, s->cfg.trigger_level, 0, 4095, false, on_v_done, s);
}

/* ── 定时器：100ms 波形帧 + 500ms 测量帧 ── */
static void scope_tick(lv_timer_t *t)
{
    (void)t;
    scope_t *s = s_scope;
    if (!s) return;
    if (num_input_is_active()) return;

    uint32_t t0 = lv_tick_get();
    if (drv_scope_get_frame(s->frame) != ESP_OK) {
        ESP_LOGW(S_TAG, "tick: get_frame failed");
        return;
    }
    const scope_frame_t *f = s->frame;

    if (f->frameno != s->last_frameno) {
        /* 首帧打印一次（采集链路是否出数） */
        if (s->last_frameno == 0) {
            ESP_LOGI(S_TAG, "tick: first frame #%u (%d pts, %d ch)", f->frameno,
                     f->points, f->channels);
        }
        s->last_frameno = f->frameno;
        if (f->running != s->running) {
            s->running = f->running;
            scope_refresh_status();
        }
        scope_draw(f);
        scope_update_meas(f);
        scope_update_z0_pos();   /* 0V 标签位置跟随各自通道 0V 线 */
    }
    uint32_t dt = lv_tick_get() - t0;
    if (dt > 50) {
        ESP_LOGW(S_TAG, "tick: slow cycle %ums (draw too heavy?)", dt);
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
    int cw = sw, chh = sh - SC_TOP_H - SC_MEAS_H - SC_BAR_H;

    lv_obj_set_size(s->root, sw, sh);

    lv_obj_set_pos(s->ch_btn, 8, 2);
    lv_obj_set_size(s->ch_btn, 56, SC_TOP_H - 4);
    lv_obj_set_pos(s->vr_btn, 68, 2);
    lv_obj_set_size(s->vr_btn, 56, SC_TOP_H - 4);
    lv_obj_set_pos(s->hz_btn, 128, 2);
    lv_obj_set_size(s->hz_btn, 48, SC_TOP_H - 4);
    lv_obj_align(s->state_dot, LV_ALIGN_TOP_RIGHT, -56, 9);
    lv_obj_align(s->state_lbl, LV_ALIGN_TOP_RIGHT, -8, 7);

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
    /* 0V 标签按钮初始位置（后续 scope_tick 每帧跟随 0V 线） */
    for (int ch = 0; ch < SCOPE_CH_MAX; ch++) {
        int zx = (ch == 0) ? 2 : cw - Z0_HOT_W - 2;
        lv_obj_set_pos(s->z0_btn[ch], zx, SC_TOP_H + chh / 2 - Z0_HOT_H / 2);
        lv_obj_set_size(s->z0_btn[ch], Z0_HOT_W, Z0_HOT_H);
    }

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
    s->ch_mode = SC_CH_MODE_CH1;
    s->vr_idx = 0;
    s->hz_idx = 0;
    s->cfg.sample_rate_hz = SC_RATE_DEF;   /* 默认最大值 80k */
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

    /* 顶栏垂直范围键（3.1V/1.5V/0.8V 循环） */
    lv_obj_t *vr_btn = sc_make_btn(root, "3.1V");
    lv_obj_add_event_cb(vr_btn, on_vr_btn, LV_EVENT_CLICKED, NULL);
    s->vr_btn = vr_btn;
    s->vr_lbl = lv_obj_get_child(vr_btn, 0);

    /* 顶栏水平缩放键（x1/x2/x4/x8 窗口切片） */
    lv_obj_t *hz_btn = sc_make_btn(root, "x1");
    lv_obj_add_event_cb(hz_btn, on_hz_btn, LV_EVENT_CLICKED, NULL);
    s->hz_btn = hz_btn;
    s->hz_lbl = lv_obj_get_child(hz_btn, 0);

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
    /* canvas 左右拖动 = H 缩放平移。lv_canvas 默认非 CLICKABLE（命中测试
     * 跳过），必须显式加 flag 才能收到 PRESSED/PRESSING */
    lv_obj_add_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cv, on_canvas_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(cv, on_canvas_press, LV_EVENT_PRESSING, NULL);

    /* 0V 标签按钮（每通道一个）：透明热区容器（大触摸）+ 半透明带色视觉标签。
     * 拖动标签 = 上下移动该通道波形（0V 线跟随）。CH1 左缘、CH2 右缘。 */
    for (int ch = 0; ch < SCOPE_CH_MAX; ch++) {
        lv_obj_t *hot = lv_obj_create(root);
        lv_obj_remove_flag(hot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                           | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_set_style_bg_opa(hot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hot, 0, 0);
        lv_obj_set_style_pad_all(hot, 0, 0);
        lv_obj_set_user_data(hot, (void *)(intptr_t)ch);
        lv_obj_add_event_cb(hot, on_z0_press, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(hot, on_z0_press, LV_EVENT_PRESSING, NULL);
        /* 视觉标签：半透明通道色背景 + 白字 0V */
        lv_obj_t *vis = lv_label_create(hot);
        lv_label_set_text(vis, "0V");
        lv_obj_set_style_text_font(vis, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(vis, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(vis, lv_color_hex(ch == 0 ? SC_WAVE1 : SC_WAVE2), 0);
        lv_obj_set_style_bg_opa(vis, LV_OPA_30, 0);
        lv_obj_set_style_pad_hor(vis, 8, 0);
        lv_obj_set_style_pad_ver(vis, 3, 0);
        lv_obj_set_style_radius(vis, 4, 0);
        lv_obj_center(vis);
        s->z0_btn[ch] = hot;
    }

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

    const char *btns[] = { "RUN", "AUTO", "80k", "V" };
    lv_event_cb_t cbs[] = { on_run_btn, on_trig_btn, on_rate_btn, on_v_btn };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = sc_make_btn(root, btns[i]);
        lv_obj_add_event_cb(b, cbs[i], LV_EVENT_CLICKED, NULL);
        s->btn[i] = b;
        s->lbl[i] = lv_obj_get_child(b, 0);
    }

    scope_relayout();
    ESP_LOGI(S_TAG, "create: UI built (%dx%d, canvas %dx%d, %dKB PSRAM)", sc_screen_w(),
             sc_screen_h(), s->canvas_w, s->canvas_h,
             (int)(s->canvas_w * s->canvas_h * 2 / 1024));

    s->tick = lv_timer_create(scope_tick, 100, NULL);

    /* 采集启动（默认 AUTO 40k CH1）——失败不崩，UI 显示 STOP 可重试 */
    esp_err_t sr = drv_scope_start(&s->cfg);
    if (sr == ESP_OK) {
        s->running = true;
        ESP_LOGI(S_TAG, "create: acquisition started");
    } else {
        ESP_LOGE(S_TAG, "create: drv_scope_start FAILED: %s (UI stays, STOP state)",
                 esp_err_to_name(sr));
    }
    scope_refresh_status();
    scope_refresh_z0_btns();   /* 初始 CH1 模式：只显示 CH1 的 0V 标签 */

    /* 0V 标签提到最上层：创建顺序在 canvas 之前，会被全黑 canvas 盖住 */
    for (int ch = 0; ch < SCOPE_CH_MAX; ch++) {
        lv_obj_move_foreground(s->z0_btn[ch]);
    }

    ESP_LOGI(S_TAG, "scope UI created (%dx%d)", sc_screen_w(), sc_screen_h());
    return root;
}

void scope_destroy(lv_obj_t *root)
{
    scope_t *s = s_scope;
    if (s) {
        gesture_set_global_swipe(true);   /* 恢复全局手势（H 放大可能禁用了） */
        drv_scope_deinit();   /* 停采集 + 释放 ADC/缓冲（给其他 APP 腾内部 RAM） */
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
