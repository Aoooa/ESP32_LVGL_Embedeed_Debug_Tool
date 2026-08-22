#ifndef SCOPE_APP_H
#define SCOPE_APP_H

/* scope：示波器 APP（apps/scope）。
 *
 * 后端 scope/drv_scope（官方 adc_continuous DMA，≤83.3kSPS）：
 *   CH1 / CH2 / Dual 通道、软件触发（边沿/电平/预触发 25%）、
 *   测量（频率/周期/Vpp/占空比/脉宽）。
 * UI（240x320 竖屏，深色 + Miku 绿）：
 *   顶栏（通道循环键 + 运行状态点）→ 波形 canvas → 测量栏 → 底栏 4 键
 *   （RUN / TRIG 模式循环 / BASE 采样率循环 / V 触发电平键盘）。
 *   波形 100ms 节流重绘，测量 500ms 节流；键盘弹出时暂停重绘防闪烁；
 *   右滑逐级返回（键盘 → APP）。
 *
 * 线程：创建/回调/定时器在 LVGL 线程；采集在 drv_scope 独立任务。
 */

#include "lvgl.h"

typedef void (*scope_back_cb_t)(void *ctx);

/* 创建示波器 APP（parent 通常为当前 screen） */
lv_obj_t *scope_create(lv_obj_t *parent, scope_back_cb_t back_cb, void *ctx);

/* 销毁（停采集 + 释放 canvas/对象树，三步走防触摸停摆） */
void scope_destroy(lv_obj_t *root);

/* 右滑返回（launcher 分发）：键盘开着先关键盘（false），无则请求关闭（true） */
bool scope_swipe_back(lv_obj_t *root);

/* 旋转事件（launcher 分发）：关闭键盘并按新分辨率重排 */
void scope_rotate(lv_obj_t *root, int deg);

/* 调试事件（测试模块用）：打印当前配置 */
void scope_debug_event(lv_obj_t *root, int evt);

/* 进入动画完成（launcher 回调）：此时才启动采集业务（滑入期间只渲染 UI） */
void scope_entered(lv_obj_t *root);

#endif /* SCOPE_APP_H */
