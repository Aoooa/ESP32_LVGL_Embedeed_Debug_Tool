#include "launcher.h"
#include "file_browser.h"
#include "reader_app.h"
#include "net_console.h"
#include "terminal.h"
#include "card_reader.h"
#include "dap_link.h"
#include "wave_gen.h"
#include "scope_app.h"
#include "gesture.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "esp_log.h"

/* ── App 卡片表（每卡最多 3 行，每行 ≤15 字符防折行） ── */
#define APP_COUNT 8

/* 卡片类型：可启动 app / 占位 */
typedef enum { APP_TYPE_LAUNCH, APP_TYPE_PLACEHOLDER } app_type_t;

/* launcher 图标：静态位图（光晕已烘入，非实时渲染），见 launcher_icons.c */
extern const lv_image_dsc_t launcher_icon_files;
extern const lv_image_dsc_t launcher_icon_reader;
extern const lv_image_dsc_t launcher_icon_terminal;
extern const lv_image_dsc_t launcher_icon_serialip;
extern const lv_image_dsc_t launcher_icon_cardr;
extern const lv_image_dsc_t launcher_icon_dap;

static const lv_image_dsc_t *const s_app_icons[APP_COUNT] = {
    &launcher_icon_files,      /* Files */
    &launcher_icon_reader,     /* Reader */
    &launcher_icon_terminal,   /* Terminal */
    &launcher_icon_serialip,   /* SerialIP */
    &launcher_icon_cardr,      /* CardR */
    &launcher_icon_dap,        /* DAPLink */
    &launcher_icon_terminal,   /* WaveGen（复用图标，后续可换） */
    &launcher_icon_serialip,   /* Scope（复用图标，后续可换） */
};

static const struct {
    const char *name;       /* APP 名称（简约，居中大字显示） */
    app_type_t type;        /* 占位或可启动 */
    launch_app_id_t id;     /* type=LAUNCH 时的 app id */
} s_apps[APP_COUNT] = {
    { "SD",    APP_TYPE_LAUNCH,     LAUNCH_APP_FILES },
    { "Book",   APP_TYPE_LAUNCH,     LAUNCH_APP_READER },
    { "Terminal", APP_TYPE_LAUNCH,     LAUNCH_APP_UART },
    { "Uart2IP", APP_TYPE_LAUNCH,     LAUNCH_APP_NET },
    { "MSD",    APP_TYPE_LAUNCH,     LAUNCH_APP_CARDREADER },
    { "SWD",  APP_TYPE_LAUNCH,     LAUNCH_APP_DAPLINK },
    { "Wave",  APP_TYPE_LAUNCH,     LAUNCH_APP_WAVEGEN },
    { "Scope", APP_TYPE_LAUNCH,     LAUNCH_APP_SCOPE },
};

/* ── 主题色（赛博朋克：暗底 + 霓虹青边框 + 霓虹品红拨轮） ── */
#define ACCENT_COLOR     0x00F0FF   /* 霓虹青：卡片边框固定色 */
#define ACCENT_COLOR_HI  0x7DF9FF   /* 亮青（按下边框/内光晕/名称文字） */
#define WHEEL_COLOR      0xFF00E5   /* 霓虹品红：调速拨轮（轨道/填充/圆钮） */
#define THEME_DARK_BG    0x0A0A12
#define THEME_DARK_CARD  0x12121F   /* 卡片底色（不透明） */
#define THEME_DARK_TEXT  0xE8E8F0
#define THEME_LIGHT_BG   0xE8E8F0
#define THEME_LIGHT_CARD 0xFFFFFF
#define THEME_LIGHT_TEXT 0x1A1A2E

/* ── 卡片渲染模式开关 ──
 * 1 = 整卡预烘焙位图（圆角+边框+图标+文字一次性画进 PSRAM，滚动帧零矢量渲染）
 * 0 = 实时渲染（圆角/边框/图标/文字每帧重画；动态内容更灵活）
 * 出问题翻转此宏即回到实时渲染，无需改其他代码 */
#define LAUNCHER_CARDS_BAKED 1

/* ── 布局参数 ── */
#define CARD_LEFT_GAP    8    /* 滚筒左侧距屏幕左边框 */
#define CARD_MAX_LINES   3    /* 每卡最多行数（卡片高度按此计算） */
#define CARD_H_PAD       8    /* 卡片内上下留白 */
#define CARD_W_PAD       12   /* 卡片内左右留白 */
#define CARD_BORDER      4    /* 卡片边框厚度 */
#define CARD_RADIUS      18   /* 卡片圆角倒角半径（圆弧形） */
#define FONT_LINE_H      16   /* lv_font_montserrat_14 的 line_height */
#define WHEEL_DRUM_GAP   10   /* 卡片右缘与拨轮左  缘的间距 */

/* ── 调速拨轮参数（屏幕右侧垂直正中） ── */
#define WHEEL_W          14    /* 拨轮视觉总宽 */
#define WHEEL_RIGHT_GAP  8     /* 拨轮右缘距屏幕右缘 */
#define WHEEL_KNOB_D     12    /* 基准直径 */
#define WHEEL_KNOB_W     14    /* 滑块直径 = 直径×1.2 */
#define WHEEL_KNOB_GAP   3     /* 滑块与行程端点间隙（悬浮感） */
#define WHEEL_SHELL_W    18    /* 胶囊外框宽（= 滑块宽 + 两侧边框余量） */
#define WHEEL_SHELL_BORDER 2   /* 胶囊外框边框厚度 */
#define WHEEL_TOUCH_PAD  8     /* 触摸热区外扩 */
#define WHEEL_MAX_SPEED  380.0f    /* 圆钮推满时的滚筒速度 px/s */
#define WHEEL_DEAD_ZONE  0.1f  /* 中心死区（防误触） */
#define WHEEL_TICK_MS    20    /* 速度驱动定时器周期 */
#define WHEEL_RETURN_MS  160   /* 松手回中动画时长 */

/* ── 单实例状态（启动器全屏唯一） ── */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *drum;
    lv_obj_t *cards[APP_COUNT];   /* 预烘焙模式：透明容器；实时模式：圆角容器 */
#if LAUNCHER_CARDS_BAKED
    lv_obj_t *card_bgs[APP_COUNT];   /* 共享背景位图（烘焙后 set_src 刷新） */
#endif
    lv_obj_t *icon_imgs[APP_COUNT];   /* 静态霓虹图标位图（两种模式都在用） */
    lv_obj_t *text_labels[APP_COUNT]; /* APP 名称标签（两种模式都在用） */

    /* 调速拨轮 */
    lv_obj_t *wheel;             /* 触摸热区容器（覆盖整个拨轮区域） */
    lv_obj_t *knob_shell;        /* 胶囊外框（粉色边框，内部透明） */
    lv_obj_t *knob;              /* 滑块（胶囊形：矩形 + 上下半圆） */
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

    /* 拨轮几何：右下角（右缘贴右，底部留 8px） */
    int wheel_h = sh / 3;
    s_launcher.wheel_cx = lv_display_get_horizontal_resolution(disp) - WHEEL_RIGHT_GAP - WHEEL_W / 2;
    s_launcher.wheel_cy = sh - wheel_h / 2 - 8;
    s_launcher.wheel_travel = wheel_h / 2 - WHEEL_KNOB_W / 2 - WHEEL_KNOB_GAP;
    if (s_launcher.wheel_travel < 20) s_launcher.wheel_travel = 20;
    s_launcher.wheel_w = WHEEL_W + 2 * WHEEL_TOUCH_PAD;
    s_launcher.wheel_h = 2 * (WHEEL_TOUCH_PAD + s_launcher.wheel_travel + WHEEL_KNOB_GAP
                              + WHEEL_KNOB_W / 2);
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

#if LAUNCHER_CARDS_BAKED
/* ── 卡片背景预烘焙 ──
 * 圆角+边框+底色一次性画进 PSRAM 位图（RGB565 不透明），8 张卡共享同一张，
 * 滚动帧只贴这一张背景图 + 实时画图标/文字（小面积）——省掉每帧重绘 8 个
 * 圆角矩形的成本，又比整卡烘焙省内存（1 张 vs 8 张）且文字/图标仍可动态改。
 * 尺寸或主题变化时重新烘焙（旋转/切主题低频）。 */
static lv_image_dsc_t s_card_bg_dsc;
static void *s_card_bg_buf;
static int s_bg_w, s_bg_h;
static bool s_bg_dark;

static void launcher_bake_card_bg(void)
{
    int w = s_launcher.card_w, h = s_launcher.card_h;
    bool dark = s_launcher.dark;
    if (w <= 0 || h <= 0) return;
    if (s_card_bg_buf && s_bg_w == w && s_bg_h == h && s_bg_dark == dark) return;

    size_t bytes = (size_t)w * h * 2;
    void *buf = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE("launcher", "card bg alloc %dx%d failed", w, h);
        return;
    }

    lv_obj_t *cv = lv_canvas_create(lv_layer_sys());
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(cv, dark ? lv_color_hex(THEME_DARK_CARD) : lv_color_hex(THEME_LIGHT_CARD),
                      LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = CARD_RADIUS;
    dsc.border_width = CARD_BORDER;
    dsc.border_color = lv_color_hex(ACCENT_COLOR);
    dsc.bg_opa = LV_OPA_TRANSP;
    lv_area_t a = { 0, 0, w - 1, h - 1 };
    lv_draw_rect(&layer, &dsc, &a);
    lv_canvas_finish_layer(cv, &layer);
    lv_obj_delete(cv);

    if (s_card_bg_buf) heap_caps_free(s_card_bg_buf);
    s_card_bg_buf = buf;
    s_bg_w = w;
    s_bg_h = h;
    s_bg_dark = dark;
    memset(&s_card_bg_dsc, 0, sizeof(s_card_bg_dsc));
    s_card_bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_card_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_card_bg_dsc.header.w = w;
    s_card_bg_dsc.header.h = h;
    s_card_bg_dsc.header.stride = w * 2;
    s_card_bg_dsc.data_size = bytes;
    s_card_bg_dsc.data = buf;
}
#endif /* LAUNCHER_CARDS_BAKED */

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
#if !LAUNCHER_CARDS_BAKED
    lv_color_t bc = s_launcher.dark ? lv_color_hex(THEME_DARK_CARD) : lv_color_hex(THEME_LIGHT_CARD);
#endif
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_set_style_text_color(s_launcher.text_labels[i], c, 0);
#if !LAUNCHER_CARDS_BAKED
        lv_obj_set_style_bg_color(s_launcher.cards[i], bc, 0);
#endif
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

#if LAUNCHER_CARDS_BAKED
    launcher_bake_card_bg();   /* 卡片底色换主题 → 重新烘焙背景位图 */
#endif
    launcher_apply_text_theme();
}

/* ── 构建 ── */

static void launcher_build_wheel(void)
{
    lv_obj_t *root = s_launcher.root;

    /* 触摸热区容器：覆盖整个拨轮区域（含余量），透明可点击。
     * 滑块只是视觉子对象，事件全部绑定在容器上，
     * 避免小滑块难以命中。 */
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

    /* 胶囊外框：矩形 + 上下半圆，粉色边框，内部透明（滑槽轮廓）。
     * 高度 = 滑块 + 上下行程，在 relayout 中定位。纯视觉不接收事件 */
    lv_obj_t *shell = lv_obj_create(wheel);
    lv_obj_remove_flag(shell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(shell, lv_color_hex(WHEEL_COLOR), 0);
    lv_obj_set_style_border_width(shell, WHEEL_SHELL_BORDER, 0);
    lv_obj_set_style_radius(shell, WHEEL_SHELL_W / 2, 0);   /* 上下半圆 */
    lv_obj_set_size(shell, WHEEL_SHELL_W, WHEEL_KNOB_W);
    s_launcher.knob_shell = shell;

    /* 滑块：圆形（直径 = WHEEL_KNOB_W，圆角 = 半径），霓虹粉，纯视觉不接收事件 */
    lv_obj_t *knob = lv_obj_create(wheel);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(knob, lv_color_hex(WHEEL_COLOR), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_radius(knob, WHEEL_KNOB_W / 2, 0);   /* 圆形 */
    lv_obj_set_size(knob, WHEEL_KNOB_W, WHEEL_KNOB_W);
    s_launcher.knob = knob;

    /* 速度定时器：按住期间驱动滚筒（20ms/50Hz），空闲暂停省资源 */
    s_launcher.wheel_timer = lv_timer_create(launcher_wheel_tick, WHEEL_TICK_MS, NULL);
    lv_timer_pause(s_launcher.wheel_timer);
}

/* ── APP 管理：统一描述符表（app_manifest.h）+ 返回栈 + 事件路由 ── */
#include "app_manifest.h"

#define APP_STACK_MAX 4   /* 返回栈深度（来源 APP 保活，压栈切换） */

typedef struct {
    void *app;                    /* APP 实例（NULL=空槽） */
    const app_manifest_t *m;      /* 描述符 */
    launcher_result_cb_t result_cb;  /* 本 APP 关闭时通知启动方（可能 NULL） */
    void *result_ctx;
} app_slot_t;

static app_slot_t s_stack[APP_STACK_MAX];
static int s_depth;               /* 当前栈深度（0=桌面） */

static char *s_result_buf;        /* 当前 APP 写入的返回结果（strdup 拷贝，弹栈时转交并释放） */

/* 全部 APP 描述符表（索引 = launch_app_id_t） */
const app_manifest_t app_manifests[LAUNCH_APP_COUNT] = {
    [LAUNCH_APP_FILES] = {
        .id = LAUNCH_APP_FILES,
        .name = "Files",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))file_browser_create,
        .destroy = (void (*)(void *))file_browser_destroy,
        .back = (bool (*)(void *))file_browser_swipe_back,
        .rotate = NULL,
        .refresh = (void (*)(void *))file_browser_refresh,
        .debug_event = (void (*)(void *, int))file_browser_debug_event,
    },
    [LAUNCH_APP_READER] = {
        .id = LAUNCH_APP_READER,
        .name = "Reader",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))reader_app_create,
        .destroy = (void (*)(void *))reader_app_destroy,
        .back = (bool (*)(void *))reader_app_swipe_back,
        .rotate = NULL,
        .refresh = (void (*)(void *))reader_app_refresh,
        .debug_event = (void (*)(void *, int))reader_app_debug_event,
        .entered = (void (*)(void *))reader_app_entered,
    },
    [LAUNCH_APP_UART] = {
        .id = LAUNCH_APP_UART,
        .name = "Terminal",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))terminal_create,
        .destroy = (void (*)(void *))terminal_destroy,
        .back = (bool (*)(void *))terminal_swipe_back,
        .rotate = NULL,
        .refresh = NULL,
        .debug_event = (void (*)(void *, int))terminal_debug_event,
    },
    [LAUNCH_APP_NET] = {
        .id = LAUNCH_APP_NET,
        .name = "SerialIP",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))net_console_create,
        .destroy = (void (*)(void *))net_console_destroy,
        .back = (bool (*)(void *))net_console_swipe_back,
        .rotate = NULL,
        .refresh = NULL,
        .debug_event = (void (*)(void *, int))net_console_debug_event,
    },
    [LAUNCH_APP_CARDREADER] = {
        .id = LAUNCH_APP_CARDREADER,
        .name = "CardR",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))card_reader_create,
        .destroy = (void (*)(void *))card_reader_destroy,
        .back = (bool (*)(void *))card_reader_swipe_back,
        .rotate = NULL,
        .refresh = (void (*)(void *))card_reader_refresh,
        .debug_event = (void (*)(void *, int))card_reader_debug_event,
    },
    [LAUNCH_APP_DAPLINK] = {
        .id = LAUNCH_APP_DAPLINK,
        .name = "DAPLink",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))dap_link_create,
        .destroy = (void (*)(void *))dap_link_destroy,
        .back = (bool (*)(void *))dap_link_swipe_back,
        .rotate = NULL,
        .refresh = NULL,
        .debug_event = NULL,
    },
    [LAUNCH_APP_WAVEGEN] = {
        .id = LAUNCH_APP_WAVEGEN,
        .name = "WaveGen",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))wave_gen_create,
        .destroy = (void (*)(void *))wave_gen_destroy,
        .back = (bool (*)(void *))wave_gen_swipe_back,
        .rotate = (void (*)(void *, int))wave_gen_rotate,
        .refresh = NULL,
        .debug_event = (void (*)(void *, int))wave_gen_debug_event,
    },
    [LAUNCH_APP_SCOPE] = {
        .id = LAUNCH_APP_SCOPE,
        .name = "Scope",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))scope_create,
        .destroy = (void (*)(void *))scope_destroy,
        .back = (bool (*)(void *))scope_swipe_back,
        .rotate = (void (*)(void *, int))scope_rotate,
        .refresh = NULL,
        .debug_event = (void (*)(void *, int))scope_debug_event,
        .entered = (void (*)(void *))scope_entered,
    },
};

/* 当前栈顶 APP（无则 NULL） */
static app_slot_t *stack_top(void)
{
    return s_depth > 0 ? &s_stack[s_depth - 1] : NULL;
}

/* 启动 APP：压栈（来源 APP 保活，arg 为带参启动参数，可 NULL，由 APP 查询） */
static const char *s_launch_arg;   /* 最近一次 launch 的 arg（launch 内可查询） */

const char *launcher_app_get_arg(void)
{
    return s_launch_arg;
}

/* ── APP 转场动画：新 APP 从右滑入（200ms） ──
 * 官方 lv_anim：APP root x 从 +屏宽 → 0（右滑入）。
 * 不做背景渐黑遮罩（实测滑动中全屏遮罩动画加剧卡顿）。
 * 完成后通知栈顶 APP 启动业务（entered 回调，可 NULL）。 */
#define APP_ENTER_MS 200

static void launcher_enter_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void launcher_enter_done(lv_anim_t *a)
{
    (void)a;
    /* 进入动画完成 → 通知栈顶 APP 启动业务（entered 回调，可 NULL）。
     * 动画期间只渲染 UI；业务（采集/扫描等）延迟到这里执行。 */
    app_slot_t *top = stack_top();
    if (top && top->m && top->m->entered) {
        top->m->entered(top->app);
    }
}

static void launcher_play_enter_anim(lv_obj_t *app_root)
{
    lv_display_t *disp = lv_display_get_default();
    int sw = lv_display_get_horizontal_resolution(disp);

    /* APP 从右滑入 */
    lv_obj_set_x(app_root, sw);
    lv_anim_t a1;
    lv_anim_init(&a1);
    lv_anim_set_var(&a1, app_root);
    lv_anim_set_exec_cb(&a1, launcher_enter_x_cb);
    lv_anim_set_values(&a1, sw, 0);
    lv_anim_set_duration(&a1, APP_ENTER_MS);
    lv_anim_set_path_cb(&a1, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a1, launcher_enter_done);
    lv_anim_start(&a1);
}

void launcher_app_launch(launch_app_id_t id, const char *arg)
{
    launcher_app_launch_with_cb(id, arg, NULL, NULL);
}

void launcher_app_launch_with_cb(launch_app_id_t id, const char *arg,
                                 launcher_result_cb_t on_result, void *ctx)
{
    if (id < 0 || id >= LAUNCH_APP_COUNT) return;
    if (s_depth >= APP_STACK_MAX) {
        ESP_LOGW("launcher", "app stack full (%d), launch %d ignored", APP_STACK_MAX, id);
        return;
    }
    const app_manifest_t *m = &app_manifests[id];
    if (!m->launch) return;
    app_slot_t *slot = &s_stack[s_depth];
    /* 转场动画：launch 前记录 child 数。新 APP root = launch 后**第一个**新增的
     * 顶层对象（各 APP 先创建 root；个别 APP 如 DAPLink 的 dropdown 展开列表
     * 也挂 screen——LVGL 官方 lv_dropdown.c:703，必须取第一个而非最后一个） */
    lv_obj_t *scr = lv_screen_active();
    uint32_t old_child_cnt = lv_obj_get_child_count(scr);
    s_launch_arg = arg;   /* APP launch 内调用 launcher_app_get_arg() 获取 */
    slot->app = m->launch(scr, launcher_app_close, NULL);
    s_launch_arg = NULL;
    if (lv_obj_get_child_count(scr) > old_child_cnt) {
        lv_obj_t *app_root = lv_obj_get_child(scr, (int32_t)old_child_cnt);
        launcher_play_enter_anim(app_root);
    }
    slot->m = m;
    slot->result_cb = on_result;   /* 本 APP 关闭时回传结果给启动方 */
    slot->result_ctx = ctx;
    s_depth++;
    ESP_LOGI("launcher", "[APP] push %s (depth=%d%s%s)", m->name, s_depth,
             arg ? ", arg=direct-open" : "",
             on_result ? ", result-cb" : "");
}

/* 当前 APP 写入返回结果：strdup 拷贝保存（结果须跨本 APP 生命周期存活，
 * 弹栈时 launcher 转交启动方回调并释放拷贝）。result=NULL 清除。 */
void launcher_app_set_result(const char *result)
{
    if (s_result_buf) {
        free(s_result_buf);
        s_result_buf = NULL;
    }
    if (result) {
        s_result_buf = strdup(result);
        ESP_LOGI("launcher", "[RESULT] stored (len=%d)", (int)strlen(result));
    }
}

/* 关闭栈顶 APP：弹栈回来源。签名兼容 back 回调（ctx 忽略）。
 * 顺序：destroy → 弹栈 → 结果回传（此时栈已正确弹出，回调内再 launch
 * 压入新槽安全）。结果由 launcher 拷贝保存，回调内同步使用，返回后释放。 */
void launcher_app_close(void *ctx)
{
    (void)ctx;
    app_slot_t *top = stack_top();
    if (!top) return;
    if (top->app && top->m && top->m->destroy) {
        top->m->destroy(top->app);
    }
    ESP_LOGI("launcher", "[APP] pop %s (depth=%d->%d)", top->m ? top->m->name : "?",
             s_depth, s_depth - 1);
    s_depth--;
    if (top->result_cb) {
        top->result_cb(top->result_ctx, s_result_buf);
        ESP_LOGI("launcher", "[RESULT] delivered to caller%s",
                 s_result_buf ? "" : " (null)");
    }
    top->result_cb = NULL;
    top->result_ctx = NULL;
    if (s_result_buf) {
        free(s_result_buf);
        s_result_buf = NULL;
    }
}

bool launcher_app_running(void)
{
    return s_depth > 0;
}

/* 返回事件（输入层右滑触发，ctx 未用）：
 * 栈顶 back 回调——true=弹栈回来源；false=APP 内部已处理。LVGL 线程直调。 */
void launcher_app_swipe_back(void *ctx)
{
    (void)ctx;
    app_slot_t *top = stack_top();
    if (!top || !top->m || !top->m->back) return;
    if (!top->m->back(top->app)) {
        ESP_LOGI("launcher", "[EVT] back: %s handled internally", top->m->name);
        return;
    }
    ESP_LOGI("launcher", "[EVT] back: %s close -> pop", top->m->name);
    launcher_app_close(NULL);
}

/* 手势事件（输入层全局右滑/左滑触发，launcher_app_swipe_gesture 入口）：
 * 栈顶 gesture 回调——true=APP 已处理；false/无回调 = 未处理 → 桌面兜底
 * （当前无实现，日志忽略）。LVGL 线程直调。 */
void launcher_app_swipe_gesture(app_gesture_t evt)
{
    app_slot_t *top = stack_top();
    if (top && top->m && top->m->gesture) {
        if (top->m->gesture(top->app, evt)) {
            ESP_LOGI("launcher", "[EVT] gesture(%d) -> %s handled", evt, top->m->name);
            return;
        }
    }
    ESP_LOGI("launcher", "[EVT] gesture(%d) unhandled -> desktop, ignored", evt);
}

/* 输入层右/左滑 → 统一手势路由（ctx 未用，事件类型由注册区分） */
static void launcher_on_swipe_right(void *ctx)
{
    (void)ctx;
    launcher_app_swipe_gesture(APP_GESTURE_SWIPE_RIGHT);
}

static void launcher_on_swipe_left(void *ctx)
{
    (void)ctx;
    launcher_app_swipe_gesture(APP_GESTURE_SWIPE_LEFT);
}

/* 旋转事件（平台旋转完成）：栈顶 rotate 回调，NULL=弹栈关闭回桌面 */
void launcher_event_rotate(int deg)
{
    app_slot_t *top = stack_top();
    if (!top) {
        launcher_relayout(s_launcher.root);
        return;
    }
    if (top->m && top->m->rotate) {
        ESP_LOGI("launcher", "[EVT] rotate %d° -> %s", deg, top->m->name);
        top->m->rotate(top->app, deg);
        return;
    }
    ESP_LOGI("launcher", "[EVT] rotate %d° -> %s has no handler, close", deg,
             top->m ? top->m->name : "?");
    launcher_app_close(NULL);
    launcher_relayout(s_launcher.root);
}

/* SD 就绪事件：栈顶 refresh 回调 */
void launcher_event_sd_ready(void)
{
    app_slot_t *top = stack_top();
    if (!top || !top->m || !top->m->refresh) return;
    ESP_LOGI("launcher", "[EVT] sd_ready -> %s", top->m->name);
    top->m->refresh(top->app);
}

/* 调试事件（测试模块调用）：栈顶 debug_event 回调 */
void launcher_event_debug(int evt)
{
    app_slot_t *top = stack_top();
    if (!top || !top->m || !top->m->debug_event) return;
    ESP_LOGI("launcher", "[EVT] debug(%d) -> %s", evt, top->m->name);
    top->m->debug_event(top->app, evt);
}

/* 卡片点击：可启动卡 → 启动 APP；占位卡无操作 */
static void on_card_event(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target_obj(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(card);
    if (i < 0 || i >= APP_COUNT) return;
    if (s_apps[i].type == APP_TYPE_LAUNCH) {
        launcher_app_launch(s_apps[i].id, NULL);
    }
}

static void launcher_build_cards(void)
{
    for (int i = 0; i < APP_COUNT; i++) {
#if LAUNCHER_CARDS_BAKED
        /* 卡片 = 透明容器 + 共享背景位图（圆角+边框，见 launcher_bake_card_bg）
         * + 实时图标/文字。滚动帧只贴 1 张 RGB565 背景图（快速拷贝），
         * 图标/文字小面积实时画 */
        lv_obj_t *card = lv_obj_create(s_launcher.drum);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                           | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
#else
        /* 实时渲染模式（LAUNCHER_CARDS_BAKED=0）：圆角容器 + 图标 + 文字每帧重画 */
        lv_obj_t *card = lv_obj_create(s_launcher.drum);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                           | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(THEME_DARK_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(ACCENT_COLOR), 0);
        lv_obj_set_style_border_width(card, CARD_BORDER, 0);
        lv_obj_set_style_radius(card, CARD_RADIUS, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(ACCENT_COLOR_HI), LV_STATE_PRESSED);
#endif
        /* 按下效果：整体轻微缩小（约 97.7%，长宽各缩约 3px）。transform_scale
         * 256=100%；勿用 transform_width/height（只改绘制边界，不缩内容） */
        lv_obj_set_style_transform_pivot_x(card, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(card, lv_pct(50), 0);
        lv_obj_set_style_transform_scale(card, 250, LV_STATE_PRESSED);
        /* 卡片点击 → 启动对应 APP（占位卡无操作） */
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, on_card_event, LV_EVENT_CLICKED, NULL);
        s_launcher.cards[i] = card;

#if LAUNCHER_CARDS_BAKED
        /* 共享背景位图（首个子对象，位于图标/文字之下） */
        lv_obj_t *bg = lv_image_create(card);
        lv_obj_add_flag(bg, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_pos(bg, 0, 0);
        s_launcher.card_bgs[i] = bg;
#endif

        /* 卡片内容：静态霓虹图标位图 + APP 名称（relayout 固定定位：图标左对齐竖线） */
        lv_obj_t *icon = lv_image_create(card);
        lv_image_set_src(icon, s_app_icons[i]);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
        s_launcher.icon_imgs[i] = icon;

        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_apps[i].name);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(ACCENT_COLOR_HI), LV_STATE_PRESSED);
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

    /* 卡片布局：y_i = i × unit，首卡顶 = 内容顶（pad_top 之下），左缘对齐。
     * 内容：霓虹图标贴左（x=8，所有卡片同一竖线），名称挨着图标右侧 */
#if LAUNCHER_CARDS_BAKED
    launcher_bake_card_bg();   /* 尺寸/主题变化时重烘焙共享背景位图 */
#endif
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_set_size(s_launcher.cards[i], s_launcher.card_w, s_launcher.card_h);
        lv_obj_set_pos(s_launcher.cards[i], 0, i * s_launcher.unit);
#if LAUNCHER_CARDS_BAKED
        lv_image_set_src(s_launcher.card_bgs[i], &s_card_bg_dsc);   /* 烘焙后刷新引用 */
#endif
        lv_obj_align(s_launcher.icon_imgs[i], LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_align(s_launcher.text_labels[i], LV_ALIGN_LEFT_MID, 58, 0);
    }

    /* 保持当前滚动位置（clamp 到新滚动域） */
    int cur = lv_obj_get_scroll_y(s_launcher.drum);
    lv_obj_scroll_to_y(s_launcher.drum, cur, LV_ANIM_OFF);

    /* 拨轮定位（右缘垂直正中） */
    int wx = s_launcher.wheel_cx - s_launcher.wheel_w / 2;
    int wy = s_launcher.wheel_cy - s_launcher.wheel_h / 2;
    lv_obj_set_pos(s_launcher.wheel, wx, wy);
    lv_obj_set_size(s_launcher.wheel, s_launcher.wheel_w, s_launcher.wheel_h);
    /* 胶囊外框：居中，高度覆盖滑块上下行程（上下端贴合半圆） */
    lv_obj_set_pos(s_launcher.knob_shell,
                   (s_launcher.wheel_w - WHEEL_SHELL_W) / 2,
                   WHEEL_TOUCH_PAD + WHEEL_KNOB_GAP);
    lv_obj_set_size(s_launcher.knob_shell, WHEEL_SHELL_W,
                    WHEEL_KNOB_W + 2 * s_launcher.wheel_travel);
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

    /* 滚筒容器：原生滚动（跟手 1:1 + 松手惯性滑行 + 边界弹性回弹）。
     * 保留 LV_OBJ_FLAG_SCROLL_ELASTIC：按住拖过边界减速弹性跟手（diff/4），
     * 松手惯性撞边后由 LVGL 回弹动画拉回（用户要求） */
    lv_obj_t *drum = lv_obj_create(root);
    lv_obj_set_scroll_dir(drum, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(drum, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(drum, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drum, 0, 0);
    lv_obj_set_style_pad_all(drum, 0, 0);
    s_launcher.drum = drum;

    launcher_build_cards();
    launcher_build_wheel();
    launcher_relayout_core();

    /* 增强触摸滚动惯性：
     * - lv_indev.c 速度采样窗口 99ms → 150ms（补偿 50ms 防抖释放确认窗口
     *   对速度矢量的稀释，见 lv_indev.c indev_scroll_throw_decay 注释）
     * - scroll_throw = 3（速度每帧衰减 3%，滑行距离 ≈33×甩动速度）
     *   手感调节：改小 → 惯性更长更拖尾；改大 → 更短更利落
     * 注：scroll_throw 是 indev 级全局参数，所有可滚动对象（桌面/文件列表/
     * 阅读器等）共用，此处对触摸 indev 统一设置 */
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_set_scroll_throw(indev, 3);
        }
    }

    /* 注册输入层事件：贴边右滑=返回；全局右滑/左滑=手势事件路由 */
    gesture_set_back_handler(launcher_app_swipe_back, NULL);
    gesture_set_right_handler(launcher_on_swipe_right, NULL);
    gesture_set_left_handler(launcher_on_swipe_left, NULL);

    return root;
}

void launcher_relayout(lv_obj_t *obj)
{
    if (obj != s_launcher.root) return;
    launcher_relayout_core();
}
