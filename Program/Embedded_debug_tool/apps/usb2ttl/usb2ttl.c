/* usb2ttl.c —— USB 虚拟串口（CDC-ACM）↔ UART1 + ISP 下载 APP（LVGL 9）
 *
 * 配色沿用 card_reader（深色卡片风格），与桌面主题一致。
 * 状态轮询：服务状态来自 TinyUSB 任务/桥接任务，经 lv_timer 回到 LVGL
 * 线程刷新（250ms）。
 */

#include "usb2ttl.h"
#include "app_usb2ttl.h"
#include "app_cardreader.h"
#include "app_dap.h"
#include "app_font.h"
#include "io_picker.h"
#include "esp_log.h"
#include <stdio.h>

/* ── 配色（与 card_reader 一致） ── */
#define UU_BG        lv_color_hex(0x000000)
#define UU_CARD      lv_color_hex(0x000000)
#define UU_TEXT      lv_color_hex(0xFFFFFF)
#define UU_DIM       lv_color_hex(0x9CA3AF)
#define UU_ACCENT    lv_color_hex(0x39C5BB)
#define UU_ERR       lv_color_hex(0xEF4444)

/* 波特率循环档位 */
static const int s_bauds[] = { 115200, 57600, 9600 };
#define BAUDS_N  (sizeof(s_bauds) / sizeof(s_bauds[0]))

struct usb2ttl_app {
    lv_obj_t *root;
    usb2ttl_back_cb_t back_cb;
    void *back_ctx;

    /* 状态行 */
    lv_obj_t *val_state;
    lv_obj_t *val_pc;
    lv_obj_t *val_hint;

    /* 可编辑行按钮 */
    lv_obj_t *btn_baud;
    lv_obj_t *btn_parity;
    lv_obj_t *btn_boot0;
    lv_obj_t *btn_rst;

    /* 自动下载勾选框 */
    lv_obj_t *chk_auto;

    /* 底部按钮 */
    lv_obj_t *btn_toggle;
    lv_obj_t *lbl_toggle;

    lv_timer_t *timer;

    int num_opt;   /* 当前编辑的引脚（num_input 回调） */
};

static lv_font_t *uu_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* 行内编辑按钮的文本 label（第一个 child） */
static lv_obj_t *btn_label(lv_obj_t *btn)
{
    return lv_obj_get_child(btn, 0);
}

/* ── 状态 → 界面 ── */

static void uu_update(struct usb2ttl_app *uu)
{
    usb2ttl_state_t st = app_usb2ttl_get_state();
    cardreader_state_t cr = app_cardreader_get_state();
    bool cr_busy = (cr == CARDREADER_EXPOSED || cr == CARDREADER_APP_OWNED);
    bool dap_busy = (app_dap_get_state() == DAP_STATE_READY);
    bool usb_conflict = cr_busy || dap_busy;

    /* USB 状态 */
    static const char *st_txt[] = {
        [USB2TTL_OFF] = "未启用",
        [USB2TTL_ON] = "运行中",
        [USB2TTL_ERROR] = "异常",
    };
    lv_label_set_text(uu->val_state, st_txt[st]);
    lv_obj_set_style_text_color(uu->val_state,
                                st == USB2TTL_ON ? UU_ACCENT :
                                st == USB2TTL_ERROR ? UU_ERR : UU_DIM, 0);

    /* PC 连接 */
    bool pc_open = (st == USB2TTL_ON) && app_usb2ttl_pc_open();
    lv_label_set_text(uu->val_pc,
                      st == USB2TTL_ON ? (pc_open ? "已打开 (COM)" : "已枚举 · 未打开")
                                        : "未连接");
    lv_obj_set_style_text_color(uu->val_pc, pc_open ? UU_ACCENT : UU_DIM, 0);

    /* 可编辑行 */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", app_usb2ttl_get_baud());
    lv_label_set_text(btn_label(uu->btn_baud), buf);
    lv_label_set_text(btn_label(uu->btn_parity),
                      app_usb2ttl_get_parity_even() ? "8E1" : "8N1");
    int b0, rst;
    app_usb2ttl_get_isp_pins(&b0, &rst);
    snprintf(buf, sizeof(buf), "IO%d", b0);
    lv_label_set_text(btn_label(uu->btn_boot0), buf);
    snprintf(buf, sizeof(buf), "IO%d", rst);
    lv_label_set_text(btn_label(uu->btn_rst), buf);

    /* 提示 */
    const char *hint;
    if (usb_conflict) {
        hint = cr_busy
            ? "读卡器(MSD)占用 USB，请先关闭再\n开启本功能。"
            : "DAP(SWD)占用 USB，请先关闭再开\n启本功能。";
    } else if (st == USB2TTL_ON) {
        hint = "桥接运行中：PC 打开串口后即可双向\n收发。UART1 已被独占，TCP/终端\n转发暂停。ISP 复位约 0.5s。";
    } else if (st == USB2TTL_ERROR) {
        hint = "上次启动失败，请检查 USB 连接后\n重试。";
    } else {
        hint = "开启后 PC 枚举为 USB2TTL，经\nUART1(IO2/IO4) 连目标。勾选自动\n下载后可由 DTR/RTS 触发 ISP。";
    }
    lv_label_set_text(uu->val_hint, hint);

    /* 开启/关闭按钮 */
    bool active = (st == USB2TTL_ON);
    lv_label_set_text(uu->lbl_toggle, active ? "关闭 USB2TTL" : "开启 USB2TTL");
    if (!active && usb_conflict) {
        lv_obj_add_state(uu->btn_toggle, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(uu->btn_toggle, LV_STATE_DISABLED);
    }

    /* 波特率/校验按钮：仅 OFF 可改（服务层同样拒绝） */
    if (st == USB2TTL_OFF) {
        lv_obj_remove_state(uu->btn_baud, LV_STATE_DISABLED);
        lv_obj_remove_state(uu->btn_parity, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(uu->btn_baud, LV_STATE_DISABLED);
        lv_obj_add_state(uu->btn_parity, LV_STATE_DISABLED);
    }

    /* 自动下载勾选框与服务层标志同步（标志跨 APP 生命周期保留） */
    bool checked = lv_obj_has_state(uu->chk_auto, LV_STATE_CHECKED);
    if (checked != app_usb2ttl_get_auto_isp()) {
        if (app_usb2ttl_get_auto_isp()) {
            lv_obj_add_state(uu->chk_auto, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(uu->chk_auto, LV_STATE_CHECKED);
        }
    }
}

/* ── 按钮回调 ── */

static void uu_toggle_cb(lv_event_t *e)
{
    struct usb2ttl_app *uu = lv_event_get_user_data(e);
    if (!uu) return;
    if (app_usb2ttl_get_state() == USB2TTL_ON) {
        app_usb2ttl_disable();
    } else {
        app_usb2ttl_enable();
    }
    uu_update(uu);
}

static void uu_isp_cb(lv_event_t *e)
{
    struct usb2ttl_app *uu = lv_event_get_user_data(e);
    if (!uu) return;
    ESP_LOGI("usb2ttl", "ISP button pressed");
    esp_err_t ret = app_usb2ttl_enter_isp();
    if (ret != ESP_OK) {
        lv_label_set_text(uu->val_hint, "ISP 引脚配置无效，请检查\nBOOT0/RST 设置。");
    }
    uu_update(uu);
}

static void uu_baud_cb(lv_event_t *e)
{
    struct usb2ttl_app *uu = lv_event_get_user_data(e);
    if (!uu) return;
    int cur = app_usb2ttl_get_baud();
    for (int i = 0; i < (int)BAUDS_N; i++) {
        if (s_bauds[i] == cur) {
            app_usb2ttl_set_baud(s_bauds[(i + 1) % BAUDS_N]);
            break;
        }
    }
    uu_update(uu);
}

static void uu_parity_cb(lv_event_t *e)
{
    struct usb2ttl_app *uu = lv_event_get_user_data(e);
    if (!uu) return;
    app_usb2ttl_set_parity_even(!app_usb2ttl_get_parity_even());
    uu_update(uu);
}

/* 引脚编辑：io_picker 确认回调（服务层校验：板面空闲 + 两脚不同） */
static void uu_io_picked(void *ctx, int io)
{
    struct usb2ttl_app *uu = ctx;
    if (!uu || io < 0) return;   /* 取消 */
    int b0, rst;
    app_usb2ttl_get_isp_pins(&b0, &rst);
    esp_err_t ret = (uu->num_opt == 0)
        ? app_usb2ttl_set_isp_pins(io, rst)
        : app_usb2ttl_set_isp_pins(b0, io);
    if (ret != ESP_OK) {
        lv_label_set_text(uu->val_hint, "无效引脚：需为板面空闲脚且与另一\n脚不同，请重新设置。");
    }
    uu_update(uu);
}

static void uu_pin_cb(lv_event_t *e)
{
    struct usb2ttl_app *uu = lv_event_get_user_data(e);
    if (!uu) return;
    /* 引脚序号（0=BOOT0, 1=RST）在按钮自身 user_data（ud 已是 uu） */
    uu->num_opt = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    io_picker_show(uu->root, IO_CAPS_ANY, uu_io_picked, uu);
}

static void uu_timer_cb(lv_timer_t *t)
{
    uu_update(lv_timer_get_user_data(t));
}

/* 自动下载勾选框回调 */
static void uu_auto_cb(lv_event_t *e)
{
    struct usb2ttl_app *uu = lv_event_get_user_data(e);
    if (!uu) return;
    bool checked = lv_obj_has_state(uu->chk_auto, LV_STATE_CHECKED);
    app_usb2ttl_set_auto_isp(checked);
    uu_update(uu);
}

/* ── 行构建 ── */

/* 纯信息行：key / value 两行（同 card_reader） */
static lv_obj_t *uu_row_text(lv_obj_t *parent, const char *key, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UU_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_pad_gap(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, UU_DIM, 0);
    lv_obj_set_style_text_font(k, uu_font(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    lv_obj_set_style_text_color(v, UU_ACCENT, 0);
    lv_obj_set_style_text_font(v, uu_font(), 0);
    if (val_out) *val_out = v;
    return row;
}

/* 可编辑行：key 标签 + 全宽按钮（按钮文字 = 当前值） */
static void uu_row_btn(lv_obj_t *parent, const char *key, lv_event_cb_t cb,
                       void *ud, lv_obj_t **btn_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, UU_DIM, 0);
    lv_obj_set_style_text_font(k, uu_font(), 0);

    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, lv_pct(100), 30);
    lv_obj_set_style_bg_color(b, UU_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, UU_ACCENT, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_text_color(b, UU_TEXT, 0);
    lv_obj_set_style_text_font(b, uu_font(), 0);
    lv_obj_set_style_bg_color(b, UU_DIM, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, UU_DIM, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_set_user_data(b, ud);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_center(l);
    if (btn_out) *btn_out = b;
}

/* ── 构建 ── */

usb2ttl_app_t *usb2ttl_create(lv_obj_t *parent, usb2ttl_back_cb_t back_cb, void *ctx)
{
    usb2ttl_app_t *uu = lv_malloc(sizeof(usb2ttl_app_t));
    if (!uu) return NULL;
    lv_memzero(uu, sizeof(*uu));
    uu->back_cb = back_cb;
    uu->back_ctx = ctx;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, UU_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    uu->root = root;

    /* 标题 */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "USB2TTL + ISP");
    lv_obj_set_pos(title, 10, 8);
    lv_obj_set_style_text_color(title, UU_ACCENT, 0);
    lv_obj_set_style_text_font(title, uu_font(), 0);

    /* 信息区（flex column，可滚动） */
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_t *info = lv_obj_create(root);
    lv_obj_set_pos(info, 8, 36);
    lv_obj_set_size(info, lv_pct(100) - 16, sh - 36 - 8 - 56);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_radius(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_gap(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(info, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);

    uu_row_text(info, "USB 状态", &uu->val_state);
    uu_row_text(info, "PC 连接", &uu->val_pc);

    uu_row_btn(info, "波特率", uu_baud_cb, uu, &uu->btn_baud);
    uu_row_btn(info, "校验", uu_parity_cb, uu, &uu->btn_parity);
    /* 引脚行：事件 user_data = uu（上下文）；引脚序号放在按钮自身 user_data */
    uu_row_btn(info, "BOOT0 引脚", uu_pin_cb, uu, &uu->btn_boot0);
    lv_obj_set_user_data(uu->btn_boot0, (void *)(intptr_t)0);
    uu_row_btn(info, "RST 引脚", uu_pin_cb, uu, &uu->btn_rst);
    lv_obj_set_user_data(uu->btn_rst, (void *)(intptr_t)1);

    /* 自动下载勾选框（默认关）：PC 经 SetCommState 控制 DTR/RTS 触发 ISP */
    lv_obj_t *auto_row = lv_obj_create(info);
    lv_obj_set_size(auto_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(auto_row, UU_CARD, 0);
    lv_obj_set_style_bg_opa(auto_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(auto_row, 0, 0);
    lv_obj_set_style_radius(auto_row, 6, 0);
    lv_obj_set_style_pad_hor(auto_row, 12, 0);
    lv_obj_set_style_pad_ver(auto_row, 4, 0);
    lv_obj_clear_flag(auto_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(auto_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(auto_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *auto_k = lv_label_create(auto_row);
    lv_label_set_text(auto_k, "自动下载");
    lv_obj_set_style_text_color(auto_k, UU_DIM, 0);
    lv_obj_set_style_text_font(auto_k, uu_font(), 0);

    uu->chk_auto = lv_checkbox_create(auto_row);
    lv_checkbox_set_text(uu->chk_auto, "DTR→BOOT0  RTS→RST");
    lv_obj_set_style_text_font(uu->chk_auto, uu_font(), 0);
    lv_obj_set_style_text_color(uu->chk_auto, UU_TEXT, 0);
    lv_obj_add_event_cb(uu->chk_auto, uu_auto_cb, LV_EVENT_VALUE_CHANGED, uu);

    uu_row_text(info, "提示", &uu->val_hint);

    /* 底部按钮：开启/关闭 + 进入 ISP */
    lv_obj_t *btns = lv_obj_create(root);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(btns, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_radius(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 0, 8);
    lv_obj_set_style_pad_column(btns, 10, 0);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    uu->btn_toggle = lv_button_create(btns);
    lv_obj_set_size(uu->btn_toggle, lv_pct(55), 36);
    lv_obj_set_style_bg_color(uu->btn_toggle, UU_CARD, 0);
    lv_obj_set_style_bg_opa(uu->btn_toggle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(uu->btn_toggle, UU_ACCENT, 0);
    lv_obj_set_style_border_width(uu->btn_toggle, 1, 0);
    lv_obj_set_style_radius(uu->btn_toggle, 8, 0);
    lv_obj_set_style_text_color(uu->btn_toggle, UU_TEXT, 0);
    lv_obj_set_style_text_font(uu->btn_toggle, uu_font(), 0);
    lv_obj_set_style_bg_color(uu->btn_toggle, UU_DIM, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(uu->btn_toggle, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_add_event_cb(uu->btn_toggle, uu_toggle_cb, LV_EVENT_CLICKED, uu);
    uu->lbl_toggle = lv_label_create(uu->btn_toggle);
    lv_obj_center(uu->lbl_toggle);

    lv_obj_t *btn_isp = lv_button_create(btns);
    lv_obj_set_size(btn_isp, lv_pct(45), 36);
    lv_obj_set_style_bg_color(btn_isp, UU_CARD, 0);
    lv_obj_set_style_bg_opa(btn_isp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_isp, UU_ERR, 0);
    lv_obj_set_style_border_width(btn_isp, 1, 0);
    lv_obj_set_style_radius(btn_isp, 8, 0);
    lv_obj_set_style_text_color(btn_isp, UU_ERR, 0);
    lv_obj_set_style_text_font(btn_isp, uu_font(), 0);
    lv_obj_add_event_cb(btn_isp, uu_isp_cb, LV_EVENT_CLICKED, uu);
    lv_obj_t *isp_lbl = lv_label_create(btn_isp);
    lv_label_set_text(isp_lbl, "进入 ISP 模式");
    lv_obj_center(isp_lbl);

    uu_update(uu);

    uu->timer = lv_timer_create(uu_timer_cb, 250, uu);

    return uu;
}

void usb2ttl_destroy(usb2ttl_app_t *uu)
{
    if (!uu) return;
    if (io_picker_active()) io_picker_close_now();   /* 防悬挂回调 */
    /* 退出 APP 自动关闭桥接（恢复 UART1 转发/USJ 控制台） */
    if (app_usb2ttl_get_state() == USB2TTL_ON) {
        ESP_LOGI("usb2ttl", "exit -> disable bridge");
        app_usb2ttl_disable();
    }
    if (uu->timer) lv_timer_delete(uu->timer);
    if (uu->root) {
        lv_obj_add_flag(uu->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(uu->root);
    }
    lv_free(uu);
}

bool usb2ttl_swipe_back(usb2ttl_app_t *uu)
{
    (void)uu;
    if (io_picker_active()) {
        io_picker_cancel();   /* 只关选择器（回调 -1），回 USB2TTL 原界面同位置 */
        return false;
    }
    return true;
}
