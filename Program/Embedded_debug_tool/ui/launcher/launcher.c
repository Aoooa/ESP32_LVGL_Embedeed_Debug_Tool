#include "launcher.h"
#include <string.h>
#include <math.h>

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
#define THEME_DARK_CARD  0x161616   /* 卡片底色（不透明） */
#define THEME_DARK_TEXT  0xFFFFFF
#define THEME_LIGHT_BG   0xF5F5F5
#define THEME_LIGHT_CARD 0xFFFFFF
#define THEME_LIGHT_TEXT 0x374151

/* ── 布局参数 ── */
#define CARD_LEFT_GAP    8    /* 滚筒左侧距屏幕左边框 */
#define CARD_MAX_LINES   3    /* 每卡最多行数（卡片高度按此计算） */
#define CARD_H_PAD       8    /* 卡片内上下留白 */
#define CARD_W_PAD       12   /* 卡片内左右留白 */
#define CARD_BORDER      4    /* 卡片边框厚度 */
#define CARD_RADIUS      18   /* 卡片圆角倒角半径（圆弧形） */
#define FONT_LINE_H      16   /* lv_font_montserrat_14 的 line_height */
#define WHEEL_DRUM_GAP   10   /* 卡片右缘与拨轮左缘的间距 */

/* ── 调速拨轮参数（屏幕右侧垂直正中） ── */
#define WHEEL_W          14    /* 拨轮视觉总宽 */
#define WHEEL_RIGHT_GAP  8     /* 拨轮右缘距屏幕右缘 */
#define WHEEL_TRACK_W    4     /* 轨道宽 */
#define WHEEL_TRACK_OPA  LV_OPA_40   /* 轨道半透明（40%） */
#define WHEEL_KNOB_D     12    /* 基准直径 */
#define WHEEL_KNOB_W     14    /* 矩形钮宽 = 直径×1.2 */
#define WHEEL_KNOB_H     18    /* 矩形钮长 = 直径×1.5 */
#define WHEEL_KNOB_RAD   4     /* 矩形钮四角圆倒角 */
#define WHEEL_KNOB_GAP   3     /* 钮与轨道间隙（悬浮感） */
#define WHEEL_TOUCH_PAD  8     /* 触摸热区外扩 */
#define WHEEL_MAX_SPEED  380.0f    /* 圆钮推满时的滚筒速度 px/s */
#define WHEEL_DEAD_ZONE  0.1f  /* 中心死区（防误触） */
#define WHEEL_TICK_MS    20    /* 速度驱动定时器周期 */
#define WHEEL_RETURN_MS  160   /* 松手回中动画时长 */

/* ── 单实例状态（启动器全屏唯一） ── */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *drum;
    lv_obj_t *cards[APP_COUNT];
    lv_obj_t *text_labels[APP_COUNT];

    /* 调速拨轮 */
    lv_obj_t *wheel;             /* 触摸热区容器（覆盖整个拨轮区域） */
    lv_obj_t *track_up;          /* 上轨道（半透明） */
    lv_obj_t *track_dn;          /* 下轨道（半透明） */
    lv_obj_t *fill_up;           /* 上轨道填充（水银柱，从下往上） */
    lv_obj_t *fill_dn;           /* 下轨道填充（水银柱，从上往下） */
    lv_obj_t *knob;              /* 圆钮（不透明，纯视觉） */
    lv_timer_t *wheel_timer;     /* 按住期间驱动滚筒速度（空闲暂停） */
    bool wheel_pressed;
    uint32_t wheel_last_tick;
    float knob_pos;              /* -1..1（圆钮偏离中心，负=上推） */
    int32_t knob_anim_val;       /* 松手回中动画变量 */

    bool dark;

    /* 主题动画 */
    int32_t theme_val;
    lv_color_t theme_from;

    /* 几何（relayout 计算） */
    int card_w, card_h, gap, unit, visible_count;
    int wheel_cx, wheel_cy, wheel_travel, wheel_w, wheel_h;   /* 拨轮几何 */
} launcher_t;

static launcher_t s_launcher;

/* ── 几何计算 ── */

static void launcher_compute_geom(void)
{
    lv_display_t *disp = lv_display_get_default();
    int sw = lv_display_get_horizontal_resolution(disp);
    int sh = lv_display_get_vertical_resolution(disp);

    /* 卡片宽：贴左，右缘贴近拨轮（留 WHEEL_DRUM_GAP 间距） */
    s_launcher.card_w = sw - CARD_LEFT_GAP - WHEEL_DRUM_GAP - WHEEL_W - WHEEL_RIGHT_GAP;

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

    /* 拨轮几何：右缘垂直正中，高 = 屏高 1/3 */
    int wheel_h = sh / 3;
    s_launcher.wheel_cx = lv_display_get_horizontal_resolution(disp) - WHEEL_RIGHT_GAP - WHEEL_W / 2;
    s_launcher.wheel_cy = sh / 2;
    s_launcher.wheel_travel = wheel_h / 2 - WHEEL_KNOB_H / 2 - WHEEL_KNOB_GAP;
    if (s_launcher.wheel_travel < 20) s_launcher.wheel_travel = 20;
    s_launcher.wheel_w = WHEEL_W + 2 * WHEEL_TOUCH_PAD;
    s_launcher.wheel_h = 2 * (WHEEL_TOUCH_PAD + s_launcher.wheel_travel + WHEEL_KNOB_GAP
                              + WHEEL_KNOB_H / 2);
}

/* ── 调速拨轮 ──
 * 按住圆钮上下拖动：偏离中心越远滚筒速度越快（控制速度，不控制位置）；
 * 被推方向轨道如水银柱被实心绿填充；松手圆钮弹回中心、填充收缩、滚筒停止。 */

static void launcher_wheel_apply(void)
{
    float pos = s_launcher.knob_pos;
    int knob_y = WHEEL_TOUCH_PAD + s_launcher.wheel_travel + WHEEL_KNOB_GAP
                 + (int)(pos * s_launcher.wheel_travel);
    lv_obj_set_y(s_launcher.knob, knob_y);

    /* 往上推(pos<0) → 上轨道从下往上填；往下拉(pos>0) → 下轨道从上往下填 */
    int fu = (int)(s_launcher.wheel_travel * (pos < 0 ? -pos : 0));
    lv_obj_set_size(s_launcher.fill_up, WHEEL_TRACK_W, fu);
    lv_obj_set_y(s_launcher.fill_up, s_launcher.wheel_travel - fu);
    int fd = (int)(s_launcher.wheel_travel * (pos > 0 ? pos : 0));
    lv_obj_set_size(s_launcher.fill_dn, WHEEL_TRACK_W, fd);
    lv_obj_set_y(s_launcher.fill_dn, 0);
}

static void launcher_knob_anim_exec(void *var, int32_t v)
{
    (void)var;
    s_launcher.knob_pos = v / 1000.0f;
    launcher_wheel_apply();
}

static void launcher_knob_anim_done(lv_anim_t *a)
{
    (void)a;
    lv_timer_pause(s_launcher.wheel_timer);
}

static void launcher_wheel_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_launcher.wheel_pressed) return;

    uint32_t now = lv_tick_get();
    float dt = (now - s_launcher.wheel_last_tick) / 1000.0f;
    if (dt < 0 || dt > 0.1f) dt = WHEEL_TICK_MS / 1000.0f;
    s_launcher.wheel_last_tick = now;

    float pos = s_launcher.knob_pos;
    if (fabsf(pos) < WHEEL_DEAD_ZONE) pos = 0;
    /* 符号：往上推(pos<0) → 内容上移(scroll_y 增大)；往下拉 → 内容下移 */
    int dy = -(int)(pos * WHEEL_MAX_SPEED * dt);
    if (dy != 0) lv_obj_scroll_by_bounded(s_launcher.drum, 0, dy, LV_ANIM_OFF);
}

static void on_wheel_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_PRESSED: {
        s_launcher.wheel_pressed = true;
        lv_anim_delete(&s_launcher.knob_anim_val, launcher_knob_anim_exec);
        s_launcher.wheel_last_tick = lv_tick_get();
        lv_timer_resume(s_launcher.wheel_timer);
        break;
    }
    case LV_EVENT_PRESSING: {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        float pos = (float)(p.y - s_launcher.wheel_cy) / s_launcher.wheel_travel;
        if (pos > 1.0f) pos = 1.0f;
        if (pos < -1.0f) pos = -1.0f;
        s_launcher.knob_pos = pos;
        launcher_wheel_apply();
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_INDEV_RESET: {
        s_launcher.wheel_pressed = false;   /* 速度归零 → 滚筒立即停止 */
        /* 圆钮橡皮筋回中（填充同步收缩） */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &s_launcher.knob_anim_val);
        lv_anim_set_exec_cb(&a, launcher_knob_anim_exec);
        lv_anim_set_values(&a, (int32_t)(s_launcher.knob_pos * 1000.0f), 0);
        lv_anim_set_duration(&a, WHEEL_RETURN_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, launcher_knob_anim_done);
        lv_anim_start(&a);
        break;
    }
    default:
        break;
    }
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
    lv_color_t bc = s_launcher.dark ? lv_color_hex(THEME_DARK_CARD) : lv_color_hex(THEME_LIGHT_CARD);
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_set_style_text_color(s_launcher.text_labels[i], c, 0);
        lv_obj_set_style_bg_color(s_launcher.cards[i], bc, 0);
        lv_obj_set_style_bg_color(s_launcher.text_labels[i], bc, 0);
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

static void launcher_build_wheel(void)
{
    lv_obj_t *root = s_launcher.root;

    /* 触摸热区容器：覆盖整个拨轮区域（含余量），透明可点击。
     * 轨道/圆钮只是视觉子对象，事件全部绑定在容器上，
     * 避免 12px 小圆钮难以命中。 */
    lv_obj_t *wheel = lv_obj_create(root);
    lv_obj_remove_flag(wheel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_opa(wheel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wheel, 0, 0);
    lv_obj_set_style_pad_all(wheel, 0, 0);
    /* 注意：LVGL 9 事件 filter 精确比较，必须逐个注册 */
    lv_obj_add_event_cb(wheel, on_wheel_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(wheel, on_wheel_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(wheel, on_wheel_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(wheel, on_wheel_event, LV_EVENT_INDEV_RESET, NULL);
    s_launcher.wheel = wheel;

    /* 上轨道（半透明绿，半圆收口） */
    lv_obj_t *up = lv_obj_create(wheel);
    lv_obj_remove_flag(up, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(up, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_set_style_bg_opa(up, WHEEL_TRACK_OPA, 0);
    lv_obj_set_style_border_width(up, 0, 0);
    lv_obj_set_style_radius(up, LV_RADIUS_CIRCLE, 0);
    s_launcher.track_up = up;
    /* 上轨道填充（水银柱：实心绿，从下往上） */
    lv_obj_t *upf = lv_obj_create(up);
    lv_obj_remove_flag(upf, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(upf, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_set_style_bg_opa(upf, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(upf, 0, 0);
    lv_obj_set_style_radius(upf, LV_RADIUS_CIRCLE, 0);
    s_launcher.fill_up = upf;

    /* 下轨道（对称） */
    lv_obj_t *dn = lv_obj_create(wheel);
    lv_obj_remove_flag(dn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(dn, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_set_style_bg_opa(dn, WHEEL_TRACK_OPA, 0);
    lv_obj_set_style_border_width(dn, 0, 0);
    lv_obj_set_style_radius(dn, LV_RADIUS_CIRCLE, 0);
    s_launcher.track_dn = dn;
    lv_obj_t *dnf = lv_obj_create(dn);
    lv_obj_remove_flag(dnf, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(dnf, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_set_style_bg_opa(dnf, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dnf, 0, 0);
    lv_obj_set_style_radius(dnf, LV_RADIUS_CIRCLE, 0);
    s_launcher.fill_dn = dnf;

    /* 矩形钮（不透明，纯视觉，不接收事件） */
    lv_obj_t *knob = lv_obj_create(wheel);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(knob, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_radius(knob, WHEEL_KNOB_RAD, 0);
    lv_obj_set_size(knob, WHEEL_KNOB_W, WHEEL_KNOB_H);
    s_launcher.knob = knob;

    /* 速度定时器：按住期间驱动滚筒（20ms/50Hz），空闲暂停省资源 */
    s_launcher.wheel_timer = lv_timer_create(launcher_wheel_tick, WHEEL_TICK_MS, NULL);
    lv_timer_pause(s_launcher.wheel_timer);
}

static void launcher_build_cards(void)
{
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(s_launcher.drum);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                           | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        /* 事件冒泡：按压卡片/文字时 PRESSED/RELEASED 必须传到 drum（吸附用）；
         * 保留 SCROLL_CHAIN：按住卡片拖动时滚动传递给滚筒 */
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
        /* 卡片底色不透明（主题色）+ 圆角倒角（圆弧形）+ 初音绿边框 */
        lv_obj_set_style_bg_color(card, lv_color_hex(THEME_DARK_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(ACCENT_COLOR), 0);
        lv_obj_set_style_border_width(card, CARD_BORDER, 0);
        lv_obj_set_style_radius(card, CARD_RADIUS, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        s_launcher.cards[i] = card;

        /* 纯文字内容（最多 3 行，LONG_WRAP + 定宽折行保护，水平居中） */
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_app_texts[i]);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(lbl, lv_color_hex(THEME_DARK_CARD), 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
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

    /* 卡片布局：y_i = i × unit，首卡顶 = 内容顶（pad_top 之下），左缘对齐 */
    int label_w = s_launcher.card_w - 2 * CARD_W_PAD;
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_set_size(s_launcher.cards[i], s_launcher.card_w, s_launcher.card_h);
        lv_obj_set_pos(s_launcher.cards[i], 0, i * s_launcher.unit);
        /* 文字垂直居中于卡片 */
        lv_obj_set_width(s_launcher.text_labels[i], label_w);
        lv_obj_center(s_launcher.text_labels[i]);
    }

    /* 保持当前滚动位置（clamp 到新滚动域） */
    int cur = lv_obj_get_scroll_y(s_launcher.drum);
    lv_obj_scroll_to_y(s_launcher.drum, cur, LV_ANIM_OFF);

    /* 拨轮定位（右缘垂直正中） */
    int wx = s_launcher.wheel_cx - s_launcher.wheel_w / 2;
    int wy = s_launcher.wheel_cy - s_launcher.wheel_h / 2;
    lv_obj_set_pos(s_launcher.wheel, wx, wy);
    lv_obj_set_size(s_launcher.wheel, s_launcher.wheel_w, s_launcher.wheel_h);
    int track_x = (s_launcher.wheel_w - WHEEL_TRACK_W) / 2;
    lv_obj_set_pos(s_launcher.track_up, track_x, WHEEL_TOUCH_PAD);
    lv_obj_set_size(s_launcher.track_up, WHEEL_TRACK_W, s_launcher.wheel_travel);
    lv_obj_set_pos(s_launcher.track_dn, track_x,
                   WHEEL_TOUCH_PAD + s_launcher.wheel_travel + WHEEL_KNOB_H + 2 * WHEEL_KNOB_GAP);
    lv_obj_set_size(s_launcher.track_dn, WHEEL_TRACK_W, s_launcher.wheel_travel);
    lv_obj_set_pos(s_launcher.knob, (s_launcher.wheel_w - WHEEL_KNOB_W) / 2, 0);
    launcher_wheel_apply();
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

    /* 滚筒容器：原生滚动（跟手 1:1 + 松手惯性滑行），无弹性边界 */
    lv_obj_t *drum = lv_obj_create(root);
    lv_obj_remove_flag(drum, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(drum, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(drum, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(drum, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drum, 0, 0);
    lv_obj_set_style_pad_all(drum, 0, 0);
    s_launcher.drum = drum;

    launcher_build_cards();
    launcher_build_wheel();
    launcher_relayout_core();

    return root;
}

void launcher_relayout(lv_obj_t *obj)
{
    if (obj != s_launcher.root) return;
    launcher_relayout_core();
}
