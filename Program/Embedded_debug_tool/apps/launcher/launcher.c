#include "launcher.h"
#include "file_browser.h"
#include "reader_app.h"
#include "net_console.h"
#include "terminal.h"
#include "card_reader.h"
#include "dap_link.h"
#include "wave_gen.h"
#include "scope_app.h"
#include "usb2ttl.h"
#include "gesture.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "esp_log.h"

/* ── App 卡片表（每卡最多 3 行，每行 ≤15 字符防折行） ── */
#define APP_COUNT 9

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
    &launcher_icon_serialip,   /* USB2TTL（复用图标，后续可换） */
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
    { "USB2TTL", APP_TYPE_LAUNCH,     LAUNCH_APP_USB2TTL },
};

/* ── 主题色（赛博朋克：暗底 + 霓虹青边框 + 霓虹品红拨轮） ── */
#define ACCENT_COLOR     0x00F0FF   /* 霓虹青：选定框边框固定色 */
#define ACCENT_COLOR_HI  0x7DF9FF   /* 亮青（按下边框/当前行文字） */
#define WHEEL_COLOR      0xFF00E5   /* 霓虹品红：调速拨轮（轨道/填充/圆钮） */
#define THEME_DARK_BG    0x0A0A12
#define THEME_DARK_CARD  0x12121F
#define THEME_DARK_TEXT  0xE8E8F0
#define THEME_LIGHT_BG   0xE8E8F0
#define THEME_LIGHT_CARD 0xFFFFFF
#define THEME_LIGHT_TEXT 0x1A1A2E

/* ── 3 行列表选择器布局参数（3 行铺满屏幕，正常列表语义） ── */
#define SEL_LEFT_GAP     8     /* 行内容左缘距屏幕左边框 */
#define SEL_RIGHT_GAP    10    /* 行内容右缘距拨轮左缘 */
#define SEL_TOP_GAP      8     /* 顶部留边（行铺满剩余高度） */
#define SEL_ROW_GAP      12    /* 行间距 */
#define SEL_BORDER       4     /* 选定框边框厚度 */
#define SEL_RADIUS       16    /* 选定框圆角 */
#define SEL_ICON_X       20    /* 图标左缘（行内） */
#define SEL_DIVIDER_X    66    /* 图标/名字分隔竖线 x（行内） */
#define SEL_NAME_X       80    /* 名字左缘（行内） */
#define SEL_SWIPE_TH     40    /* 垂直滑动切换阈值（px 累计） */
#define SEL_FLING_MS     90    /* 惯性连续切换间隔（ms/格） */
#define SEL_FLING_MAX    3     /* 惯性最大格数（防止轻滑滑到底） */

/* ── 调速拨轮参数（屏幕右侧垂直正中） ── */
#define WHEEL_W          14    /* 拨轮视觉总宽 */
#define WHEEL_RIGHT_GAP  8     /* 拨轮右缘距屏幕右缘 */
#define WHEEL_KNOB_D     12    /* 基准直径 */
#define WHEEL_KNOB_W     14    /* 滑块直径 = 直径×1.2 */
#define WHEEL_KNOB_GAP   3     /* 滑块与行程端点间隙（悬浮感） */
#define WHEEL_SHELL_W    18    /* 胶囊外框宽（= 滑块宽 + 两侧边框余量） */
#define WHEEL_SHELL_BORDER 2   /* 胶囊外框边框厚度 */
#define WHEEL_TOUCH_PAD  8     /* 触摸热区外扩 */
#define WHEEL_MAX_SWITCH 10.0f /* 圆钮推满时的自动切换速率（个/s） */
#define WHEEL_DEAD_ZONE  0.1f  /* 中心死区（防误触） */
#define WHEEL_TICK_MS    20    /* 速度驱动定时器周期 */
#define WHEEL_RETURN_MS  160   /* 松手回中动画时长 */

/* ── 单实例状态（启动器全屏唯一） ── */
typedef struct {
    lv_obj_t *root;

    /* 3 行切换选择器：当前行（选定框框住）+ 上下预览行。
     * 每行 = 图标 | 竖线 | 名字（无卡片边框） */
    lv_obj_t *sel_rows[3];      /* 0=预览(上) 1=当前 2=预览(下)，容器（透明） */
    lv_obj_t *sel_icons[3];     /* 行内图标 */
    lv_obj_t *sel_divs[3];      /* 行内分隔竖线 */
    lv_obj_t *sel_names[3];     /* 行内名字 */
    lv_obj_t *sel_frame;        /* 选定框（霓虹边框，独立对象做滑入动画） */
    lv_obj_t *idx_lbl;          /* 右上角 "N/9" 指示 */
    int cur_idx;                /* 当前选中 index */
    int32_t frame_anim_val;     /* 选定框滑入动画变量 */

    /* 垂直滑动切换（root 事件） */
    lv_coord_t press_y0;        /* 按下起点 y */
    lv_coord_t press_y_last;    /* 上一帧 y */
    int32_t swipe_accum;        /* dy 累计（达到阈值切换一次并清零段） */
    uint32_t press_last_tick;   /* 测速时间戳 */
    bool press_moved;           /* 本次按下是否已切换过（单击判定） */
    lv_timer_t *fling_timer;    /* 惯性连续切换定时器 */
    int fling_dir;              /* 惯性方向：-1=上一个 +1=下一个，0=停止 */
    int fling_count;            /* 惯性剩余格数（有限，防轻滑滑到底） */
    int win_start;              /* 3 行窗口首行 index（正常列表语义） */

    /* 调速拨轮 */
    lv_obj_t *wheel;             /* 触摸热区容器（覆盖整个拨轮区域） */
    lv_obj_t *knob_shell;        /* 胶囊外框（粉色边框，内部透明） */
    lv_obj_t *knob;              /* 滑块（胶囊形：矩形 + 上下半圆） */
    lv_timer_t *wheel_timer;     /* 按住期间驱动自动切换（空闲暂停） */
    bool wheel_pressed;
    uint32_t wheel_last_tick;
    float wheel_accum;           /* 切换频率累积器 */
    float knob_pos;              /* -1..1（圆钮偏离中心，负=上推） */
    int32_t knob_anim_val;       /* 松手回中动画变量 */

    bool dark;

    /* 主题动画 */
    int32_t theme_val;
    lv_color_t theme_from;

    /* 几何（relayout 计算） */
    int sel_w, sel_row_h, sel_gap, sel_rows_y[3];   /* 三行 y 坐标 */
    int wheel_cx, wheel_cy, wheel_travel, wheel_w, wheel_h;   /* 拨轮几何 */
} launcher_t;

static launcher_t s_launcher;

/* ── 几何计算 ── */

static void launcher_compute_geom(void)
{
    lv_display_t *disp = lv_display_get_default();
    int sw = lv_display_get_horizontal_resolution(disp);
    int sh = lv_display_get_vertical_resolution(disp);

    /* 行内容宽：贴左，右缘贴近拨轮（留 SEL_RIGHT_GAP 间距） */
    s_launcher.sel_w = sw - SEL_LEFT_GAP - SEL_RIGHT_GAP - WHEEL_W - WHEEL_RIGHT_GAP;

    /* 三行铺满屏幕（正常列表语义）：顶部留边，行高 = 剩余高度均分。
     * 选中项跟随其在列表中的自然位置（第一项在顶行、末项在底行） */
    s_launcher.sel_gap = SEL_ROW_GAP;
    int row_h = (sh - 2 * SEL_TOP_GAP - 2 * SEL_ROW_GAP) / 3;
    if (row_h < 60) row_h = 60;
    s_launcher.sel_row_h = row_h;
    int y0 = SEL_TOP_GAP;
    for (int i = 0; i < 3; i++) {
        s_launcher.sel_rows_y[i] = y0 + i * (row_h + SEL_ROW_GAP);
    }

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

/* ── 3 行切换选择器：核心切换 ──
 * dir=+1 下一个（index+1，内容向下）；dir=-1 上一个（index-1）。
 * 三行内容直接替换（无平移渲染），选定框从相邻方向滑入吸附到当前行。 */

static void launcher_selector_refresh(void);   /* 前向：刷新 3 行窗口内容 */
static void launcher_selector_frame_anim(int target_row, int dir);   /* 前向：框滑入动画 */

static bool launcher_selector_move(int dir)
{
    int cur = s_launcher.cur_idx;
    if (dir > 0) {
        if (cur >= APP_COUNT - 1) return false;   /* 到底 */
        cur++;
        if (cur > s_launcher.win_start + 2) s_launcher.win_start++;   /* 窗口下滚一格 */
    } else {
        if (cur <= 0) return false;               /* 到顶 */
        cur--;
        if (cur < s_launcher.win_start) s_launcher.win_start--;       /* 窗口上滚一格 */
    }
    s_launcher.cur_idx = cur;
    ESP_LOGI("launcher", "[SEL] idx=%d/%d (%s)", cur, APP_COUNT, s_apps[cur].name);
    launcher_selector_refresh();   /* 窗口滚动时 3 行内容替换 + 高亮框定位 */
    /* 框滑入到选中行（正常列表语义：选中行可位于顶/中/底行） */
    launcher_selector_frame_anim(cur - s_launcher.win_start, dir);
    return true;
}

/* 惯性连续切换（有限格数，防止轻滑一路滑到底） */
static void launcher_fling_tick(lv_timer_t *t)
{
    (void)t;
    if (s_launcher.fling_count <= 0) {
        lv_timer_pause(s_launcher.fling_timer);
        s_launcher.fling_dir = 0;
        return;
    }
    if (launcher_selector_move(s_launcher.fling_dir)) {
        s_launcher.fling_count--;
    } else {
        s_launcher.fling_count = 0;
        lv_timer_pause(s_launcher.fling_timer);
        s_launcher.fling_dir = 0;
    }
}

static void launcher_fling_start(int dir, int count)
{
    /* 手指已落下时 timer 被 pause；重复启动无害（reset 重新计时，方向/格数覆盖） */
    s_launcher.fling_dir = dir;
    s_launcher.fling_count = count;
    lv_timer_reset(s_launcher.fling_timer);
    lv_timer_resume(s_launcher.fling_timer);
}

/* ── 调速拨轮 ──
 * 按住圆钮上下拖动：偏离中心越远自动切换速率越快（控制切换速度）；
 * 上推 → 选下一个，下拉 → 选上一个；松手圆钮弹回中心、切换停止。 */

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
    /* 上推(pos<0) → 下一个；下拉(pos>0) → 上一个。速率 = |pos|×WHEEL_MAX_SWITCH */
    s_launcher.wheel_accum += pos * WHEEL_MAX_SWITCH * dt;
    while (s_launcher.wheel_accum >= 1.0f) {
        s_launcher.wheel_accum -= 1.0f;
        if (!launcher_selector_move(-1)) break;   /* 到底停止 */
    }
    while (s_launcher.wheel_accum <= -1.0f) {
        s_launcher.wheel_accum += 1.0f;
        if (!launcher_selector_move(1)) break;
    }
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
    /* 主题变化 → 刷新三行内容（当前行/预览行配色在 refresh 内按主题设置） */
    launcher_selector_refresh();
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
    lv_obj_t *root;               /* APP 根对象（screen 顶层，拖动返回时平移） */
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
        .drag_exit = (void (*)(void *))reader_app_drag_exit,
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
    [LAUNCH_APP_USB2TTL] = {
        .id = LAUNCH_APP_USB2TTL,
        .name = "USB2TTL",
        .launch = (void *(*)(lv_obj_t *, void (*)(void *), void *))usb2ttl_create,
        .destroy = (void (*)(void *))usb2ttl_destroy,
        .back = (bool (*)(void *))usb2ttl_swipe_back,
        .rotate = NULL,
        .refresh = NULL,
        .debug_event = NULL,
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
        slot->root = app_root;
        launcher_play_enter_anim(app_root);
    } else {
        slot->root = NULL;
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

/* ── 手机式滑动返回（拖动） ──
 * 手势层贴边右滑 → 拖动回调（launcher 注册）：
 *   · 拖动中：先问栈顶 back()——false=非全屏界面（键盘/对话框）内部已处理，
 *     不进拖动；true=可拖动 → 平移当前 APP root 的 x 跟随手指（露出下层）。
 *   · 松手：dx > 屏宽/3 → 动画滑出 + 完成后 destroy 弹栈；
 *           否则回弹原位（取消返回）。
 * 拖动期间 LVGL 正常渲染（仅平移 root，APP 功能不受影响）。 */
#define SWIPE_BACK_RATIO 3   /* 滑出阈值 = 屏宽/3 */

static lv_obj_t *s_drag_root;    /* 拖动中的 APP root（松手时验证未被销毁） */
static bool s_drag_armed;        /* 拖动已获准（back() 判定通过，仅首帧判定一次） */

static void launcher_drag_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void launcher_drag_exit_done(lv_anim_t *a)
{
    (void)a;
    /* 先取栈顶 manifest（launcher_app_close 弹栈后 m 失效）：
     * 有 drag_exit 回调 → APP 自行决定滑出后行为（如关覆盖层回书架）；
     * 无 → 默认弹栈销毁 */
    app_slot_t *top = stack_top();
    const app_manifest_t *m = top ? top->m : NULL;
    s_drag_root = NULL;
    s_drag_armed = false;
    if (m && m->drag_exit) {
        m->drag_exit(top->app);
    } else {
        launcher_app_close(NULL);
    }
}

static void launcher_drag_animate(lv_obj_t *root, int to_x, int ms,
                                  bool exit)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_exec_cb(&a, launcher_drag_x_cb);
    lv_anim_set_values(&a, lv_obj_get_x(root), to_x);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (exit) lv_anim_set_completed_cb(&a, launcher_drag_exit_done);
    lv_anim_start(&a);
}

static void launcher_on_drag(void *ctx, int dx, bool pressed)
{
    (void)ctx;
    app_slot_t *top = stack_top();
    if (!top || !top->root) return;
    lv_display_t *disp = lv_display_get_default();
    int sw = lv_display_get_horizontal_resolution(disp);

    if (pressed) {
        if (!s_drag_armed) {
            /* 拖动首帧：判定一次是否可返回（非全屏界面如键盘/对话框由
             * back() 处理返回 false → 整个手势取消，不进入拖动）。
             * 不逐帧调用——file_browser 等 back() 有副作用（目录上跳/SD 枚举） */
            s_drag_armed = true;
            if (!top->m->back || !top->m->back(top->app)) {
                s_drag_armed = false;
                s_drag_root = NULL;
                ESP_LOGI("launcher", "[DRAG] cancelled (non-fullscreen, back handled internally)");
                return;
            }
            s_drag_root = top->root;   /* 记录拖动目标（松手时验证未被销毁） */
        }
        int x = dx;
        if (x < 0) x = 0;
        if (x > sw) x = sw;
        if (s_drag_root) lv_obj_set_x(s_drag_root, x);
    } else {
        /* 松手：判定滑出或回弹。拖动目标已销毁（APP 自行关闭）→ 跳过动画 */
        app_slot_t *cur_top = stack_top();
        if (!cur_top || cur_top->root != s_drag_root || !s_drag_root) {
            s_drag_root = NULL;
            s_drag_armed = false;
            return;
        }
        int cur = lv_obj_get_x(s_drag_root);
        if (cur > sw / SWIPE_BACK_RATIO) {
            launcher_drag_animate(s_drag_root, sw, 150, true);   /* 滑出 → 返回 */
            ESP_LOGI("launcher", "[DRAG] release dx=%d -> exit", dx);
        } else {
            launcher_drag_animate(s_drag_root, 0, 150, false);   /* 回弹原位 */
            ESP_LOGI("launcher", "[DRAG] release dx=%d -> cancel", dx);
        }
        s_drag_armed = false;
    }
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

/* ── 3 行切换选择器：刷新 / 选定框动画 / 构建 / 滑动事件 ── */

static void launcher_frame_anim_exec(void *var, int32_t v)
{
    (void)var;
    lv_obj_set_y(s_launcher.sel_frame, v);
}

/* 选定框滑入动画：滑到 target_row 行（0=顶 1=中 2=底）。
 * dir>0（下一个，上滑触发）内容来自下方 → 框从下方相邻行滑入；dir<0 反之 */
static void launcher_selector_frame_anim(int target_row, int dir)
{
    int y_target = s_launcher.sel_rows_y[target_row];
    int from_row = target_row + ((dir > 0) ? 1 : -1);
    if (from_row < 0) from_row = 0;
    if (from_row > 2) from_row = 2;
    int y_from = s_launcher.sel_rows_y[from_row];
    lv_obj_set_y(s_launcher.sel_frame, y_from);
    lv_anim_delete(&s_launcher.frame_anim_val, launcher_frame_anim_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_launcher.frame_anim_val);
    lv_anim_set_exec_cb(&a, launcher_frame_anim_exec);
    lv_anim_set_values(&a, y_from, y_target);
    lv_anim_set_duration(&a, 100);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 刷新 3 行窗口内容（win_start..win_start+2）+ 高亮行定位。
 * 正常列表语义：选中项（cur）位于窗口中的自然位置，第一项在顶行、末项在底行 */
static void launcher_selector_refresh(void)
{
    int start = s_launcher.win_start;
    bool dark = s_launcher.dark;
    lv_color_t cur_c = lv_color_hex(dark ? THEME_DARK_TEXT : THEME_LIGHT_TEXT);
    lv_color_t prev_c = lv_color_hex(dark ? 0x4A5568 : 0x9CA3AF);

    for (int i = 0; i < 3; i++) {
        int idx = start + i;
        if (idx < 0 || idx >= APP_COUNT) {   /* 防御：窗口始终界内 */
            lv_obj_add_flag(s_launcher.sel_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_launcher.sel_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_launcher.sel_icons[i], s_app_icons[idx]);
        lv_label_set_text(s_launcher.sel_names[i], s_apps[idx].name);

        if (idx == s_launcher.cur_idx) {
            /* 选中行：大字亮色，图标/竖线全亮 */
            lv_obj_set_style_text_font(s_launcher.sel_names[i], &lv_font_montserrat_28, 0);
            lv_obj_set_style_text_color(s_launcher.sel_names[i], cur_c, 0);
            lv_obj_set_style_opa(s_launcher.sel_icons[i], LV_OPA_COVER, 0);
            lv_obj_set_style_opa(s_launcher.sel_divs[i], LV_OPA_COVER, 0);
        } else {
            /* 非选中行：小字灰色半透明 */
            lv_obj_set_style_text_font(s_launcher.sel_names[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(s_launcher.sel_names[i], prev_c, 0);
            lv_obj_set_style_opa(s_launcher.sel_icons[i], LV_OPA_40, 0);
            lv_obj_set_style_opa(s_launcher.sel_divs[i], LV_OPA_50, 0);
        }
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", s_launcher.cur_idx + 1, APP_COUNT);
    lv_label_set_text(s_launcher.idx_lbl, buf);

    /* 高亮框定位到选中行（无动画直接设；切换动画由 frame_anim 驱动） */
    lv_obj_set_pos(s_launcher.sel_frame, SEL_LEFT_GAP,
                   s_launcher.sel_rows_y[s_launcher.cur_idx - start]);
}

/* 根事件：垂直滑动切换 + 单击启动。
 * 拨轮区域有自己的事件（不冒泡到 root），不冲突。 */
static void on_sel_swipe_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        s_launcher.press_y0 = p.y;
        s_launcher.press_y_last = p.y;
        s_launcher.swipe_accum = 0;
        s_launcher.press_moved = false;
        s_launcher.press_last_tick = lv_tick_get();
        /* 手指落下打断惯性切换 */
        s_launcher.fling_dir = 0;
        lv_timer_pause(s_launcher.fling_timer);
        break;
    }
    case LV_EVENT_PRESSING: {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_coord_t dy = p.y - s_launcher.press_y_last;
        s_launcher.press_y_last = p.y;
        s_launcher.swipe_accum += dy;
        /* 下滑(dy>0) → 上一个（选中框向顶）；上滑 → 下一个。达阈值立即切换，余量保留 */
        while (s_launcher.swipe_accum >= SEL_SWIPE_TH) {
            s_launcher.swipe_accum -= SEL_SWIPE_TH;
            if (launcher_selector_move(-1)) {
                s_launcher.press_moved = true;
            } else {
                s_launcher.swipe_accum = 0;
                break;
            }
        }
        while (s_launcher.swipe_accum <= -SEL_SWIPE_TH) {
            s_launcher.swipe_accum += SEL_SWIPE_TH;
            if (launcher_selector_move(1)) {
                s_launcher.press_moved = true;
            } else {
                s_launcher.swipe_accum = 0;
                break;
            }
        }
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_INDEV_RESET: {
        if (s_launcher.press_moved) {
            /* 松手惯性：按滑动位移折算有限格数（每 2×阈值一格，最多 SEL_FLING_MAX），
             * 防止轻滑一路滑到底 */
            lv_coord_t total = s_launcher.press_y_last - s_launcher.press_y0;
            int dir = (total > 0) ? -1 : 1;   /* 下滑=上一个；上滑=下一个 */
            int n = (abs((int)total) - SEL_SWIPE_TH) / (SEL_SWIPE_TH * 2);
            if (n > SEL_FLING_MAX) n = SEL_FLING_MAX;
            if (n > 0) launcher_fling_start(dir, n);
        }
        break;
    }
    case LV_EVENT_CLICKED: {
        if (!s_launcher.press_moved) {
            /* 单击（无滑动）→ 启动当前选中 APP */
            if (s_apps[s_launcher.cur_idx].type == APP_TYPE_LAUNCH) {
                ESP_LOGI("launcher", "[SEL] click -> launch %s",
                         s_apps[s_launcher.cur_idx].name);
                launcher_app_launch(s_apps[s_launcher.cur_idx].id, NULL);
            }
        }
        break;
    }
    default:
        break;
    }
}

/* 构建 3 行选择器：行 = 图标 | 竖线 | 名字（无卡片边框） + 选定框 + 索引指示 */
static void launcher_build_selector(void)
{
    lv_obj_t *root = s_launcher.root;

    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(root);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                           | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        s_launcher.sel_rows[i] = row;

        lv_obj_t *icon = lv_image_create(row);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
        s_launcher.sel_icons[i] = icon;

        lv_obj_t *div = lv_obj_create(row);
        lv_obj_remove_flag(div, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(div, lv_color_hex(ACCENT_COLOR_HI), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(div, 0, 0);
        lv_obj_set_style_radius(div, 1, 0);
        lv_obj_set_size(div, 2, 40);   /* 高度 relayout 时按行高调整 */
        s_launcher.sel_divs[i] = div;

        lv_obj_t *name = lv_label_create(row);
        lv_obj_add_flag(name, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_opa(name, LV_OPA_TRANSP, 0);
        s_launcher.sel_names[i] = name;
    }

    /* 选定框：霓虹青边框 + 圆角，透明底（独立对象做滑入动画） */
    lv_obj_t *frame = lv_obj_create(root);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(ACCENT_COLOR), 0);
    lv_obj_set_style_border_width(frame, SEL_BORDER, 0);
    lv_obj_set_style_radius(frame, SEL_RADIUS, 0);
    s_launcher.sel_frame = frame;

    /* 索引指示（右上角） */
    lv_obj_t *il = lv_label_create(root);
    lv_obj_set_style_text_font(il, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(il, lv_color_hex(0x4A5568), 0);
    lv_obj_align(il, LV_ALIGN_TOP_RIGHT, -10, 8);
    s_launcher.idx_lbl = il;

    /* 惯性切换定时器 */
    s_launcher.fling_timer = lv_timer_create(launcher_fling_tick, SEL_FLING_MS, NULL);
    lv_timer_pause(s_launcher.fling_timer);

    /* 根事件：垂直滑动切换 + 单击启动（LVGL 9 事件 filter 精确，逐个注册） */
    lv_obj_add_event_cb(root, on_sel_swipe_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, on_sel_swipe_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, on_sel_swipe_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, on_sel_swipe_event, LV_EVENT_INDEV_RESET, NULL);
    lv_obj_add_event_cb(root, on_sel_swipe_event, LV_EVENT_CLICKED, NULL);
}

static void launcher_relayout_core(void)
{
    lv_display_t *disp = lv_display_get_default();
    int sw = lv_display_get_horizontal_resolution(disp);
    int sh = lv_display_get_vertical_resolution(disp);

    launcher_compute_geom();

    /* root 跟随逻辑分辨率（旋转后必须同步） */
    lv_obj_set_size(s_launcher.root, sw, sh);

    /* 三行选择器：行容器（图标 | 竖线 | 名字）+ 选定框 */
    for (int i = 0; i < 3; i++) {
        lv_obj_set_pos(s_launcher.sel_rows[i], SEL_LEFT_GAP, s_launcher.sel_rows_y[i]);
        lv_obj_set_size(s_launcher.sel_rows[i], s_launcher.sel_w, s_launcher.sel_row_h);
        lv_obj_align(s_launcher.sel_icons[i], LV_ALIGN_LEFT_MID, SEL_ICON_X, 0);
        lv_obj_align(s_launcher.sel_divs[i], LV_ALIGN_LEFT_MID, SEL_DIVIDER_X, 0);
        lv_obj_align(s_launcher.sel_names[i], LV_ALIGN_LEFT_MID, SEL_NAME_X, 0);
        lv_obj_set_size(s_launcher.sel_divs[i], 2, s_launcher.sel_row_h - 24);
    }
    lv_obj_set_pos(s_launcher.sel_frame, SEL_LEFT_GAP, s_launcher.sel_rows_y[1]);
    lv_obj_set_size(s_launcher.sel_frame, s_launcher.sel_w, s_launcher.sel_row_h);

    launcher_selector_refresh();   /* 内容/索引/边界同步（含旋转后重排） */

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

    /* 3 行列表选择器（窗口铺满屏幕，正常列表语义）+ 右侧调速拨轮 */
    s_launcher.cur_idx = 0;
    s_launcher.win_start = 0;
    launcher_build_selector();
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

    /* 手机式滑动返回：贴边右滑 → 拖动回调（平移当前 APP + 松手判定）。
     * back 回调仍保留：拖动开始前用它判断是否可返回（键盘/对话框等
     * 非全屏界面 back()==false，不进入拖动） */
    gesture_set_drag_handler(launcher_on_drag, NULL);

    return root;
}

void launcher_relayout(lv_obj_t *obj)
{
    if (obj != s_launcher.root) return;
    launcher_relayout_core();
}
