/* meter_app.c —— 多路电压表 APP「Meter」（LVGL 9）：通道卡片列表 + 展开波形。
 * 采集走 meter/drv_meter（adc_continuous 100kHz，4s 窗口）。
 * 卡片：✕删除 / CH#·GPIO# / ▶开始·⏸暂停 / ~波形开关 / 当前电压 / MAX·MIN；
 * 点卡片主体 → io_picker(IO_CAP_ADC1)。波形展开：内嵌 canvas（宽占满、高 52），
 * V 轴自动跟随窗口 [min,max]，琥珀虚线 + 数值标注 MAX/MIN。 */

#include "meter_app.h"
#include "drv_meter.h"
#include "io_picker.h"
#include "launcher.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define M_TAG "meter"

/* ── 配色（与 wave/scope 一致：Miku 绿暗色） ── */
#define MET_BG        0x000000
#define MET_CARD      0x050B0A
#define MET_BORDER    0x123B36
#define MET_TEXT      0xE6F0EE
#define MET_DIM       0x94A3B8
#define MET_GREEN     0x39C5BB
#define MET_GREEN_HI  0x7FE5DC
#define MET_AMBER     0xFBBF24
#define MET_RUN       0x22C55E
#define MET_STOP      0xE5484D

/* ── 布局 ── */
#define MET_BAR_H     48   /* 底部添加栏 */
/* 波形 canvas 高 = 屏幕高度 1/3（竖屏 320→106px；旋转后按屏高动态计算，
 * 在 meter_refresh_list 内取值，不在此处固定） */

typedef struct {
    lv_obj_t *root;
    meter_back_cb_t back_cb;
    void *ctx;

    bool ch_used[METER_CH_MAX];
    int  ch_io[METER_CH_MAX];     /* -1=未选 */
    bool ch_run[METER_CH_MAX];    /* 采集中 */
    bool ch_exp[METER_CH_MAX];    /* 波形展开 */

    /* 显示缓存（暂停/停止后冻结显示用） */
    float v_cur[METER_CH_MAX], v_min[METER_CH_MAX], v_max[METER_CH_MAX];
    int   ch_rate[METER_CH_MAX];

    lv_obj_t *list_area;
    /* 每槽位对象引用（重建列表后刷新用） */
    lv_obj_t *val_lbl[METER_CH_MAX];
    lv_obj_t *mm_lbl[METER_CH_MAX];
    lv_obj_t *run_btn[METER_CH_MAX];
    lv_obj_t *wave_lbl[METER_CH_MAX];    /* 波形区 MAX/MIN 标注文字 */
    lv_obj_t *canvas[METER_CH_MAX];
    lv_color_t *canvas_buf[METER_CH_MAX];
    int canvas_w, canvas_h;              /* canvas 尺寸（每槽相同） */
    lv_timer_t *tick;

    int pick_slot;   /* io_picker 回调目标槽位 */
} meter_t;

static meter_t *s_meter;

static int meter_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_horizontal_resolution(d) : 240;
}

static int meter_screen_h(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_vertical_resolution(d) : 320;
}

static void meter_refresh_list(meter_t *m);

/* ── 采集集重建：运行中通道的 IO 列表 → drv_meter_start/stop ── */

static void meter_apply_channels(meter_t *m)
{
    /* 槽位数组传给驱动：io[i] < 0 = 未激活；槽位索引即驱动通道索引，
     * UI 取快照/波形用同一索引（紧凑列表会错位，不可用） */
    int ios[METER_CH_MAX];
    int n = 0;
    for (int i = 0; i < METER_CH_MAX; i++) {
        ios[i] = (m->ch_used[i] && m->ch_run[i]) ? m->ch_io[i] : -1;
        if (ios[i] >= 0) n++;
    }
    if (n == 0) {
        drv_meter_stop();
    } else {
        if (drv_meter_start(ios) != ESP_OK) {
            ESP_LOGE(M_TAG, "drv_meter_start failed (%d ch)", n);
        }
    }
}

/* ── IO 选择回调（io_picker，单选即回） ── */

static void meter_io_picked(void *ctx, int io)
{
    meter_t *m = ctx;
    if (!m) return;
    int slot = m->pick_slot;
    if (io < 0) return;   /* 取消 */
    if (slot < 0 || slot >= METER_CH_MAX || !m->ch_used[slot]) return;

    /* 其它通道已占用同一脚 → 忽略（picker 已按账本置灰，防御） */
    for (int i = 0; i < METER_CH_MAX; i++) {
        if (i != slot && m->ch_used[i] && m->ch_io[i] == io) return;
    }

    if (m->ch_io[slot] >= 0) io_picker_release(m->ch_io[slot]);
    m->ch_io[slot] = io;
    io_picker_reserve(io);
    m->v_cur[slot] = 0; m->v_min[slot] = 0; m->v_max[slot] = 0;
    meter_apply_channels(m);   /* 换 IO 后若已在运行则重配 */
    meter_refresh_list(m);
}

static void meter_card_click(lv_event_t *e)
{
    meter_t *m = s_meter;
    if (!m) return;
    int slot = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (slot < 0 || slot >= METER_CH_MAX || !m->ch_used[slot]) return;
    m->pick_slot = slot;
    /* 仅 ADC1（S3 连续 DMA 只支持 ADC1） */
    io_picker_show(m->root, IO_CAP_ADC1, meter_io_picked, m);
}

/* ── 卡片内按钮 ── */

static void meter_del_click(lv_event_t *e)
{
    meter_t *m = s_meter;
    if (!m) return;
    int slot = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (slot < 0 || slot >= METER_CH_MAX || !m->ch_used[slot]) return;
    if (m->ch_io[slot] >= 0) io_picker_release(m->ch_io[slot]);
    m->ch_used[slot] = false;
    m->ch_run[slot] = false;
    m->ch_exp[slot] = false;
    meter_apply_channels(m);
    meter_refresh_list(m);
}

static void meter_run_click(lv_event_t *e)
{
    meter_t *m = s_meter;
    if (!m) return;
    int slot = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (slot < 0 || slot >= METER_CH_MAX || !m->ch_used[slot]) return;
    if (m->ch_io[slot] < 0) return;   /* 未选 IO：先点卡片选 */
    m->ch_run[slot] = !m->ch_run[slot];
    if (m->ch_run[slot]) {
        /* 开始：清零 min/max 重记 */
        m->v_cur[slot] = 0; m->v_min[slot] = 0; m->v_max[slot] = 0;
    }
    meter_apply_channels(m);
    meter_refresh_list(m);
}

static void meter_wave_click(lv_event_t *e)
{
    meter_t *m = s_meter;
    if (!m) return;
    int slot = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (slot < 0 || slot >= METER_CH_MAX || !m->ch_used[slot]) return;
    m->ch_exp[slot] = !m->ch_exp[slot];
    meter_refresh_list(m);
}

/* ── 波形绘制（直写 canvas buffer，仿 scope_draw） ── */

static void meter_draw_wave(meter_t *m, int slot)
{
    lv_obj_t *cv = m->canvas[slot];
    if (!cv || !m->canvas_buf[slot]) return;
    int cw = m->canvas_w, chh = m->canvas_h;
    uint16_t *buf = (uint16_t *)m->canvas_buf[slot];
    const uint16_t c_grid = (uint16_t)lv_color_to_u16(lv_color_hex(0x1E2A2E));
    const uint16_t c_amb  = (uint16_t)lv_color_to_u16(lv_color_hex(MET_AMBER));
    const uint16_t c_wave = (uint16_t)lv_color_to_u16(lv_color_hex(MET_GREEN_HI));

    memset(buf, 0, (size_t)cw * chh * 2);

    float col_min[240], col_max[240];
    int ncols = cw < 240 ? cw : 240;
    float wmin = 0, wmax = 3.1f;
    if (drv_meter_get_wave(slot, ncols, col_min, col_max, &wmin, &wmax) != ESP_OK) {
        wmin = 0; wmax = 3.1f;
    }
    /* V 轴量程固定 [-0.1V, 3.4V]：0V/3.3V 不贴 canvas 上下边缘，观感直观
     * （不增减 canvas 高度，仅压缩显示精度）。MAX/MIN 虚线画在实际数据极值处 */
    const float lo = -0.1f, hi = 3.4f;

    int margin = 3;
    int usable = chh - 2 * margin;

    /* 水平网格（3 条浅线） */
    for (int j = 1; j < 4; j++) {
        int y = margin + j * usable / 4;
        if (y >= chh) y = chh - 1;
        for (int x = 0; x < cw; x++) buf[y * cw + x] = c_grid;
    }

    /* 波形：每列 min/max 竖线（V 映射：高电压 → 小 y，量程 lo..hi） */
    for (int col = 0; col < ncols; col++) {
        int y_hi = margin + (int)((hi - col_max[col]) / (hi - lo) * (usable - 1) + 0.5f);
        int y_lo = margin + (int)((hi - col_min[col]) / (hi - lo) * (usable - 1) + 0.5f);
        if (y_hi < 0) y_hi = 0;
        if (y_lo >= chh) y_lo = chh - 1;
        if (y_lo < y_hi) y_lo = y_hi;
        uint16_t *p = buf + y_hi * cw + col;
        for (int y = y_hi; y <= y_lo; y++, p += cw) *p = c_wave;
    }

    /* MAX/MIN 琥珀虚线：画在实际数据极值（wmin/wmax）所在行，钳制在 canvas 内 */
    {
        int y_max = margin + (int)((hi - wmax) / (hi - lo) * (usable - 1) + 0.5f);
        int y_min = margin + (int)((hi - wmin) / (hi - lo) * (usable - 1) + 0.5f);
        if (y_max < 0) y_max = 0;
        if (y_max >= chh) y_max = chh - 1;
        if (y_min < 0) y_min = 0;
        if (y_min >= chh) y_min = chh - 1;
        for (int x = 0; x < cw; x += 5) {
            if (x + 2 > cw) break;
            for (int k = 0; k < 2; k++) {
                int xx = x + k;
                if (xx < cw) buf[y_max * cw + xx] = c_amb;
                buf[y_min * cw + xx] = c_amb;
            }
        }
    }

    lv_obj_invalidate(cv);

    /* 标注文字：实际数据 MAX/MIN 数值（琥珀） */
    if (m->wave_lbl[slot]) {
        char b[40];
        snprintf(b, sizeof(b), "MAX %.3fV  MIN %.3fV", wmax, wmin);
        lv_label_set_text(m->wave_lbl[slot], b);
    }
}

/* ── tick：刷新值 + 波形（100ms） ── */

static void meter_tick(lv_timer_t *t)
{
    (void)t;
    meter_t *m = s_meter;
    if (!m) return;

    for (int i = 0; i < METER_CH_MAX; i++) {
        if (!m->ch_used[i]) continue;
        if (m->ch_run[i]) {
            meter_ch_snap_t sn;
            if (drv_meter_get_snap(i, &sn) == ESP_OK) {
                m->v_cur[i] = sn.v_now;
                m->v_min[i] = sn.v_min;   /* 驱动自 start 起累计 */
                m->v_max[i] = sn.v_max;
                m->ch_rate[i] = sn.rate_hz;
                /* running 状态回读（SINGLE 类自动停等场景） */
                if (!sn.running) m->ch_run[i] = false;
            }
        }
        if (m->val_lbl[i]) {
            char b[24];
            snprintf(b, sizeof(b), "%.3fV", m->v_cur[i]);
            lv_label_set_text(m->val_lbl[i], b);
        }
        if (m->mm_lbl[i]) {
            char b[40];
            snprintf(b, sizeof(b), "MAX %.3f\nMIN %.3f", m->v_max[i], m->v_min[i]);
            lv_label_set_text(m->mm_lbl[i], b);
        }
        if (m->canvas[i] && m->ch_run[i]) {
            meter_draw_wave(m, i);
        }
    }
}

/* ── 空状态 ── */

static void meter_show_empty(lv_obj_t *list)
{
    lv_obj_t *lbl = lv_label_create(list);
    lv_label_set_text(lbl, "no channel\npress + to add");
    lv_obj_set_style_text_color(lbl, lv_color_hex(MET_DIM), 0);
    lv_obj_center(lbl);
}

static lv_obj_t *meter_make_icon_btn(lv_obj_t *parent, const char *txt,
                                     lv_color_t border, lv_color_t txtcol)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 24, 20);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0E141C), 0);
    lv_obj_set_style_border_color(b, border, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, txtcol, 0);
    lv_obj_center(l);
    return b;
}

/* ── 重建列表 ── */

static void meter_refresh_list(meter_t *m)
{
    int list_w = meter_screen_w() - 12;
    int wave_h = meter_screen_h() / 3;   /* canvas 高 = 屏高 1/3（竖屏 320→106） */
    /* 旋转后尺寸变化 → 旧 canvas 缓冲尺寸不匹配，统一释放重建 */
    if (m->canvas_w != list_w || m->canvas_h != wave_h) {
        for (int i = 0; i < METER_CH_MAX; i++) {
            if (m->canvas_buf[i]) {
                heap_caps_free(m->canvas_buf[i]);
                m->canvas_buf[i] = NULL;
            }
        }
    }

    lv_obj_clean(m->list_area);
    for (int i = 0; i < METER_CH_MAX; i++) {
        m->val_lbl[i] = NULL;
        m->mm_lbl[i] = NULL;
        m->run_btn[i] = NULL;
        m->wave_lbl[i] = NULL;
        m->canvas[i] = NULL;
    }

    int used = 0;
    for (int i = 0; i < METER_CH_MAX; i++) if (m->ch_used[i]) used++;
    if (used == 0) {
        meter_show_empty(m->list_area);
        return;
    }

    for (int i = 0; i < METER_CH_MAX; i++) {
        if (!m->ch_used[i]) continue;
        bool exp = m->ch_exp[i];

        lv_obj_t *card = lv_button_create(m->list_area);
        /* 展开：行1(24)+行2(24)+标注行(12)+canvas(wave_h)+底2；折叠：58 */
        lv_obj_set_size(card, lv_pct(100), exp ? (68 + wave_h) : 58);
        lv_obj_set_style_bg_color(card, lv_color_hex(MET_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(MET_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 8, 0);
        /* 绝对定位子对象：按钮默认 padding 会整体偏移，须归零 */
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, meter_card_click, LV_EVENT_CLICKED, m);

        /* 行1：✕ CH# GPIO# ··· ▶/⏸ ~ */
        lv_obj_t *row1 = lv_obj_create(card);
        lv_obj_set_pos(row1, 6, 2);
        lv_obj_set_size(row1, list_w - 12, 24);
        lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row1, 0, 0);
        lv_obj_set_style_pad_all(row1, 0, 0);
        lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row1, 6, 0);
        lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *del = meter_make_icon_btn(row1, LV_SYMBOL_CLOSE, lv_color_hex(MET_STOP),
                                            lv_color_hex(MET_DIM));
        lv_obj_set_user_data(del, (void *)(intptr_t)i);
        lv_obj_add_event_cb(del, meter_del_click, LV_EVENT_CLICKED, m);

        lv_obj_t *cl = lv_label_create(row1);
        lv_label_set_text_fmt(cl, "CH%d", i);
        lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(MET_GREEN_HI), 0);

        lv_obj_t *iol = lv_label_create(row1);
        lv_label_set_text_fmt(iol, m->ch_io[i] >= 0 ? "IO%d" : "IO--", m->ch_io[i]);
        lv_obj_set_style_text_font(iol, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(iol, lv_color_hex(MET_DIM), 0);

        lv_obj_t *sp = lv_obj_create(row1);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* ▶(未运行)/⏸(运行中) */
        lv_obj_t *run = meter_make_icon_btn(row1,
                                            m->ch_run[i] ? "||" : ">",
                                            lv_color_hex(m->ch_run[i] ? MET_RUN : MET_GREEN),
                                            lv_color_hex(m->ch_run[i] ? MET_RUN : MET_GREEN_HI));
        m->run_btn[i] = run;
        lv_obj_set_user_data(run, (void *)(intptr_t)i);
        lv_obj_add_event_cb(run, meter_run_click, LV_EVENT_CLICKED, m);

        /* ~ 波形开关 */
        lv_obj_t *wb = meter_make_icon_btn(row1, "~",
                                           lv_color_hex(exp ? MET_GREEN : MET_DIM),
                                           lv_color_hex(exp ? MET_GREEN_HI : MET_TEXT));
        lv_obj_set_user_data(wb, (void *)(intptr_t)i);
        lv_obj_add_event_cb(wb, meter_wave_click, LV_EVENT_CLICKED, m);

        /* 行2：当前电压（+ 折叠态 MAX/MIN —— 展开时波形区已有标注，避免重复） */
        lv_obj_t *row2 = lv_obj_create(card);
        lv_obj_set_pos(row2, 6, 26);
        lv_obj_set_size(row2, list_w - 12, 24);
        lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row2, 0, 0);
        lv_obj_set_style_pad_all(row2, 0, 0);
        lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *vl = lv_label_create(row2);
        lv_label_set_text_fmt(vl, "%.3fV", m->v_cur[i]);
        lv_obj_set_style_text_font(vl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(vl, lv_color_hex(MET_GREEN_HI), 0);
        m->val_lbl[i] = vl;

        if (!exp) {
            lv_obj_t *mm = lv_label_create(row2);
            lv_label_set_text_fmt(mm, "MAX %.3f\nMIN %.3f", m->v_max[i], m->v_min[i]);
            lv_obj_set_style_text_font(mm, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(mm, lv_color_hex(MET_AMBER), 0);
            lv_obj_align(mm, LV_ALIGN_RIGHT_MID, 0, 0);
            m->mm_lbl[i] = mm;
        }

        if (exp) {
            /* 波形区：紧贴行2（y=52）——标注文字行 + 下方 canvas，不裁剪 */
            lv_obj_t *wc = lv_obj_create(card);
            lv_obj_set_pos(wc, 6, 52);
            lv_obj_set_size(wc, list_w - 12, 12 + wave_h);
            lv_obj_set_style_bg_opa(wc, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(wc, 0, 0);
            lv_obj_clear_flag(wc, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *wl = lv_label_create(wc);
            lv_obj_set_style_text_font(wl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(wl, lv_color_hex(MET_AMBER), 0);
            lv_obj_set_pos(wl, 4, 0);
            m->wave_lbl[i] = wl;

            if (!m->canvas_buf[i]) {
                m->canvas_buf[i] = heap_caps_aligned_alloc(128, (size_t)list_w * wave_h * 2,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (m->canvas_buf[i]) {
                lv_obj_t *cv = lv_canvas_create(wc);
                lv_canvas_set_buffer(cv, m->canvas_buf[i], list_w, wave_h, LV_COLOR_FORMAT_RGB565);
                lv_obj_set_pos(cv, 0, 12);
                m->canvas_w = list_w;
                m->canvas_h = wave_h;
                m->canvas[i] = cv;
                /* 暂停态也显示一帧静态波形（数据还在 ring） */
                meter_draw_wave(m, i);
            }
        }
    }
}

/* ── 添加通道 ── */

static void meter_add_channel(lv_event_t *e)
{
    (void)e;
    meter_t *m = s_meter;
    if (!m) return;
    int slot = -1;
    for (int i = 0; i < METER_CH_MAX; i++) {
        if (!m->ch_used[i]) { slot = i; break; }
    }
    if (slot < 0) return;
    m->ch_used[slot] = true;
    m->ch_io[slot] = -1;    /* 待点卡片选择 */
    m->ch_run[slot] = false;   /* 默认暂停 */
    m->ch_exp[slot] = false;
    m->v_cur[slot] = 0; m->v_min[slot] = 0; m->v_max[slot] = 0;
    m->ch_rate[slot] = 0;
    meter_refresh_list(m);
}

/* ── Public API ── */

lv_obj_t *meter_create(lv_obj_t *parent, meter_back_cb_t back_cb, void *ctx)
{
    int sw = meter_screen_w(), sh = meter_screen_h();

    meter_t *m = malloc(sizeof(meter_t));
    if (!m) return NULL;
    memset(m, 0, sizeof(meter_t));
    m->back_cb = back_cb;
    m->ctx = ctx;
    for (int i = 0; i < METER_CH_MAX; i++) m->ch_io[i] = -1;
    s_meter = m;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, sw, sh);
    lv_obj_set_style_bg_color(root, lv_color_hex(MET_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    m->root = root;

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_set_size(list, sw, sh - MET_BAR_H);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_gap(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    m->list_area = list;

    /* 底部添加通道栏 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, sw, MET_BAR_H);
    lv_obj_set_pos(bar, 0, sh - MET_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(MET_CARD), 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(MET_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *add = lv_button_create(bar);
    lv_obj_set_size(add, sw - 16, MET_BAR_H - 10);
    lv_obj_set_style_bg_color(add, lv_color_hex(0x0E141C), 0);
    lv_obj_set_style_border_color(add, lv_color_hex(MET_GREEN), 0);
    lv_obj_set_style_border_width(add, 1, 0);
    lv_obj_set_style_radius(add, 6, 0);
    lv_obj_set_style_pad_all(add, 0, 0);
    lv_obj_t *al = lv_label_create(add);
    lv_label_set_text(al, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(al, lv_color_hex(MET_GREEN_HI), 0);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_28, 0);
    lv_obj_center(al);
    lv_obj_add_event_cb(add, meter_add_channel, LV_EVENT_CLICKED, m);

    meter_refresh_list(m);

    m->tick = lv_timer_create(meter_tick, 100, NULL);

    ESP_LOGI(M_TAG, "Meter UI created (%dx%d)", sw, sh);
    return root;
}

void meter_destroy(lv_obj_t *root)
{
    meter_t *m = s_meter;
    if (io_picker_active()) io_picker_close_now();
    if (m) {
        drv_meter_stop();
        drv_meter_deinit();   /* 释放 adc_continuous/校准/ring（给其他 APP 腾 RAM） */
        for (int i = 0; i < METER_CH_MAX; i++) {
            if (m->ch_io[i] >= 0) io_picker_release(m->ch_io[i]);
            if (m->canvas_buf[i]) {
                heap_caps_free(m->canvas_buf[i]);
                m->canvas_buf[i] = NULL;
            }
        }
        if (m->tick) lv_timer_delete(m->tick);
    }
    s_meter = NULL;
    if (root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(root);
    }
    free(m);
}

bool meter_swipe_back(lv_obj_t *root)
{
    (void)root;
    return true;   /* 无内部层级；io_picker 激活时放行拖动（drag_root 拖选择器） */
}

lv_obj_t *meter_drag_root(void *app)
{
    (void)app;
    return io_picker_active() ? io_picker_get_obj() : NULL;
}

void meter_drag_exit(void *app)
{
    (void)app;
    if (io_picker_active()) {
        io_picker_cancel();   /* 仅关选择器（回调 -1），APP 原位保留 */
    } else {
        launcher_app_close(NULL);
    }
}

static void meter_relayout(meter_t *m)
{
    int sw = meter_screen_w(), sh = meter_screen_h();
    lv_obj_set_size(m->root, sw, sh);
    lv_obj_set_size(m->list_area, sw, sh - MET_BAR_H);
    meter_refresh_list(m);
}

void meter_rotate(lv_obj_t *root, int deg)
{
    (void)deg;
    if (s_meter) meter_relayout(s_meter);
}

void meter_debug_event(lv_obj_t *root, int evt)
{
    (void)root;
    (void)evt;
    meter_t *m = s_meter;
    if (!m) return;
    for (int i = 0; i < METER_CH_MAX; i++) {
        if (!m->ch_used[i]) continue;
        ESP_LOGI(M_TAG, "[DBG] ch%d io=%d run=%d exp=%d v=%.3f min=%.3f max=%.3f rate=%d",
                 i, m->ch_io[i], m->ch_run[i], m->ch_exp[i], m->v_cur[i],
                 m->v_min[i], m->v_max[i], m->ch_rate[i]);
    }
}