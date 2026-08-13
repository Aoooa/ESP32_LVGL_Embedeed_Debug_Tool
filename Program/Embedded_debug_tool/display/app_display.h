#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdbool.h>

/* 显示方向（编译期接口）：0 = 竖屏（LVGL 240x320 + 硬件竖屏 swap=false，0x37 轴垂直）
 *                         1 = 横屏（LVGL 320x240 + 硬件横屏 swap=true，0x37 轴水平）
 * 使用 adapter 原 flush（颜色字节序正确）；切方向需重新编译烧录。 */
#ifndef APP_DISPLAY_ORIENTATION
#define APP_DISPLAY_ORIENTATION 0
#endif

/*
 * 显示方向架构（LVGL 逻辑方向 × 硬件方向解耦）：
 *
 * 1. 显示方向（编译期宏 APP_DISPLAY_ORIENTATION）：
 *    0 = 竖屏（LVGL 240x320 + 硬件竖屏 swap=false，0x37 轴垂直）
 *    1 = 横屏（LVGL 320x240 + 硬件横屏 swap=true，0x37 轴水平）
 *    使用 adapter 原 flush（颜色字节序正确）；切方向需重新编译烧录。
 *    说明：adapter 注册时分辨率固定，运行时切换分辨率需自定义 flush
 *    （字节序与 adapter 不一致会反色），故采用编译期配置。
 *
 * 2. 触摸：touch_rotated_read 按当前 LVGL 分辨率自动选择映射（横竖屏通用）。
 *
 * 3. 布局：file_browser 用 fb_screen_w/h() 动态尺寸，横竖屏通用。
 */

void app_display_start(void);
void app_display_set_info(const char *ip, int uart1_port, int uart2_port);
void app_display_notify_status(void);
void app_display_notify_sd_ready(void);   /* SD 挂载完成后刷新文件浏览器 */

/* 阅读器硬件滚动（0x37）：启用后 flush 区域映射到 (y+offset)%height，
 * 0x37 负责硬件平移（轴 = RAM 行 = 屏幕垂直，需组合 A 硬件竖屏） */
void app_display_hw_scroll_set(int offset, int height);
void app_display_hw_scroll_enable(bool en);

#endif
