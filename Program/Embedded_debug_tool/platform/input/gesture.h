#ifndef GESTURE_H
#define GESTURE_H

/* gesture —— 输入层（platform/input）：
 * 触摸坐标旋转映射 + 防抖 + 手势检测。
 *
 * 职责：
 *  - 触摸 read_cb（esp_lcd_touch → LVGL 逻辑坐标，按当前方向旋转映射）
 *  - 触摸防抖（时间戳锁存，按下/松开各需持续 50ms 才确认）
 *  - 手势检测：
 *      · 贴边右滑（起点 ≤40px，右移 ≥20px）→ 返回事件（唯一返回手势）
 *      · 全局右滑（任意起点，右移 ≥50px）→ right 事件（仅识别，接口预留）
 *      · 全局左滑（任意起点，左移 ≥50px）→ left 事件（仅识别，接口预留）
 *    （贴边候选右移 >10px 即 wait_release 禁单击）
 *  - 返回/前向事件通过回调注册分发（launcher 注册 → APP 处理），本层不感知 APP
 *
 * 线程：read_cb 在 LVGL 线程（lv_indev_read 内）执行，回调同线程直调。
 */

#include "lvgl.h"
#include "esp_lcd_touch.h"

/* 触摸读回调（esp_lcd_touch custom_touch_read）：坐标旋转映射 + 手势检测。
 * 由显示层（app_display）挂到 esp_lv_adapter touch 配置。 */
esp_err_t gesture_read_cb(esp_lcd_touch_handle_t tp,
                          esp_lcd_touch_point_data_t *points,
                          uint8_t *count, uint8_t max_count, void *user_ctx);

/* 同步当前逻辑方向（0/90/180/270），旋转坐标映射依据 */
void gesture_set_rotation(int deg);

/* 注册返回事件回调（贴边右滑/全局右滑触发时调用；launcher 注册，NULL=不处理）。
 * 回调在 LVGL 线程（read_cb 上下文）执行。 */
typedef void (*gesture_back_cb_t)(void *ctx);
void gesture_set_back_handler(gesture_back_cb_t cb, void *ctx);

/* 注册拖动返回回调（手机式滑动返回：边沿右滑后不立即触发，
 * 持续上报位移让调用方跟随手指平移界面；松手上报最终状态）。
 *   cb(ctx, dx, pressed) —— pressed=true 拖动中（dx=当前累计右移，≥0）；
 *                            pressed=false 松手（dx=最终位移）。
 * 回调在 LVGL 线程（read_cb 上下文）执行。NULL=关闭拖动模式（走立即返回）。 */
typedef void (*gesture_drag_cb_t)(void *ctx, int dx, bool pressed);
void gesture_set_drag_handler(gesture_drag_cb_t cb, void *ctx);

/* 注册全局右滑事件回调（仅识别不触发功能；接口预留，NULL=不处理）。
 * 回调在 LVGL 线程（read_cb 上下文）执行。 */
typedef void (*gesture_right_cb_t)(void *ctx);
void gesture_set_right_handler(gesture_right_cb_t cb, void *ctx);

/* 注册全局左滑事件回调（仅识别不触发功能；接口预留，NULL=不处理）。
 * 回调在 LVGL 线程（read_cb 上下文）执行。 */
typedef void (*gesture_left_cb_t)(void *ctx);
void gesture_set_left_handler(gesture_left_cb_t cb, void *ctx);

/* 全局右/左滑手势总开关（en=false 时仅禁用非贴边全局手势，
 * 贴边右滑返回不受影响）。用于 H 放大平移等需要独占水平拖动的场景。 */
void gesture_set_global_swipe(bool en);

/* ── 边缘垂直/方向手势（全局，APP 订阅响应；仅识别广播，APP 决定处理） ── */

/* 上边缘下滑（起点在顶部 EDGE 区，向下滑 ≥ 阈值）→ fired */
typedef void (*gesture_topdrop_cb_t)(void *ctx);
void gesture_set_topdrop_handler(gesture_topdrop_cb_t cb, void *ctx);

/* 下边缘上滑（起点在底部 EDGE 区，向上滑 ≥ 阈值）→ fired */
typedef void (*gesture_bottomup_cb_t)(void *ctx);
void gesture_set_bottomup_handler(gesture_bottomup_cb_t cb, void *ctx);

/* 当前是否有触摸按下（调试/测试用） */
bool gesture_is_pressed(void);

#endif /* GESTURE_H */
