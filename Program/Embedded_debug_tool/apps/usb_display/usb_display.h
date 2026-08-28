/* usb_display.h —— USB 电脑副屏 APP（LVGL 9）
 *
 * 双状态 UI：
 *   IDLE       — 显示 PC 连接状态 + 副屏开关按钮
 *   STREAMING  — 全屏显示 PC 副屏帧（lv_image 指向解码后的 RGB565 缓冲）
 *
 * 状态切换：
 *   IDLE → STREAMING：点击 "开启副屏" → app_usbdisp_enable() → 等待首帧 → 切换
 *   STREAMING → IDLE：左滑 / 点击 "退出副屏" → app_usbdisp_disable() → 切换
 *
 * 帧回调：app_usbdisp 通过 app_usbdisp_register_frame_cb 注入 LVGL 线程新帧
 */
#ifndef USB_DISPLAY_H
#define USB_DISPLAY_H

#include "lvgl.h"

typedef void (*usb_display_back_cb_t)(void *ctx);

typedef struct usb_display usb_display_t;

usb_display_t *usb_display_create(lv_obj_t *parent, usb_display_back_cb_t back_cb, void *ctx);
void usb_display_destroy(usb_display_t *app);
bool usb_display_swipe_back(usb_display_t *app);

/* 拖动返回目标：副屏全屏时拖 root */
lv_obj_t *usb_display_drag_root(void *app);

#endif /* USB_DISPLAY_H */
