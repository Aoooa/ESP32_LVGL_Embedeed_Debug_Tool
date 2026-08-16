#include "launcher.h"
#include <string.h>


/* ── App 占位表（纯文字占位，每卡最多 3 行，每行 ≤15 字符防折行） ── */
#define APP_COUNT 6

static const char *const s_app_texts[APP_COUNT] = {
    "UART 1\nSerial bridge\nTCP :8080",
    "WiFi AP\nDebug console\n192.168.4.1",
    "Web UI\nHTTP + WS\n192.168.4.1",
    "File Browser\nSD card files\nTXT reader",
    "TXT Reader\nLarge files\nOn-demand",
    "Clock\nTime display\nPlaceholder",
};

/* ── 主题色 ── */
#define ACCENT_COLOR     0x39C5BB   /* 初音绿：卡片边框固定色 */
#define THEME_DARK_BG    0x0D0D0D
#define THEME_DARK_TEXT  0xFFFFFF
#define THEME_LIGHT_BG   0xF5F5F5
#define THEME_LIGHT_TEXT 0x374151

/* ── 布局参数 ── */
#define CARD_W_RATIO     60   /* 卡片宽占屏宽百分比（3/5） */
#define CARD_LEFT_GAP    8    /* 滚筒左侧距屏幕左边框 */
#define CARD_MAX_LINES   3    /* 每卡最多行数（卡片高度按此计算） */
#define CARD_H_PAD       8    /* 卡片内上下留白 */
#define CARD_W_PAD       12   /* 卡片内左右留白 */
#define CARD_BORDER      4    /* 斜切边框厚度 */
#define CARD_CHAMFER     10   /* 四角斜切量（斜线长度） */
#define CARD_BIAS        3    /* 斜切角内边偏移（≈边框厚/√2） */
#define FONT_LINE_H      16   /* lv_font_montserrat_14 的 line_height */
#define SCALE_MAX_X      307   /* 中央卡最大横向缩放：256×1.2 ≈ 307（向右加宽） */
#define SCALE_MAX_Y      281   /* 中央卡最大纵向缩放：256×1.1 ≈ 281（中心对称） */

/* ── 单实例状态（启动器全屏唯一） ── */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *drum;
    lv_obj_t *cards[APP_COUNT];
    lv_obj_t *text_labels[APP_COUNT];

    bool dark;

    /* 主题动画 */
    int32_t theme_val;
    lv_color_t theme_from;

    /* 几何（relayout 计算） */
    int card_w, card_h, gap, unit, visible_count;

    /* 吸附动画 */
    int32_t snap_val;

    /* 卡片当前缩放（变化检测，避免无谓重绘） */
    int32_t prev_sx[APP_COUNT];
    int32_t prev_sy[APP_COUNT];
} launcher_t;

static launcher_t s_launcher;

/* ── 几何计算 ── */

static void launcher_compute_geom(void)
{
    lv_display_t *disp = lv_display_get_default();
    int sh = lv_display_get_vertical_resolution(disp);

    s_launcher.card_w = lv_display_get_horizontal_resolution(disp) * CARD_W_RATIO / 100;

    /* 可见卡数：单数，默认 3；若 3 张满屏时卡高低于文字最小高度（3 行+留白）
     * 则降为 1 张。卡片+间距同比例缩放，恰好铺满整个屏幕高度：
     * 总高 = n×卡高 + (n+1)×间距 = 屏高（上下边距 = 间距，卡间 = 间距） */
    int base_h = CARD_MAX_LINES * FONT_LINE_H + 2 * CARD_H_PAD;  /* 64 */
    int base_gap = base_h / 4;                                   /* 16 */
    int min_h = CARD_MAX_LINES * FONT_LINE_H + 2 * 4;            /* 文字下限 */

    int n = 3;
    for (;;) {
        int denom = n * base_h + (n + 1) * base_gap;
        int card_h = (sh * base_h + denom / 2) / denom;          /* 四舍五入 */
        if (card_h >= min_h || n == 1) {
            s_launcher.visible_count = n;
            s_launcher.card_h = card_h;
            break;
        }
        n -= 2;
    }
    /* 剩余高度均分给所有间隙（卡间 n-1 + 上下 2 = n+1 个） */
    s_launcher.gap = (sh - s_launcher.visible_count * s_launcher.card_h)
                     / (s_launcher.visible_count + 1);
    if (s_launcher.gap < 2) s_launcher.gap = 2;
    s_launcher.unit = s_launcher.card_h + s_launcher.gap;
}

/* ── 中央卡放大：越靠近屏幕中心越大；左缘对齐、宽度向右扩展 1.2x、高度对称 1.1x ── */

static void launcher_update_cards(void)
{
    int scr_y = lv_obj_get_scroll_y(s_launcher.drum);
    int cy = lv_display_get_vertical_resolution(lv_display_get_default()) / 2;
    int unit = s_launcher.unit;

    for (int i = 0; i < APP_COUNT; i++) {
        int card_cy = s_launcher.gap + i * unit + s_launcher.card_h / 2 - scr_y;
        int dist = card_cy > cy ? card_cy - cy : cy - card_cy;
        int t = unit - dist;              /* 靠近中央 → t 大 */
        if (t < 0) t = 0;
        if (t > unit) t = unit;
        /* 量化 1/16 步进（≈每 6px 滚动变一级），减少 transform 重绘频率 */
        int tq = (t * 16 + unit / 2) / unit;
        int32_t sx = LV_SCALE_NONE + tq * (SCALE_MAX_X - LV_SCALE_NONE) / 16;
        int32_t sy = LV_SCALE_NONE + tq * (SCALE_MAX_Y - LV_SCALE_NONE) / 16;
        if (sx != s_launcher.prev_sx[i]) {
            s_launcher.prev_sx[i] = sx;
            lv_obj_set_style_transform_scale_x(s_launcher.cards[i], sx, 0);
        }
        if (sy != s_launcher.prev_sy[i]) {
            s_launcher.prev_sy[i] = sy;
            lv_obj_set_style_transform_scale_y(s_launcher.cards[i], sy, 0);
        }
    }
}

/* ── 吸附动画：把离屏幕中心最近的卡片滚到屏幕中心 ── */

static void launcher_snap_exec(void *var, int32_t v)
{
    *(int32_t *)var = v;
    lv_obj_scroll_to_y(s_launcher.drum, v, LV_ANIM_OFF);
}

static void launcher_snap_cancel(void)
{
    lv_anim_delete(&s_launcher.snap_val, launcher_snap_exec);
}

static void launcher_snap(void)
{
    int unit = s_launcher.unit;
    int cur = lv_obj_get_scroll_y(s_launcher.drum);
    /* 卡 i 居中时 scroll = (i-1)×unit，即目标 = round(cur/unit)×unit，
     * 取离屏幕中心最近的卡片（cur 相同时偏向下方卡片）。
     * max_scroll = 内容总高 - 视口高 = (N×unit + gap) - sh */
    int target = ((cur + unit / 2) / unit) * unit;
    int max_scroll = APP_COUNT * unit + s_launcher.gap
                     - lv_display_get_vertical_resolution(lv_display_get_default());
    if (max_scroll < 0) max_scroll = 0;
    if (target < 0) target = 0;
    if (target > max_scroll) target = max_scroll;
    if (cur == target) return;

    launcher_snap_cancel();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_launcher.snap_val);
    lv_anim_set_exec_cb(&a, launcher_snap_exec);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ── 滚筒事件 ── */

/* 扩展绘制区：返回卡片最大放大 1.2x 时的横向右扩量（其余方向同量级） */
static void on_drum_refr_ext(lv_event_t *e)
{
    int32_t *s = lv_event_get_param(e);
    int32_t ext = s_launcher.card_w * (SCALE_MAX_X - LV_SCALE_NONE) / LV_SCALE_NONE + 4;
    if (ext > *s) *s = ext;
}

static void on_drum_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_PRESSED:
        /* 用户重新按下：取消吸附动画，交还原生滚动 */
        launcher_snap_cancel();
        break;
    case LV_EVENT_SCROLL:
        /* 滚动（拖动/吸附动画）中实时更新中央放大效果 */
        launcher_update_cards();
        break;
    case LV_EVENT_RELEASED:
        /* 松手吸附：最近卡片滚到屏幕中心，两边卡片跟随 */
        launcher_snap();
        break;
    default:
        break;
    }
}

/* ── 卡片斜切边框（DRAW_MAIN 自定义绘制：4 直边 + 4 斜切角，无圆弧） ── */

static void on_card_draw_main(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    /* 绘制坐标空间 = 屏幕绝对坐标（lv_refr.c lv_obj_redraw），
     * 局部坐标必须加对象左上角偏移 */
    lv_area_t c;
    lv_obj_get_coords(card, &c);
    int ox = c.x1, oy = c.y1;
    int w = lv_obj_get_width(card);
    int h = lv_obj_get_height(card);
    lv_color_t col = lv_color_hex(ACCENT_COLOR);
    int bw = CARD_BORDER;
    int ch = CARD_CHAMFER;
    int bi = CARD_BIAS;

    /* 4 条直边 */
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col;
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = 0;
    lv_area_t a;
    a.x1 = ox + ch;       a.y1 = oy;            a.x2 = ox + w - ch - 1; a.y2 = oy + bw - 1;
    lv_draw_rect(layer, &rd, &a);                                        /* 顶边 */
    a.x1 = ox + ch;       a.y1 = oy + h - bw;   a.x2 = ox + w - ch - 1; a.y2 = oy + h - 1;
    lv_draw_rect(layer, &rd, &a);                                        /* 底边 */
    a.x1 = ox;            a.y1 = oy + ch;       a.x2 = ox + bw - 1;      a.y2 = oy + h - ch - 1;
    lv_draw_rect(layer, &rd, &a);                                        /* 左边 */
    a.x1 = ox + w - bw;   a.y1 = oy + ch;       a.x2 = ox + w - 1;       a.y2 = oy + h - ch - 1;
    lv_draw_rect(layer, &rd, &a);                                        /* 右边 */

    /* 4 个斜切角（每个角一个梯形 = 2 三角形，外轮廓为 45° 斜线） */
    lv_draw_triangle_dsc_t td;
    lv_draw_triangle_dsc_init(&td);
    td.color = col;
    td.opa = LV_OPA_COVER;

    /* 左上：外 (0,ch)-(ch,0)，内 (ch+bi,bi)-(bi,ch+bi) */
    td.p[0].x = ox;            td.p[0].y = oy + ch;
    td.p[1].x = ox + ch;       td.p[1].y = oy;
    td.p[2].x = ox + ch + bi;  td.p[2].y = oy + bi;
    lv_draw_triangle(layer, &td);
    td.p[0].x = ox;            td.p[0].y = oy + ch;
    td.p[1].x = ox + ch + bi;  td.p[1].y = oy + bi;
    td.p[2].x = ox + bi;       td.p[2].y = oy + ch + bi;
    lv_draw_triangle(layer, &td);

    /* 右上：外 (w-ch,0)-(w,ch)，内 (w-ch-bi,bi)-(w-bi,ch+bi) */
    td.p[0].x = ox + w - ch;      td.p[0].y = oy;
    td.p[1].x = ox + w;           td.p[1].y = oy + ch;
    td.p[2].x = ox + w - bi;      td.p[2].y = oy + ch + bi;
    lv_draw_triangle(layer, &td);
    td.p[0].x = ox + w - ch;      td.p[0].y = oy;
    td.p[1].x = ox + w - bi;      td.p[1].y = oy + ch + bi;
    td.p[2].x = ox + w - ch - bi; td.p[2].y = oy + bi;
    lv_draw_triangle(layer, &td);

    /* 左下：外 (0,h-ch)-(ch,h)，内 (bi,h-ch-bi)-(ch+bi,h-bi) */
    td.p[0].x = ox;            td.p[0].y = oy + h - ch;
    td.p[1].x = ox + ch;       td.p[1].y = oy + h;
    td.p[2].x = ox + ch + bi;  td.p[2].y = oy + h - bi;
    lv_draw_triangle(layer, &td);
    td.p[0].x = ox;            td.p[0].y = oy + h - ch;
    td.p[1].x = ox + ch + bi;  td.p[1].y = oy + h - bi;
    td.p[2].x = ox + bi;       td.p[2].y = oy + h - ch - bi;
    lv_draw_triangle(layer, &td);

    /* 右下：外 (w-ch,h)-(w,h-ch)，内 (w-ch-bi,h-bi)-(w-bi,h-ch-bi) */
    td.p[0].x = ox + w - ch;      td.p[0].y = oy + h;
    td.p[1].x = ox + w;           td.p[1].y = oy + h - ch;
    td.p[2].x = ox + w - bi;      td.p[2].y = oy + h - ch - bi;
    lv_draw_triangle(layer, &td);
    td.p[0].x = ox + w - ch;      td.p[0].y = oy + h;
    td.p[1].x = ox + w - bi;      td.p[1].y = oy + h - ch - bi;
    td.p[2].x = ox + w - ch - bi; td.p[2].y = oy + h - bi;
    lv_draw_triangle(layer, &td);
}

/* ── 主题 ── */

static void launcher_theme_exec(void *var, int32_t v)
{
    (void)var;
    bool dark = s_launcher.dark;
    lv_color_t to = dark ? lv_color_hex(THEME_DARK_BG) : lv_color_hex(THEME_LIGHT_BG);
    /* lv_color_mix(c1, c2, mix)：mix==0 → c2，mix==255 → c1，
     * 故传 (to, from) 使 v:0→255 从旧色渐变到目标色 */
    lv_obj_set_style_bg_color(s_launcher.root, lv_color_mix(to, s_launcher.theme_from, v), 0);
}

static void launcher_apply_text_theme(void)
{
    lv_color_t c = s_launcher.dark ? lv_color_hex(THEME_DARK_TEXT) : lv_color_hex(THEME_LIGHT_TEXT);
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_set_style_text_color(s_launcher.text_labels[i], c, 0);
    }
}

void launcher_set_theme(lv_obj_t *obj, bool dark)
{
    if (obj != s_launcher.root || dark == s_launcher.dark) return;
    s_launcher.dark = dark;
    s_launcher.theme_from = lv_obj_get_style_bg_color(s_launcher.root, 0);

    lv_anim_delete(&s_launcher.theme_val, launcher_theme_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_launcher.theme_val);
    lv_anim_set_exec_cb(&a, launcher_theme_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    launcher_apply_text_theme();
}

/* ── 构建 ── */

static void launcher_build_cards(void)
{
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(s_launcher.drum);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                           | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        /* 事件冒泡：按压卡片/文字时 PRESSED/RELEASED 必须传到 drum（吸附用）；
         * 保留 SCROLL_CHAIN：按住卡片拖动时滚动传递给滚筒 */
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        /* 斜切边框：默认边框/圆角全关，由 DRAW_MAIN 自定义绘制 */
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 0, 0);
        lv_obj_add_event_cb(card, on_card_draw_main, LV_EVENT_DRAW_MAIN, NULL);
        s_launcher.cards[i] = card;

        /* 纯文字内容（最多 3 行，LONG_WRAP + 定宽折行保护，水平居中） */
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_app_texts[i]);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        s_launcher.text_labels[i] = lbl;
    }
    launcher_apply_text_theme();
}

static void launcher_relayout_core(void)
{
    lv_display_t *disp = lv_display_get_default();
    int sw = lv_display_get_horizontal_resolution(disp);
    int sh = lv_display_get_vertical_resolution(disp);

    launcher_compute_geom();

    /* root 跟随逻辑分辨率（旋转后必须同步） */
    lv_obj_set_size(s_launcher.root, sw, sh);

    /* 滚筒容器：宽 = 卡宽，贴左边框（左侧仅留 CARD_LEFT_GAP，不居中）。
     * 视口 = 全屏高；内容总高（卡+间距）恰好 = 屏高 → 初始显示单数张整卡，
     * 卡 0 顶 = 间距、卡 n-1 底 = 屏高 - 间距，间距均分铺满全高 */
    int drum_x = CARD_LEFT_GAP;
    lv_obj_set_size(s_launcher.drum, s_launcher.card_w, sh);
    lv_obj_set_pos(s_launcher.drum, drum_x, 0);
    lv_obj_set_style_pad_top(s_launcher.drum, s_launcher.gap, 0);
    lv_obj_set_style_pad_bottom(s_launcher.drum, s_launcher.gap, 0);

    /* 卡片布局：y_i = i × unit，首卡顶 = 内容顶（pad_top 之下）。
     * 左边缘对齐（x=0），缩放锚点 = 左缘中心：
     * 放大时宽度只向右扩展（1.2x）、高度上下对称（1.1x），
     * 与上下卡间距对称保留（1.1x 上扩 4px < 间距 20px） */
    int label_w = s_launcher.card_w - 2 * CARD_W_PAD;
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_set_size(s_launcher.cards[i], s_launcher.card_w, s_launcher.card_h);
        lv_obj_set_pos(s_launcher.cards[i], 0, i * s_launcher.unit);
        lv_obj_set_style_transform_pivot_x(s_launcher.cards[i], 0, 0);
        lv_obj_set_style_transform_pivot_y(s_launcher.cards[i], s_launcher.card_h / 2, 0);
        /* 文字垂直居中于卡片 */
        lv_obj_set_width(s_launcher.text_labels[i], label_w);
        lv_obj_center(s_launcher.text_labels[i]);
    }

    /* 保持当前滚动位置（clamp 到新滚动域） */
    int cur = lv_obj_get_scroll_y(s_launcher.drum);
    lv_obj_scroll_to_y(s_launcher.drum, cur, LV_ANIM_OFF);

    /* 初始化中央放大状态 */
    for (int i = 0; i < APP_COUNT; i++) {
        s_launcher.prev_sx[i] = -1;
        s_launcher.prev_sy[i] = -1;
    }
    launcher_update_cards();
}

/* ── Public API ── */

lv_obj_t *launcher_create(lv_obj_t *parent)
{
    memset(&s_launcher, 0, sizeof(s_launcher));
    s_launcher.dark = true;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(THEME_DARK_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, lv_display_get_horizontal_resolution(lv_display_get_default()),
                    lv_display_get_vertical_resolution(lv_display_get_default()));
    s_launcher.root = root;

    /* 滚筒容器：原生滚动（跟手），无惯性/弹性（松手由吸附动画接管）；
     * OVERFLOW_VISIBLE：中央卡放大超出容器时不被裁剪 */
    lv_obj_t *drum = lv_obj_create(root);
    lv_obj_remove_flag(drum, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(drum, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_scroll_dir(drum, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(drum, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(drum, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drum, 0, 0);
    lv_obj_set_style_pad_all(drum, 0, 0);
    /* 注意：LVGL 9 事件 filter 是精确枚举值比较（非位掩码），
     * 多个事件必须分别注册，位或会导致 filter 等于某个枚举值而失效 */
    lv_obj_add_event_cb(drum, on_drum_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(drum, on_drum_event, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(drum, on_drum_event, LV_EVENT_RELEASED, NULL);
    /* 扩展绘制区：中央卡 transform 放大 1.2x 右扩 ~29px 超出滚筒，
     * 必须扩大 drum 的裁剪范围，否则放大部分被容器边缘切除 */
    lv_obj_add_event_cb(drum, on_drum_refr_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    s_launcher.drum = drum;

    launcher_build_cards();
    launcher_relayout_core();

    return root;
}

void launcher_relayout(lv_obj_t *obj)
{
    if (obj != s_launcher.root) return;
    launcher_relayout_core();
}
