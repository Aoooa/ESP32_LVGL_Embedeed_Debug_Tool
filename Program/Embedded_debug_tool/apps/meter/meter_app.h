#ifndef METER_APP_H
#define METER_APP_H

/* meter_app：多路电压表 APP「Meter」（apps/meter）。
 *
 * 主界面：滚动通道卡片列表（✕删除 / CH#·GPIO# / ▶开始·⏸暂停 / ~波形开关 /
 * 当前电压 / MAX·MIN）+ 底部"添加通道"；无顶部状态栏，返回靠右滑手势。
 * 点卡片主体直接弹 IO 选择（IO_CAP_ADC1，仅 ADC1 连续采集）。
 * 波形：~ 展开卡片内嵌小 canvas（宽占满、高≈卡 1/3），固定 100kHz、4s 窗口
 * 压缩一屏；V 轴自动跟随窗口 [min,max]，琥珀虚线标注 MAX/MIN。
 * 默认暂停：添加卡片后不采集，按▶才开始（采集集重建，全部暂停停采样）。
 *
 * 线程：创建/回调/波形绘制在 LVGL 线程；采集在 drv_meter 任务。
 * IO 账本：选 IO → io_picker_reserve；删除/换选 → release；destroy 全释放。
 */

#include "lvgl.h"

typedef void (*meter_back_cb_t)(void *ctx);

/* 创建 APP（parent 通常为当前 screen） */
lv_obj_t *meter_create(lv_obj_t *parent, meter_back_cb_t back_cb, void *ctx);

/* 销毁（停采集 + 释放 IO + 删除对象树） */
void meter_destroy(lv_obj_t *root);

/* 返回（launcher 分发）：无内部层级，直接请求关闭（io_picker 由拖动处理） */
bool meter_swipe_back(lv_obj_t *root);

/* 拖动返回目标/滑出收尾（launcher 分发）：IO 选择器激活时只拖选择器 */
lv_obj_t *meter_drag_root(void *app);
void meter_drag_exit(void *app);

/* 旋转：按新分辨率重排（关闭并重建列表） */
void meter_rotate(lv_obj_t *root, int deg);

/* 调试事件：打印各通道状态 */
void meter_debug_event(lv_obj_t *root, int evt);

#endif /* METER_APP_H */