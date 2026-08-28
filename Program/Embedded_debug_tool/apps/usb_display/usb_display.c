/* usb_display.c —— USB 副屏 APP UI */
#include "usb_display.h"
#include "app_usbdisp.h"
#include "app_font.h"
#include "esp_log.h"

static const char *TAG = "usb_display";

#define UD_BG       lv_color_hex(0x000000)
#define UD_PANEL    lv_color_hex(0x0A0A0A)
#define UD_TEXT     lv_color_hex(0xE8E8F0)
#define UD_DIM      lv_color_hex(0x6B7280)
#define UD_ACCENT   lv_color_hex(0x39C5BB)
#define UD_ERR      lv_color_hex(0xEF4444)
#define UD_OK       lv_color_hex(0x10B981)

typedef enum { UD_STATE_IDLE = 0, UD_STATE_STREAMING } ud_state_t;

struct usb_display {
    lv_obj_t *root;
    usb_display_back_cb_t back_cb;
    void *back_ctx;
    ud_state_t state;
    lv_obj_t *idle_root;       /* IDLE 状态 UI */
    lv_obj_t *val_state;       /* "未连接" / "已连接" */
    lv_obj_t *val_status;      /* 服务状态 */
    lv_obj_t *val_fps;
    lv_obj_t *val_err;
    lv_obj_t *btn_toggle;
    lv_obj_t *lbl_toggle;
    lv_obj_t *stream_root;     /* STREAMING 状态 UI */
    lv_obj_t *img;             /* lv_image 显示 PC 帧 */
    lv_obj_t *overlay;         /* 半透明左上角 fps 覆盖（可关闭） */
    lv_obj_t *ov_fps;
    lv_obj_t *ov_back;         /* STREAMING 状态左上角退出按钮 */
    lv_obj_t *stream_msg;      /* 等待首帧提示 */
    lv_image_dsc_t img_dsc;    /* RGB565 描述符（指向 app_usbdisp 共享缓冲） */
    lv_timer_t *timer;
};

/* ── 帧回调（LVGL 线程，由 app_usbdisp lv_timer 调度） ── */
static void on_frame(void *ctx, const uint16_t *rgb, uint32_t w, uint32_t h) {
    usb_display_t *ud = (usb_display_t *)ctx;
    if (!ud || ud->state != UD_STATE_STREAMING) return;
    /* 重新指向新帧缓冲（不复制，零拷贝） */
    ud->img_dsc.data = (const uint8_t *)rgb;
    ud->img_dsc.data_size = w * h * 2;
    lv_image_set_src(ud->img, &ud->img_dsc);
    lv_obj_invalidate(ud->img);
    /* 隐藏首帧提示 */
    if (ud->stream_msg && lv_obj_is_visible(ud->stream_msg)) {
        lv_obj_add_flag(ud->stream_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── IDLE 状态 ── */

static lv_font_t *ud_font(void) {
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static lv_obj_t *ud_label(lv_obj_t *parent, const char *txt, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, ud_font(), 0);
    return l;
}

static void ud_row(lv_obj_t *parent, const char *key, lv_obj_t **val_out, lv_color_t val_color) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UD_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 3, 0);
    lv_obj_set_style_pad_gap(row, 1, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, UD_DIM, 0);
    lv_obj_set_style_text_font(k, ud_font(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    lv_obj_set_style_text_color(v, val_color, 0);
    lv_obj_set_style_text_font(v, ud_font(), 0);
    *val_out = v;
}

static void ud_update_idle(usb_display_t *ud) {
    usdisp_state_t st = app_usbdisp_get_state();
    bool pc = app_usbdisp_pc_connected();
    bool active = (st == USDISP_ACTIVE);

    /* 单一清晰状态行 */
    const char *state_txt;
    lv_color_t state_color;
    if (active && pc) {
        state_txt = "● 运行中";
        state_color = UD_OK;
    } else if (active) {
        state_txt = "○ 启动中...";
        state_color = UD_ACCENT;
    } else {
        state_txt = "○ 未启动（点击下方按钮开启）";
        state_color = UD_DIM;
    }
    lv_label_set_text(ud->val_state, state_txt);
    lv_obj_set_style_text_color(ud->val_state, state_color, 0);

    /* 服务状态 + 帧率 */
    lv_label_set_text(ud->val_status, app_usbdisp_state_str(st));
    lv_obj_set_style_text_color(ud->val_status,
                                active ? UD_ACCENT : UD_DIM, 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", app_usbdisp_get_fps());
    lv_label_set_text(ud->val_fps, buf);

    snprintf(buf, sizeof(buf), "%u", (unsigned)app_usbdisp_get_error_count());
    lv_label_set_text(ud->val_err, buf);

    /* 按钮文字：未启用 → "开启副屏"；运行中 → "关闭副屏"（同一个按钮反向） */
    if (active) {
        lv_label_set_text(ud->lbl_toggle, "关闭副屏");
        lv_obj_remove_state(ud->btn_toggle, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(ud->lbl_toggle, "开启副屏");
        lv_obj_remove_state(ud->btn_toggle, LV_STATE_DISABLED);
    }
}

static void ud_timer_cb(lv_timer_t *t) {
    usb_display_t *ud = lv_timer_get_user_data(t);
    if (!ud) return;
    ud_update_idle(ud);
    /* 更新 streaming 状态的 fps */
    if (ud->state == UD_STATE_STREAMING && ud->ov_fps) {
        char buf[16];
        snprintf(buf, sizeof(buf), "FPS %.1f", app_usbdisp_get_fps());
        lv_label_set_text(ud->ov_fps, buf);
    }
}

/* ── 切换到 STREAMING ── */
static void ud_enter_streaming(usb_display_t *ud) {
    if (ud->state == UD_STATE_STREAMING) return;
    ESP_LOGI(TAG, "enter streaming");
    ud->state = UD_STATE_STREAMING;
    lv_obj_add_flag(ud->idle_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ud->stream_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ud->stream_msg, LV_OBJ_FLAG_HIDDEN);
    /* 切到黑底，避免白闪 */
    lv_obj_set_style_bg_color(ud->root, UD_BG, 0);
}

/* ── 切回 IDLE ── */
static void ud_exit_streaming(usb_display_t *ud) {
    if (ud->state == UD_STATE_IDLE) return;
    ESP_LOGI(TAG, "exit streaming");
    ud->state = UD_STATE_IDLE;
    lv_obj_remove_flag(ud->idle_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ud->stream_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ud->stream_msg, LV_OBJ_FLAG_HIDDEN);
    app_usbdisp_disable();
    ud_update_idle(ud);
}

/* ── 切换按钮回调（开/关双向） ── */
static void ud_btn_cb(lv_event_t *e) {
    usb_display_t *ud = lv_event_get_user_data(e);
    if (!ud) return;

    if (app_usbdisp_get_state() == USDISP_ACTIVE) {
        /* 关闭副屏：释放 USB PHY */
        app_usbdisp_disable();
        ESP_LOGI(TAG, "user disable (USB released)");
    } else {
        /* 开启副屏：占用 USB PHY */
        esp_err_t ret = app_usbdisp_enable();
        if (ret == ESP_OK) {
            ud_enter_streaming(ud);
        } else {
            ESP_LOGW(TAG, "enable failed: %s", esp_err_to_name(ret));
        }
    }
    ud_update_idle(ud);
}

/* ── 退出按钮（STREAMING 状态左上角） ── */
static void ud_back_btn_cb(lv_event_t *e) {
    usb_display_t *ud = lv_event_get_user_data(e);
    if (!ud) return;
    if (ud->state == UD_STATE_STREAMING) {
        ud_exit_streaming(ud);
    } else {
        if (ud->back_cb) ud->back_cb(ud->back_ctx);
    }
}

/* ── 创建 ── */
usb_display_t *usb_display_create(lv_obj_t *parent, usb_display_back_cb_t back_cb, void *ctx) {
    usb_display_t *ud = lv_malloc(sizeof(usb_display_t));
    if (!ud) return NULL;
    lv_memzero(ud, sizeof(*ud));
    ud->back_cb = back_cb;
    ud->back_ctx = ctx;
    ud->state = UD_STATE_IDLE;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, UD_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    ud->root = root;

    /* ── IDLE UI：标题 → 按钮（顶部永远可见）→ 状态/提示（可滚） ── */
    lv_obj_t *idle = lv_obj_create(root);
    lv_obj_set_size(idle, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(idle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle, 0, 0);
    lv_obj_set_style_pad_all(idle, 10, 0);
    lv_obj_set_style_pad_gap(idle, 6, 0);
    lv_obj_set_flex_flow(idle, LV_FLEX_FLOW_COLUMN);
    /* 启用滚动：超出屏幕可滑 */
    lv_obj_set_scroll_dir(idle, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(idle, LV_SCROLLBAR_MODE_OFF);
    ud->idle_root = idle;

    /* 标题 */
    lv_obj_t *title = lv_label_create(idle);
    lv_label_set_text(title, "USB 副屏");
    lv_obj_set_style_text_color(title, UD_ACCENT, 0);
    lv_obj_set_style_text_font(title, ud_font(), 0);

    /* 开启按钮（顶部，固定位置，按钮可见性最高优先） */
    lv_obj_t *btn = lv_button_create(idle);
    lv_obj_set_size(btn, lv_pct(100), 42);
    lv_obj_set_style_bg_color(btn, UD_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UD_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_text_color(btn, UD_ACCENT, 0);
    lv_obj_set_style_text_font(btn, ud_font(), 0);
    lv_obj_add_event_cb(btn, ud_btn_cb, LV_EVENT_CLICKED, ud);
    ud->btn_toggle = btn;
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "开启副屏");
    lv_obj_center(lbl);
    ud->lbl_toggle = lbl;

    /* 分隔线 */
    lv_obj_t *sep = lv_obj_create(idle);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, UD_DIM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* 状态行 */
    ud_row(idle, "副屏状态", &ud->val_state, UD_DIM);
    ud_row(idle, "服务状态", &ud->val_status, UD_DIM);
    ud_row(idle, "帧率 (FPS)", &ud->val_fps, UD_TEXT);
    ud_row(idle, "错误数", &ud->val_err, UD_TEXT);

    /* 提示 */
    lv_obj_t *hint = lv_label_create(idle);
    lv_label_set_text(hint,
        "首次使用需安装 xfz1986 驱动。\n"
        "开启副屏会顶替读卡器/DAP/USB2TTL的 USB。\n"
        "关闭后 USB PHY 自动归还。");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, UD_DIM, 0);
    lv_obj_set_style_text_font(hint, ud_font(), 0);

    /* ── STREAMING UI ── */
    lv_obj_t *stream = lv_obj_create(root);
    lv_obj_set_size(stream, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(stream, UD_BG, 0);
    lv_obj_set_style_bg_opa(stream, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stream, 0, 0);
    lv_obj_set_style_radius(stream, 0, 0);
    lv_obj_set_style_pad_all(stream, 0, 0);
    lv_obj_clear_flag(stream, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stream, LV_OBJ_FLAG_HIDDEN);
    ud->stream_root = stream;

    /* 全屏图像 */
    ud->img = lv_image_create(stream);
    lv_obj_set_size(ud->img, lv_pct(100), lv_pct(100));
    lv_obj_align(ud->img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(ud->img, LV_OBJ_FLAG_SCROLLABLE);

    /* img_dsc 初始化（头 16B magic + cf/w/h/stride + data 先 NULL） */
    lv_memset(&ud->img_dsc, 0, sizeof(ud->img_dsc));
    ud->img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ud->img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ud->img_dsc.header.w = 240;
    ud->img_dsc.header.h = 320;
    ud->img_dsc.header.stride = 240 * 2;
    ud->img_dsc.data_size = 240 * 320 * 2;
    /* data 由 app_usbdisp 帧回调设置 */
    lv_image_set_src(ud->img, &ud->img_dsc);

    /* 等待首帧提示 */
    lv_obj_t *msg = lv_label_create(stream);
    lv_label_set_text(msg, "等待 PC 帧...");
    lv_obj_set_style_text_color(msg, UD_DIM, 0);
    lv_obj_set_style_text_font(msg, ud_font(), 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
    ud->stream_msg = msg;

    /* FPS 角标（右上角） */
    lv_obj_t *fps = lv_label_create(stream);
    lv_label_set_text(fps, "FPS --");
    lv_obj_set_style_text_color(fps, UD_TEXT, 0);
    lv_obj_set_style_bg_color(fps, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(fps, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(fps, 2, 0);
    lv_obj_set_style_radius(fps, 3, 0);
    lv_obj_align(fps, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_text_font(fps, ud_font(), 0);
    ud->ov_fps = fps;

    /* 左上角退出按钮 */
    lv_obj_t *bk = lv_label_create(stream);
    lv_label_set_text(bk, LV_SYMBOL_LEFT " 退出");
    lv_obj_set_style_text_color(bk, UD_TEXT, 0);
    lv_obj_set_style_bg_color(bk, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bk, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(bk, 4, 0);
    lv_obj_set_style_radius(bk, 4, 0);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_text_font(bk, ud_font(), 0);
    lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bk, ud_back_btn_cb, LV_EVENT_CLICKED, ud);
    ud->ov_back = bk;

    ud_update_idle(ud);
    ud->timer = lv_timer_create(ud_timer_cb, 250, ud);

    /* 注册帧回调 */
    app_usbdisp_register_frame_cb(on_frame, ud);

    /* 不自动开启：USB 必须用户主动开启才占用 */

    return ud;
}

void usb_display_destroy(usb_display_t *ud) {
    if (!ud) return;
    if (ud->state == UD_STATE_STREAMING) app_usbdisp_disable();
    if (ud->timer) lv_timer_delete(ud->timer);
    app_usbdisp_register_frame_cb(NULL, NULL);
    lv_obj_delete(ud->root);
    lv_free(ud);
}

bool usb_display_swipe_back(usb_display_t *ud) {
    if (!ud) return true;
    if (ud->state == UD_STATE_STREAMING) {
        ud_exit_streaming(ud);
        return false;  /* 拦截，不退出 APP */
    }
    return true;
}

lv_obj_t *usb_display_drag_root(void *app) {
    usb_display_t *ud = (usb_display_t *)app;
    if (!ud) return NULL;
    return ud->state == UD_STATE_STREAMING ? ud->root : NULL;
}
