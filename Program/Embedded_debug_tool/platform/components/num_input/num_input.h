#ifndef NUM_INPUT_H
#define NUM_INPUT_H

/* num_input：通用数字输入面板（LVGL 9，公共组件）。
 *
 * 弹出底部数字键盘（半透明遮罩 + 值显示 + 键盘）：
 *   键：0-9、小数点、回车(OK 右下角)、清空(CLR)、删除(⌫)
 * 点击键盘外区域取消；OK 确认（范围 clamp）。结果经回调异步返回：
 *   on_done(ctx, ok, value) —— ok=true 确认值；ok=false 用户取消。
 *
 * 线程：须在 LVGL 线程或持 esp_lv_adapter 锁调用。
 */

#include "lvgl.h"
#include <stdbool.h>

typedef void (*num_input_cb_t)(void *ctx, bool ok, int value);

/* 显示数字输入面板（无标题，纯键盘）。
 * parent      面板挂载对象（通常为当前屏幕/APP root）
 * initial     初始值（默认 0）
 * min, max    允许范围（确认时 clamp）
 * allow_decimal 是否允许小数点键
 * on_done     结果回调（NULL=仅关闭）
 * ctx         回调上下文
 * 返回 false = 已有面板在显示（拒绝重复打开） */
bool num_input_show(lv_obj_t *parent, int initial, int min, int max,
                    bool allow_decimal, num_input_cb_t on_done, void *ctx);

/* 面板是否正在显示（用于右滑返回等层级判断） */
bool num_input_is_active(void);

/* 取消面板（等同点击遮罩：关闭并回调 ok=false） */
void num_input_cancel(void);

#endif /* NUM_INPUT_H */
