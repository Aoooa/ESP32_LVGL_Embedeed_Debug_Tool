/* num_input.c —— 通用数字输入面板：半透明遮罩 + 底部数字键盘。
 * 布局（4x4）：1 2 3 空 / 4 5 6 ⌫ / 7 8 9 CLR / . 0 空 OK(右下)
 * 无标题（公共组件）；OK 确认 / 点击遮罩取消，结果经回调异步返回。 */

#include "num_input.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define N_TAG "num_input"

#define NI_PANEL_H  200   /* 底部面板高度（值显示 + 键盘） */
#define NI_KEY_H    36    /* 键盘键高 */
#define NI_BUF_MAX  15

typedef struct {
    char buf[NI_BUF_MAX + 1];
    int len;
    bool allow_decimal;
    int min, max;
    num_input_cb_t on_done;
    void *ctx;
    lv_obj_t *modal;
    lv_obj_t *val_lbl;
} num_input_t;

static num_input_t s_ni;

static void ni_update_val(void)
{
    if (s_ni.val_lbl) lv_label_set_text(s_ni.val_lbl, s_ni.buf);
}

static void ni_close(void)
{
    if (s_ni.modal) {
        lv_obj_t *m = s_ni.modal;
        s_ni.modal = NULL;
        s_ni.val_lbl = NULL;
        lv_obj_delete(m);
    }
}

/* 遮罩点击 → 取消（回调 ok=false） */
static void ni_cancel(lv_event_t *e)
{
    (void)e;
    num_input_cancel();
}

bool num_input_is_active(void)
{
    return s_ni.modal != NULL;
}

void num_input_cancel(void)
{
    if (!s_ni.modal) return;
    num_input_cb_t cb = s_ni.on_done;
    void *ctx = s_ni.ctx;
    ni_close();
    if (cb) cb(ctx, false, 0);
}

/* 键盘键回调：k 为 "0".."9" / "." / "bs" / "clr" / "ok"
 * 键值存对象 user_data，从目标对象取 */
static void ni_key(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    const char *k = (const char *)lv_obj_get_user_data(btn);
    if (!k || !s_ni.modal) return;

    if (strcmp(k, "ok") == 0) {
        int v = atoi(s_ni.buf);
        if (v < s_ni.min) v = s_ni.min;
        if (v > s_ni.max) v = s_ni.max;
        num_input_cb_t cb = s_ni.on_done;
        void *ctx = s_ni.ctx;
        ni_close();
        if (cb) cb(ctx, true, v);
        return;
    }
    if (strcmp(k, "bs") == 0) {              /* 删除一位 */
        if (s_ni.len > 0) s_ni.buf[--s_ni.len] = '\0';
    } else if (strcmp(k, "clr") == 0) {      /* 清空 */
        s_ni.len = 0;
        s_ni.buf[0] = '\0';
    } else if (strcmp(k, "neg") == 0) {      /* 负号：只能开关在开头 */
        if (s_ni.buf[0] == '-') {
            memmove(s_ni.buf, s_ni.buf + 1, strlen(s_ni.buf));
            s_ni.len--;
        } else if (s_ni.len < NI_BUF_MAX) {
            memmove(s_ni.buf + 1, s_ni.buf, strlen(s_ni.buf) + 1);
            s_ni.buf[0] = '-';
            s_ni.len++;
        }
    } else if (strcmp(k, ".") == 0) {        /* 小数点（仅一次） */
        if (s_ni.allow_decimal && !strchr(s_ni.buf, '.') && s_ni.len < NI_BUF_MAX) {
            if (s_ni.len == 0) {
                s_ni.buf[0] = '0';
                s_ni.len = 1;
            }
            s_ni.buf[s_ni.len++] = '.';
            s_ni.buf[s_ni.len] = '\0';
        }
    } else {                                 /* 数字 */
        if (s_ni.len < NI_BUF_MAX) {
            s_ni.buf[s_ni.len++] = k[0];
            s_ni.buf[s_ni.len] = '\0';
        }
    }
    ni_update_val();
}

bool num_input_show(lv_obj_t *parent, int initial, int min, int max,
                    bool allow_decimal, num_input_cb_t on_done, void *ctx)
{
    if (!parent || s_ni.modal) return false;

    snprintf(s_ni.buf, sizeof(s_ni.buf), "%d", initial);
    s_ni.len = (int)strlen(s_ni.buf);
    s_ni.allow_decimal = allow_decimal;
    s_ni.min = min;
    s_ni.max = max;
    s_ni.on_done = on_done;
    s_ni.ctx = ctx;

    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_set_size(m, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(m, 0, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_60, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m, ni_cancel, LV_EVENT_CLICKED, NULL);   /* 点外部取消 */
    s_ni.modal = m;

    int sw = lv_display_get_horizontal_resolution(lv_display_get_default());
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());

    /* 底部面板（点击不冒泡到遮罩） */
    lv_obj_t *panel = lv_obj_create(m);
    lv_obj_set_size(panel, sw, NI_PANEL_H);
    lv_obj_set_pos(panel, 0, sh - NI_PANEL_H);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0E141C), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x1F2A36), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(panel, 2, 0);   /* 键盘左右贴边 */
    lv_obj_set_style_pad_gap(panel, 4, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 当前值显示（无标题，公共组件） */
    s_ni.val_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(s_ni.val_lbl, lv_color_hex(0x7FE5DC), 0);
    lv_obj_set_style_text_font(s_ni.val_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_width(s_ni.val_lbl, sw - 12);
    lv_obj_set_style_text_align(s_ni.val_lbl, LV_TEXT_ALIGN_CENTER, 0);
    ni_update_val();

    /* 键盘 4x4：1 2 3 ⌫ / 4 5 6 - / 7 8 9 CLR / . 0 OK(2列贴右) */
    static const char *rows[4][4] = {
        { "1", "2", "3", "bs" },
        { "4", "5", "6", "neg" },
        { "7", "8", "9", "clr" },
        { ".", "0", "ok", "" },
    };
    int kpad = 3;
    int key_h = NI_KEY_H;
    for (int r = 0; r < 4; r++) {
        lv_obj_t *row = lv_obj_create(panel);
        lv_obj_set_size(row, lv_pct(100), key_h);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, kpad, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        for (int c = 0; c < 4; c++) {
            const char *k = rows[r][c];
            if (!k[0]) continue;
            bool wide = (r == 3 && c == 2);   /* 最后一行 OK 占 2 键宽 */
            lv_obj_t *b = lv_button_create(row);
            lv_obj_set_flex_grow(b, wide ? 2 : 1);   /* 弹性铺满：左右精确贴边 */
            lv_obj_set_size(b, 0, key_h);
            lv_obj_set_style_radius(b, 6, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            if (strcmp(k, "ok") == 0) {
                lv_obj_set_style_bg_color(b, lv_color_hex(0x39C5BB), 0);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            } else if (strcmp(k, "bs") == 0 || strcmp(k, "clr") == 0
                       || strcmp(k, "neg") == 0) {
                lv_obj_set_style_bg_color(b, lv_color_hex(0x1F2A36), 0);
            } else {
                lv_obj_set_style_bg_color(b, lv_color_hex(0x121A24), 0);
            }
            lv_obj_t *lbl = lv_label_create(b);
            if (strcmp(k, "bs") == 0) lv_label_set_text(lbl, "DEL");   /* 用 ASCII，避免 FontAwesome 缺字形 */
            else if (strcmp(k, "clr") == 0) lv_label_set_text(lbl, "CLR");
            else if (strcmp(k, "neg") == 0) lv_label_set_text(lbl, "-");
            else if (strcmp(k, "ok") == 0) lv_label_set_text(lbl, "OK");
            else lv_label_set_text(lbl, k);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(lbl,
                strcmp(k, "ok") == 0 ? lv_color_hex(0x062018) : lv_color_hex(0xE6F0EE), 0);
            /* 键内文字/符号绝对居中（对齐按钮中心，不受按钮 padding 影响） */
            lv_obj_center(lbl);
            lv_obj_set_user_data(b, (void *)k);
            lv_obj_add_event_cb(b, ni_key, LV_EVENT_CLICKED, NULL);
        }
    }
    return true;
}
