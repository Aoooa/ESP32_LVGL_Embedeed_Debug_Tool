/* speed_wheel.c —— 通用调速器组件（LVGL 9）
 *
 * 结构：触摸热区容器（透明，接收事件）+ 胶囊外框（半透明灰边框）+ 滑块
 * （深灰圆钮）。拖动滑块 → 回调 pos（-1..1）；松手滑块弹性回中。
 * 事件只绑定热区容器（避免小滑块难命中），滑块为纯视觉子对象。
 */

#include "speed_wheel.h"
#include "esp_log.h"
#include <stdlib.h>

#define SW_W          36   /* 组件总宽（含触摸热区外扩） */
#define SW_SHELL_W    14   /* 胶囊外框视觉宽 */
#define SW_SHELL_B    2    /* 外框边框厚 */
#define SW_KNOB_W     12   /* 滑块直径 */
#define SW_KNOB_GAP   3    /* 滑块与行程端点间隙 */
#define SW_RETURN_MS  160  /* 松手回中动画时长 */

/* 样式色（半透明灰 + 深灰） */
#define SW_FRAME_COLOR  0x8A8A8A   /* 外框灰（半透明渲染） */
#define SW_KNOB_COLOR   0x3A3A3A   /* 滑块深灰 */

typedef struct {
    lv_obj_t *root;        /* 热区容器（事件载体） */
    lv_obj_t *shell;       /* 胶囊外框（纯视觉） */
    lv_obj_t *knob;        /* 滑块（纯视觉） */
    speed_wheel_cb_t cb;
    void *ctx;
    int travel;            /* 滑块行程（px） */
    int cy;                /* 滑块中心 y（相对组件顶部） */
    bool pressed;
    int32_t anim_val;      /* 回中动画变量 */
} sw_t;

static sw_t *sw_get(lv_obj_t *obj)
{
    return lv_obj_get_user_data(obj);
}

static void sw_apply(sw_t *w)
{
    int knob_y = SW_KNOB_GAP + w->travel + (int)(w->anim_val / 1000.0f * w->travel);
    lv_obj_set_y(w->knob, knob_y);
}

static void sw_anim_exec(void *var, int32_t v)
{
    sw_t *w = var;
    w->anim_val = v;
    sw_apply(w);
}

static void sw_on_event(lv_event_t *e)
{
    sw_t *w = sw_get(lv_event_get_current_target(e));
    if (!w) return;

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: {
        w->pressed = true;
        lv_anim_delete(&w->anim_val, sw_anim_exec);
        break;
    }
    case LV_EVENT_PRESSING: {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        /* 全局坐标 → 组件内坐标（须减组件自身及父链 y 偏移，组件不在 (0,0) 时也准） */
        lv_obj_t *par = lv_obj_get_parent(w->root);
        lv_coord_t py = p.y - lv_obj_get_y(w->root) - lv_obj_get_y(par);
        float pos = (float)(py - w->cy) / w->travel;
        if (pos > 1.0f) pos = 1.0f;
        if (pos < -1.0f) pos = -1.0f;
        w->anim_val = (int32_t)(pos * 1000.0f);
        sw_apply(w);
        if (w->cb) w->cb(w->ctx, pos);
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_INDEV_RESET: {
        w->pressed = false;
        if (w->cb) w->cb(w->ctx, 0.0f);   /* 归零停止 */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &w->anim_val);
        lv_anim_set_exec_cb(&a, sw_anim_exec);
        lv_anim_set_values(&a, w->anim_val, 0);
        lv_anim_set_duration(&a, SW_RETURN_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        break;
    }
    default:
        break;
    }
}

lv_obj_t *speed_wheel_create(lv_obj_t *parent, int h_px,
                             speed_wheel_cb_t cb, void *ctx)
{
    sw_t *w = lv_malloc(sizeof(sw_t));
    if (!w) return NULL;
    lv_memzero(w, sizeof(*w));
    if (h_px <= 0) h_px = 100;   /* 防御：非法高度用默认 */
    w->cb = cb;
    w->ctx = ctx;
    w->travel = h_px / 2 - SW_KNOB_W / 2 - SW_KNOB_GAP;
    if (w->travel < 12) w->travel = 12;
    w->cy = h_px / 2;
    w->anim_val = 0;

    /* 热区容器（事件载体，全透明可点） */
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_size(root, SW_W, h_px);
    lv_obj_set_user_data(root, w);
    lv_obj_add_event_cb(root, sw_on_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, sw_on_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, sw_on_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, sw_on_event, LV_EVENT_INDEV_RESET, NULL);
    w->root = root;

    /* 胶囊外框（半透明灰：半透明底 + 半透明边框） */
    lv_obj_t *shell = lv_obj_create(root);
    lv_obj_remove_flag(shell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(shell, lv_color_hex(SW_FRAME_COLOR), 0);
    lv_obj_set_style_bg_opa(shell, LV_OPA_20, 0);      /* 半透明灰底 */
    lv_obj_set_style_border_color(shell, lv_color_hex(SW_FRAME_COLOR), 0);
    lv_obj_set_style_border_opa(shell, LV_OPA_50, 0);   /* 半透明灰边框 */
    lv_obj_set_style_border_width(shell, SW_SHELL_B, 0);
    lv_obj_set_style_radius(shell, SW_SHELL_W / 2, 0);
    lv_obj_set_pos(shell, (SW_W - SW_SHELL_W) / 2, SW_KNOB_GAP);
    lv_obj_set_size(shell, SW_SHELL_W, w->travel * 2 + SW_KNOB_W);
    w->shell = shell;

    /* 滑块（深灰圆钮，半透明） */
    lv_obj_t *knob = lv_obj_create(root);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC
                       | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(knob, lv_color_hex(SW_KNOB_COLOR), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_80, 0);        /* 深灰滑块 */
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_radius(knob, SW_KNOB_W / 2, 0);
    lv_obj_set_pos(knob, (SW_W - SW_KNOB_W) / 2, SW_KNOB_GAP);
    lv_obj_set_size(knob, SW_KNOB_W, SW_KNOB_W);
    w->knob = knob;

    sw_apply(w);
    return root;
}

void speed_wheel_set_pos(lv_obj_t *obj, float pos)
{
    sw_t *w = sw_get(obj);
    if (!w) return;
    if (pos > 1.0f) pos = 1.0f;
    if (pos < -1.0f) pos = -1.0f;
    w->anim_val = (int32_t)(pos * 1000.0f);
    sw_apply(w);
}

bool speed_wheel_is_pressed(lv_obj_t *obj)
{
    sw_t *w = sw_get(obj);
    return w ? w->pressed : false;
}
