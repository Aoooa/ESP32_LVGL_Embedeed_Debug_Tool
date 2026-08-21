#ifndef WAVE_GEN_H
#define WAVE_GEN_H

/* wave_gen：波形输出 APP（apps/wave_gen）。
 *
 * 多通道波形发生器（后端 wave/drv_wave，最多 7 通道）：
 *   PWM / SQUARE（LEDC，占定时器）/ SINE（载波+软件调制，占定时器）/
 *   PULSE（GPIO 脉冲，不占定时器，完成后自动停止）
 * 主界面：可滚动通道选项卡列表（✕删除 / IO+模式+参数摘要 / 双色指示灯
 * start-stop）+ 底部"添加通道"；无顶部状态栏，返回靠右滑手势。
 * 设置对话框：左列配置选项（MODE/IO/参数按模式动态，单选高亮），右侧
 * 显示选项内容（模式按钮 / IO 列表 / 数值选择 1Hz 步进），底部说明+实时
 * 示意图（字符画）。
 *
 * 线程：创建/回调在 LVGL 线程；指示灯状态 100ms 轮询驱动同步。
 */

#include "lvgl.h"

typedef void (*wave_gen_back_cb_t)(void *ctx);

/* 创建波形输出 APP（parent 通常为当前 screen） */
lv_obj_t *wave_gen_create(lv_obj_t *parent, wave_gen_back_cb_t back_cb, void *ctx);

/* 销毁（停止全部输出后删除对象树） */
void wave_gen_destroy(lv_obj_t *root);

/* 右滑返回（launcher 分发）：弹窗开着先关弹窗（false），无弹窗请求关闭（true） */
bool wave_gen_swipe_back(lv_obj_t *root);

/* 旋转事件（launcher 分发）：关闭弹窗并按新分辨率重排 */
void wave_gen_rotate(lv_obj_t *root, int deg);

/* 调试事件（测试模块用）：打印各通道状态 */
void wave_gen_debug_event(lv_obj_t *root, int evt);

#endif /* WAVE_GEN_H */
