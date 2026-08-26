/* io_picker.c —— 通用 IO 引脚选择模块（见 io_picker.h） */

#include "io_picker.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "io_picker";

/* ── 静态资源表：只标硬件性不可用（板上焊死/总线独占） ──
 * 可用池：IO2,4..18,21..32,37,43,44（系统 APP 占用不算，App 负责自行配置） */

typedef enum {
    IO_HW_STD = 0,   /* 普通数字 IO（可矩阵复用） */
    IO_HW_ADC,       /* ADC1/ADC2 通道（S3：ADC1=IO1..10，ADC2=IO11..20） */
    IO_HW_RESERVED,  /* 硬件性不可用（owner 说明原因） */
} io_hw_t;

typedef struct {
    int io;
    io_hw_t hw;
    const char *owner;   /* RESERVED 的占用者；NULL=无 */
} io_pin_def_t;

/* 硬件性不可用清单（灰）：BOOT/LCD/SD/Touch/USB/IMU */
static const int s_reserved[] = {
    0, 1, 3, 19, 20, 33, 34, 35, 36, 38, 39, 40, 41, 42, 45, 46, 47, 48,
};
#define RESERVED_N (sizeof(s_reserved) / sizeof(s_reserved[0]))

/* 保留引脚说明（顺序对应 s_reserved） */
static const char *const s_reserved_owner[RESERVED_N] = {
    "BOOT",         /* 0 */
    "LCD-BL",       /* 1 */
    "IMU-INT",      /* 3 */
    "USB-D",        /* 19 */
    "USB-D",        /* 20 */
    "SD-MOSI",      /* 33 */
    "SD-CLK",       /* 34 */
    "SD-MISO",      /* 35 */
    "SD-CS",        /* 36 */
    "LCD-MOSI",     /* 38 */
    "LCD-CLK",      /* 39 */
    "LCD/IMU",      /* 40 */
    "IMU",          /* 41 */
    "LCD-DC",       /* 42 */
    "LCD-CS",       /* 45 */
    "TP-INT",       /* 46 */
    "TP-SCL",       /* 47 */
    "TP-SDA",       /* 48 */
};

/* ── 动态占用位图（App 运行时占用，退出时释放） ── */
static uint64_t s_reserved_map[2];   /* 0..63 / 64..127 */

static bool io_reserved_static(int io)
{
    for (size_t i = 0; i < RESERVED_N; i++) {
        if (s_reserved[i] == io) return true;
    }
    return false;
}

static bool io_reserved_dynamic(int io)
{
    if (io < 0 || io >= 128) return false;
    return (s_reserved_map[io >> 6] >> (io & 63)) & 1u;
}

static void io_reserved_dyn_set(int io, bool set)
{
    if (io < 0 || io >= 128) return;
    uint64_t mask = 1ull << (io & 63);
    if (set) s_reserved_map[io >> 6] |= mask;
    else s_reserved_map[io >> 6] &= ~mask;
}

/* ADC 通道：S3 ADC1=IO1..10（排除 3 保留），ADC2=IO11..20（排除 19,20） */
static bool io_is_adc(int io)
{
    if (io >= 1 && io <= 10) return true;
    if (io >= 11 && io <= 20) return true;
    return false;
}

/* 该脚对给定能力是否可用（静态未保留 + 动态未占用 + 能力匹配） */
bool io_picker_is_available(int io, uint32_t caps)
{
    if (io < 0 || io > 48) return false;
    if (io >= 22 && io <= 25) return false;   /* S3 未引出 */
    if (io_reserved_static(io) || io_reserved_dynamic(io)) return false;
    if (caps & IO_CAP_ADC) return io_is_adc(io);
    return true;   /* GPIO/UART/SPI/I2C/PWM：S3 IO 矩阵全复用 */
}

void io_picker_reserve(int io)
{
    io_reserved_dyn_set(io, true);
    ESP_LOGI(TAG, "reserve IO%02d", io);
}

void io_picker_release(int io)
{
    io_reserved_dyn_set(io, false);
    ESP_LOGI(TAG, "release IO%02d", io);
}

int io_picker_alloc(uint32_t caps)
{
    for (int io = 0; io <= 48; io++) {
        if (io_picker_is_available(io, caps)) return io;
    }
    return -1;
}

/* ── UI ── */

typedef struct {
    uint32_t caps;
    io_pick_done_t cb;
    void *ctx;
} io_ui_t;

static io_ui_t *s_ui;          /* 当前选择界面状态（NULL=无） */
static lv_obj_t *s_dlg;        /* 遮罩根 */

static io_hw_t io_hw_of(int io)
{
    if (io_reserved_static(io)) return IO_HW_RESERVED;
    if (io_is_adc(io)) return IO_HW_ADC;
    return IO_HW_STD;
}

static const char *io_reserved_owner_of(int io)
{
    for (size_t i = 0; i < RESERVED_N; i++) {
        if (s_reserved[i] == io) return s_reserved_owner[i];
    }
    return NULL;
}

static void io_ui_close(int io)
{
    if (!s_ui) return;
    io_pick_done_t cb = s_ui->cb;
    void *ctx = s_ui->ctx;
    lv_obj_t *dlg = s_dlg;
    s_ui = NULL;
    s_dlg = NULL;
    if (dlg) lv_obj_delete(dlg);
    if (io < 0) ESP_LOGI(TAG, "picker cancelled");
    else ESP_LOGI(TAG, "picked IO%02d", io);
    if (cb) cb(ctx, io);
}

static void io_ui_pin_evt(lv_event_t *e)
{
    io_ui_t *ui = lv_event_get_user_data(e);
    int io = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!ui || io < 0) return;
    io_ui_close(io);
}

/* ── 空白点击取消（返回/整屏拖动由 launcher 统一处理：
 * 拖动触发时 gesture 已 wait_release → LVGL 不再向界面派发触摸，屏只随根移动） ── */

static void io_ui_evt(lv_event_t *e)
{
    if (!s_dlg) return;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED &&
        lv_event_get_target(e) == s_dlg) {
        io_ui_close(-1);   /* 点空白取消 */
    }
}

bool io_picker_show(lv_obj_t *parent, uint32_t caps, io_pick_done_t cb, void *ctx)
{
    if (s_ui) return false;
    if (!parent) return false;

    io_ui_t *ui = lv_malloc(sizeof(io_ui_t));
    if (!ui) return false;
    ui->caps = caps;
    ui->cb = cb;
    ui->ctx = ctx;

    lv_display_t *disp = lv_display_get_default();
    int sw = disp ? lv_display_get_horizontal_resolution(disp) : 320;
    int sh = disp ? lv_display_get_vertical_resolution(disp) : 240;

    /* 全屏深色层（无标题/说明；两列按钮贴左右边缘） */
    lv_obj_t *dlg = lv_obj_create(parent);
    lv_obj_remove_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dlg, sw, sh);
    lv_obj_set_pos(dlg, 0, 0);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dlg, 0, 0);
    lv_obj_set_style_radius(dlg, 0, 0);
    lv_obj_set_style_pad_all(dlg, 0, 0);
    lv_obj_add_event_cb(dlg, io_ui_evt, LV_EVENT_ALL, NULL);

    /* 左右两列（可滚动；左列贴左缘、右列贴右缘） */
    int col_w = sw / 2;
    static const int left_sel[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                    16, 17, 18, 19, 20, 21 };
    static const int right_sel[] = { 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
                                     38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48 };
#define LEFT_N  (sizeof(left_sel) / sizeof(left_sel[0]))
#define RIGHT_N (sizeof(right_sel) / sizeof(right_sel[0]))

    for (int side = 0; side < 2; side++) {
        const int *sel = side ? right_sel : left_sel;
        int n = side ? (int)RIGHT_N : (int)LEFT_N;
        lv_obj_t *col = lv_obj_create(dlg);
        lv_obj_set_pos(col, side ? sw / 2 : 0, 0);
        lv_obj_set_size(col, col_w, sh);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_radius(col, 0, 0);
        lv_obj_set_style_pad_all(col, 2, 0);
        lv_obj_set_style_pad_gap(col, 2, 0);
        lv_obj_set_scroll_dir(col, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        for (int i = 0; i < n; i++) {
            int io = sel[i];
            bool ok = io_picker_is_available(io, ui->caps);
            io_hw_t hw = io_hw_of(io);

            lv_obj_t *b = lv_button_create(col);
            lv_obj_set_size(b, lv_pct(100), 24);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, 4, 0);

            /* 分区色：可用=青/黄（ADC），不可用=灰 */
            lv_color_t bg;
            if (!ok || hw == IO_HW_RESERVED) {
                bg = lv_color_hex(0x374151);
            } else if (hw == IO_HW_ADC) {
                bg = lv_color_hex(0xFBBF24);
            } else {
                bg = lv_color_hex(0x1F6FB4);
            }
            lv_obj_set_style_bg_color(b, bg, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x2E5F8A), LV_STATE_PRESSED);

            lv_obj_t *l = lv_label_create(b);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
            lv_label_set_text_fmt(l, "IO%02d", io);

            if (!ok) {
                /* 不可用：显示原因小字，不可点 */
                const char *ow = hw == IO_HW_RESERVED ? io_reserved_owner_of(io) : NULL;
                if (!ow && io_reserved_dynamic(io)) ow = "busy";
                lv_obj_t *r = lv_label_create(b);
                lv_obj_align(r, LV_ALIGN_RIGHT_MID, -6, 0);
                lv_obj_set_style_text_font(r, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(r, lv_color_hex(0x9CA3AF), 0);
                lv_label_set_text(r, ow ? ow : (io >= 22 && io <= 25) ? "n/a" : "");
            } else {
                lv_obj_set_user_data(b, (void *)(intptr_t)io);
                lv_obj_add_event_cb(b, io_ui_pin_evt, LV_EVENT_CLICKED, ui);
            }
        }
    }

    s_ui = ui;
    s_dlg = dlg;
    ESP_LOGI(TAG, "picker shown caps=0x%lx", (unsigned long)caps);
    return true;
}

bool io_picker_active(void)
{
    return s_ui != NULL;
}

lv_obj_t *io_picker_get_obj(void)
{
    return s_dlg;
}

void io_picker_cancel(void)
{
    io_ui_close(-1);   /* 取消：回调 io=-1（调用方保留界面原位置） */
}

void io_picker_close_now(void)
{
    if (!s_ui) return;
    s_ui = NULL;   /* 不清回调：宿主即将销毁，不再调用 */
    if (s_dlg) {
        lv_obj_delete(s_dlg);
        s_dlg = NULL;
    }
}