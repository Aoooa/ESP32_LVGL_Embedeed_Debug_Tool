#ifndef APP_FONT_H
#define APP_FONT_H

/* app_font：中文字体管理器（编译进固件的 C 数组字体，零运行时加载）。
 *
 * 字体由 lv_font_conv --format lvgl 从 simhei.ttf 生成（fonts/han_sc_16.c），
 * 覆盖 ASCII + 全角标点 + CJK 基本区 20902 字，直接编译进固件 flash。
 * 相比 SD 卡 bin 方案：无文件 IO、无启动延迟、无内存大块分配，启动即用。
 */

#include "lvgl.h"

/* 获取指定字号中文字体（当前仅 16px，其他字号返回 NULL，调用方回退英文内置字体） */
lv_font_t *app_font_get(int size);

/* 保留 API（SD 挂载后调用）：C 数组方案无需重试，函数保留为空实现 */
void app_font_retry(void);

#endif /* APP_FONT_H */
