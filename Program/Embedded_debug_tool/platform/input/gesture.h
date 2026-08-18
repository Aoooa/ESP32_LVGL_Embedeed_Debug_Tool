#ifndef GESTURE_H
#define GESTURE_H

/* gesture —— 输入层（platform/input）：
 * 触摸坐标旋转映射 + 左缘右滑返回手势检测。
 *
 * 职责：
 *  - 触摸 read_cb（esp_lcd_touch → LVGL 逻辑坐标，按当前方向旋转映射）
 *  - 手势检测（贴边起点 ≤15px、候选 dx>25 禁单击 wait_release、触发 dx≥30 发返回事件）
 *  - 返回事件通过回调注册分发（launcher 注册 → APP 处理），本层不感知 APP
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

/* 注册返回事件回调（触发手势时调用；launcher 注册，NULL=不处理）。
 * 回调在 LVGL 线程（read_cb 上下文）执行。 */
typedef void (*gesture_back_cb_t)(void *ctx);
void gesture_set_back_handler(gesture_back_cb_t cb, void *ctx);

/* 当前是否有触摸按下（调试/测试用） */
bool gesture_is_pressed(void);

#endif /* GESTURE_H */
