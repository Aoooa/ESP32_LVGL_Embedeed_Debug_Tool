#ifndef LAUNCHER_H
#define LAUNCHER_H

/* launcher：桌面启动器（卡片列表滚筒），基于 LVGL 9.4。
 *
 * 结构：
 *   root（全屏背景，纯色，随主题动画渐变）
 *   └── drum（滚筒容器：可滚动，卡片为刚性整体，1:1 跟手 + 原生惯性）
 *       └── cards[]（圆角边框卡片，纯文字内容，最多 3 行）
 *
 * 布局规则（relayout 按实际分辨率计算）：
 *   - 卡片宽 = 屏幕宽 × 4/5，水平居中（左右与屏幕边框各留 1/10 屏宽间隙）
 *   - 卡片高 = 3 × 行高 + 上下内边距
 *   - 卡片间距 = 卡片高 / 4；滚筒上下边距 = 卡片间距
 *   - 可见卡片数 = (屏高 - 2×间距) / (卡高+间距)，向下取整
 *
 * 交互：手指上下拖动跟手滚动，松手保留原生惯性，无吸附/无缩放动画。
 * 主题：launcher_set_theme 切换黑/白，背景 300ms 渐变，文字色即时反转。
 *
 * 线程：全部交互在 LVGL 线程；launcher_set_theme 可在外部线程调用，
 * 但必须持有 esp_lv_adapter 锁（与 app_display 其余接口一致）。
 */

#include "lvgl.h"

/* 创建启动器（parent 通常为当前 screen；root 铺满 parent） */
lv_obj_t *launcher_create(lv_obj_t *parent);

/* 屏幕旋转/分辨率变化后重排几何（须在 LVGL 线程或持锁调用） */
void launcher_relayout(lv_obj_t *obj);

/* 切换主题：dark=true 黑夜（#0D0D0D 底白字），false 白天（#F5F5F5 底深灰字），
 * 背景 300ms 渐变，绿色边框元素不变 */
void launcher_set_theme(lv_obj_t *obj, bool dark);

#endif /* LAUNCHER_H */
