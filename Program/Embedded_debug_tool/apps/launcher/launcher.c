#include "launcher.h"
#include "file_browser.h"
#include "reader_app.h"
#include "net_console.h"
#include "terminal.h"
#include "card_reader.h"
#include "gesture.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "esp_log.h"

/* launcher 图标字体：只含 montserrat_28 缺失的 FontAwesome 字形，缺失字形回退 montserrat_28 */
LV_FONT_DECLARE(lv_font_launcher_icons);

/* ── App 卡片表（每卡最多 3 行，每行 ≤15 字符防折行） ── */
#define APP_COUNT 6

/* 卡片类型：可启动 app / 占位 */
typedef enum { APP_TYPE_LAUNCH, APP_TYPE_PLACEHOLDER } app_type_t;

/* 图标字体新增字形（FontAwesome，montserrat_28 缺失，由 lv_font_launcher_icons 提供） */
#define LAUNCHER_ICON_BOOK     "\xEF\x80\xAD"   /* U+F02D fa-book（阅读器） */
#define LAUNCHER_ICON_TERMINAL "\xEF\x84\xA0"   /* U+F120 fa-terminal（串口终端） */
#define LAUNCHER_ICON_SDCARD   "\xEF\x9F\x82"   /* U+F7C2 fa-sd-card（USB 读卡器） */

/* APP 图标（LV_SYMBOL / FontAwesome，lv_font_launcher_icons + fallback montserrat_28） */
static const char *const s_app_icons[APP_COUNT] = {
    LV_SYMBOL_DIRECTORY,   /* Files */
    LAUNCHER_ICON_BOOK,    /* Reader（书本） */
    LAUNCHER_ICON_TERMINAL, /* Terminal（串口终端） */
    LV_SYMBOL_WIFI,        /* SerialIP（串口转 TCP/IP） */
    LAUNCHER_ICON_SDCARD,  /* SD 读卡器 */
    LV_SYMBOL_POWER,       /* Slot 6 */
};

static const struct {
    const char *name;       /* APP 名称（简约，居中大字显示） */
    app_type_t type;        /* 占位或可启动 */
    launch_app_id_t id;     /* type=LAUNCH 时的 app id */
} s_apps[APP_COUNT] = {
    { "Files",    APP_TYPE_LAUNCH,     LAUNCH_APP_FILES },
    { "Reader",   APP_TYPE_LAUNCH,     LAUNCH_APP_READER },
    { "Terminal", APP_TYPE_LAUNCH,     LAUNCH_APP_UART },
    { "SerialIP", APP_TYPE_LAUNCH,     LAUNCH_APP_NET },
    { "CardR",    APP_TYPE_LAUNCH,     LAUNCH_APP_CARDREADER },
    { "Slot 6",   APP_TYPE_PLACEHOLDER, 0 },
};

/* ── 主题色 ── */
#define ACCENT_COLOR     0x39C5BB   /* 初音绿：卡片边框固定色 */
#define ACCENT_COLOR_HI  0x6FE3D8   /* 初音绿加亮（按下边框/内光晕） */
#define WHEEL_COLOR      0xFF3E9E   /* 霓虹粉：调速拨轮（轨道/填充/圆钮） */
#define THEME_DARK_BG    0x080A0C
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
    lv_obj_t *cards[APP_COUNT];
    lv_obj_t *icon_labels[APP_COUNT];   /* 白色图标（LV_SYMBOL，固定白色） */
    lv_obj_t *text_labels[APP_COUNT];

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
} app_slot_t;

static app_slot_t s_stack[APP_STACK_MAX];
static int s_depth;               /* 当前栈深度（0=桌面） */

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

void launcher_app_launch(launch_app_id_t id, const char *arg)
{
    if (id < 0 || id >= LAUNCH_APP_COUNT) return;
    if (s_depth >= APP_STACK_MAX) {
        ESP_LOGW("launcher", "app stack full (%d), launch %d ignored", APP_STACK_MAX, id);
        return;
    }
    const app_manifest_t *m = &app_manifests[id];
    if (!m->launch) return;
    app_slot_t *slot = &s_stack[s_depth];
    s_launch_arg = arg;   /* APP launch 内调用 launcher_app_get_arg() 获取 */
    slot->app = m->launch(lv_screen_active(), launcher_app_close, NULL);
    s_launch_arg = NULL;
    slot->m = m;
    s_depth++;
    ESP_LOGI("launcher", "[APP] push %s (depth=%d%s)", m->name, s_depth,
             arg ? ", arg=direct-open" : "");
}

/* 关闭栈顶 APP：弹栈回来源（来源对象树保活，状态天然保留） */
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
        /* 按下效果：边框绿色加亮 + 整体轻微缩小（含图标/文字）。
         * 用 transform_scale（256=100%，250≈97.7%，长宽各缩约 3px）——
         * 父对象 transform 会作用于子对象绘制，图标/文字同步缩放；
         * 注意勿用 transform_width/height（只改绘制边界，不缩内容） */
        lv_obj_set_style_border_color(card, lv_color_hex(ACCENT_COLOR_HI), LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(card, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(card, lv_pct(50), 0);
        lv_obj_set_style_transform_scale(card, 250, LV_STATE_PRESSED);
        /* 卡片点击 → 启动对应 APP（占位卡无操作） */
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, on_card_event, LV_EVENT_CLICKED, NULL);
        s_launcher.cards[i] = card;

        /* 卡片内容：白色图标 + APP 名称（relayout 固定定位：图标左对齐竖线）。
         * 图标字体：新增字形（书本/终端）+ fallback montserrat_28（其余图标） */
        lv_obj_t *icon_lbl = lv_label_create(card);
        lv_label_set_text(icon_lbl, s_app_icons[i]);
        lv_obj_add_flag(icon_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_text_font(icon_lbl, &lv_font_launcher_icons, 0);
        lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0xFFFFFF), 0);   /* 固定白色 */
        lv_obj_set_style_text_color(icon_lbl, lv_color_hex(ACCENT_COLOR_HI), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(icon_lbl, LV_OPA_TRANSP, 0);
        s_launcher.icon_labels[i] = icon_lbl;

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
     * 内容：白色图标贴左（x=16，所有卡片同一竖线），名称挨着图标右侧 */
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_set_size(s_launcher.cards[i], s_launcher.card_w, s_launcher.card_h);
        lv_obj_set_pos(s_launcher.cards[i], 0, i * s_launcher.unit);
        lv_obj_align(s_launcher.icon_labels[i], LV_ALIGN_LEFT_MID, 10, 0);
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

    /* 注册输入层返回事件（右滑手势触发 → 统一分发当前 APP） */
    gesture_set_back_handler(launcher_app_swipe_back, NULL);

    return root;
}

void launcher_relayout(lv_obj_t *obj)
{
    if (obj != s_launcher.root) return;
    launcher_relayout_core();
}
