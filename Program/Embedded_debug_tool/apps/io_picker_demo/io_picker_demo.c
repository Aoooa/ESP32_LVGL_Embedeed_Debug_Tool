/* io_picker_demo.c —— IO 选择模块演示入口：
 * 一个"PICK IO"按钮 → io_picker_show(UART|GPIO) → 显示所选引脚 */

#include "io_picker_demo.h"
#include "io_picker.h"
#include "app_font.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "io_demo";

#define DBG_BG     lv_color_hex(0x000000)
#define DBG_TEXT   lv_color_hex(0xFFFFFF)
#define DBG_DIM    lv_color_hex(0x9CA3AF)
#define DBG_ACCENT lv_color_hex(0x7DD3FC)

struct io_picker_demo {
    lv_obj_t *root;
    lv_obj_t *result_lbl;
};

static lv_font_t *demo_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

static void demo_done(void *ctx, int io)
{
    io_picker_demo_t *demo = ctx;
    if (!demo) return;
    if (io < 0) {
        lv_label_set_text(demo->result_lbl, "cancelled");
    } else {
        lv_label_set_text_fmt(demo->result_lbl, "Selected: IO%d", io);
        ESP_LOGI(TAG, "picked IO%d", io);
    }
}

static void demo_pick_evt(lv_event_t *e)
{
    io_picker_demo_t *demo = lv_event_get_user_data(e);
    if (!demo) return;
    io_picker_show(demo->root, IO_CAP_UART | IO_CAP_GPIO, demo_done, demo);
}

io_picker_demo_t *io_picker_demo_create(lv_obj_t *parent, void (*back_cb)(void *ctx), void *ctx)
{
    (void)back_cb;
    (void)ctx;
    io_picker_demo_t *demo = calloc(1, sizeof(io_picker_demo_t));
    if (!demo) return NULL;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, DBG_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    demo->root = root;

    lv_obj_t *title = lv_label_create(root);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(title, DBG_TEXT, 0);
    lv_obj_set_style_text_font(title, demo_font(), 0);
    lv_label_set_text(title, "IO Picker Demo");

    lv_obj_t *hint = lv_label_create(root);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_text_color(hint, DBG_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_label_set_text(hint, "needs UART / GPIO");

    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F6FB4), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E5F8A), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_center(bl);
    lv_obj_set_style_text_font(bl, demo_font(), 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(bl, "PICK IO");
    lv_obj_add_event_cb(btn, demo_pick_evt, LV_EVENT_CLICKED, demo);

    demo->result_lbl = lv_label_create(root);
    lv_obj_align(demo->result_lbl, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_text_color(demo->result_lbl, DBG_ACCENT, 0);
    lv_obj_set_style_text_font(demo->result_lbl, demo_font(), 0);
    lv_label_set_text(demo->result_lbl, "Selected: -");

    ESP_LOGI(TAG, "created");
    return demo;
}

void io_picker_demo_destroy(io_picker_demo_t *demo)
{
    if (!demo) return;
    if (io_picker_active()) io_picker_close_now();   /* 防悬挂回调访问已释放上下文 */
    if (demo->root) {
        lv_obj_add_flag(demo->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(demo->root);
    }
    free(demo);
}

bool io_picker_demo_swipe_back(io_picker_demo_t *demo)
{
    (void)demo;
    return true;   /* 全屏页：允许跟手拖动返回 */
}