#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include "lvgl.h"

#include <stdbool.h>

/*
 * 显示平台（platform/display）：LVGL 初始化/触摸挂载/旋转/字体/UI 线程。
 *
 * - app_display_start：初始化 LVGL（adapter）、挂载触摸 read_cb（gesture 层）、
 *   建桌面（launcher）、启动 terminal 常驻任务。
 * - app_display_set_rotation(deg)：LVGL 逻辑分辨率 + 硬件 MADCTL 同向切换，
 *   旋转事件路由到 launcher（栈顶 APP rotate 回调）。
 * - app_display_notify_sd_ready：SD 挂载后异步加载字体，通知当前 APP 刷新。
 *
 * APP（终端等）由 launcher 统一管理，本平台不感知。
 */

void app_display_start(void);

/* 运行时旋转 0/90/180/270 度（须在 LVGL 线程/持锁上下文调用） */
void app_display_set_rotation(int deg);

/* 阅读器硬件滚动（0x37）：启用后 flush 区域映射到 (y+offset)%height，
 * 0x37 负责硬件平移；注意旋转会改变 0x37 轴方向（MADCTL 变化后需重设滚动区） */
void app_display_hw_scroll_set(int offset, int height);
void app_display_hw_scroll_enable(bool en);

/* SD 挂载完成后通知（平台加载字体 + 路由到当前 APP refresh） */
void app_display_notify_sd_ready(void);

#endif
