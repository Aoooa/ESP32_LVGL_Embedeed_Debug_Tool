#ifndef SPEED_WHEEL_H
#define SPEED_WHEEL_H

/* speed_wheel：通用调速器组件（系统组件，任何界面可加，类似 num_input）。
 *
 * 用法：创建一个竖向调速器（胶囊外框 + 滑块），按住滑块上下拖动 →
 * 持续回调 pos（-1..1，上推为负、下拉为正），松手滑块弹性回中并停止。
 * pos 的用途由使用方决定（滚动速度/倍率/音量等）。
 *
 * 样式：半透明灰色边框 + 深灰色滑块（低调通用，不绑定桌面主题）。
 * 尺寸：高度调用方指定（h_px，小尺寸），宽度内部固定。
 *
 * 线程：须在 LVGL 线程或持 esp_lv_adapter 锁调用。
 */

#include "lvgl.h"
#include <stdbool.h>

typedef void (*speed_wheel_cb_t)(void *ctx, float pos);   /* pos: -1..1，拖动期间持续回调 */

/* 创建调速器。parent 为其父对象；h_px 为组件总高（含触摸热区）。
 * 返回组件根对象（外部用 lv_obj_set_pos/align 定位）。 */
lv_obj_t *speed_wheel_create(lv_obj_t *parent, int h_px,
                             speed_wheel_cb_t cb, void *ctx);

/* 外部强制设置滑块位置（-1..1，可回中复位；通常由组件内部管理，备用） */
void speed_wheel_set_pos(lv_obj_t *obj, float pos);

/* 组件当前是否被按住 */
bool speed_wheel_is_pressed(lv_obj_t *obj);

#endif /* SPEED_WHEEL_H */
