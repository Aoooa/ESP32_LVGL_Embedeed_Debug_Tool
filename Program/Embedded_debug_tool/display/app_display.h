#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdbool.h>

/*
 * 显示方向架构（运行时旋转，官方路径：LVGL 逻辑方向 × 硬件 MADCTL 同向切换）：
 *
 * 1. app_display_set_rotation(deg)：0/90/180/270 运行时切换，三件事同步完成：
 *    - lv_display_set_resolution 交换逻辑分辨率（LVGL 自动全屏重绘）
 *    - drv_display_set_hw_rotation 硬件 MADCTL（ST7789 swap/mirror，官方矩阵）
 *    - file_browser_relayout 重排 UI（阅读器重建）
 *    注意：SPI 面板官方不使用 LVGL 软件旋转（lv_display_set_rotation 与
 *    adapter 直写 flush 不兼容），旋转 = 硬件 MADCTL，buffer 注册固定 320 宽，
 *    partial tile 按 area 宽动态 reshape，横竖方向均安全。
 *
 * 2. 触摸：touch_rotated_read 按当前方向（s_orientation_deg）选择 4 方向映射，
 *    映射矩阵与官方 touch_rotation_helper 一致（0° 直连已验证）。
 *
 * 3. 布局：file_browser 用 fb_screen_w/h() 动态尺寸 + file_browser_relayout。
 *
 * 4. 硬件滚动（0x33/0x37）：MADCTL 改变后滚动区失效，旋转后需重新设置。
 */

void app_display_start(void);
void app_display_set_info(const char *ip, int uart1_port, int uart2_port);
void app_display_notify_status(void);
void app_display_notify_sd_ready(void);   /* SD 挂载完成后刷新文件浏览器 */

/* 运行时旋转 0/90/180/270 度（须在 LVGL 线程/持锁上下文调用） */
void app_display_set_rotation(int deg);

/* 阅读器硬件滚动（0x37）：启用后 flush 区域映射到 (y+offset)%height，
 * 0x37 负责硬件平移；注意旋转会改变 0x37 轴方向（MADCTL 变化后需重设滚动区） */
void app_display_hw_scroll_set(int offset, int height);
void app_display_hw_scroll_enable(bool en);

#endif
