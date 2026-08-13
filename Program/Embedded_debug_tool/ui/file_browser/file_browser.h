#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

/* file_browser：SD 卡目录浏览器界面（LVGL）。
 *
 * 布局（全屏）：
 *   顶部 路径 label（深色底白字）
 *   中部 lv_list（文件夹黄色、文件白色，点击文件夹进入下级）
 *   底部 两个按钮：返回上级 / 返回根目录
 *
 * 依赖：drv_sdcard 已挂载、app_sdcard_list_dir 枚举。
 * 不打开文件：点击文件项仅无操作（供后续扩展选中/预览）。
 */

#include "lvgl.h"

/* 创建目录浏览器（parent 通常为当前 screen）。
 * 根目录默认 /sdcard，可用 file_browser_set_root 修改（创建后、使用前）。 */
lv_obj_t *file_browser_create(lv_obj_t *parent);

/* 设置根目录（如 /sdcard），并立即刷新到根目录 */
void file_browser_set_root(lv_obj_t *obj, const char *root_path);

/* 获取当前路径（内部缓冲，只读） */
const char *file_browser_get_current_path(lv_obj_t *obj);

/* 重新枚举当前目录（SD 挂载完成后调用，刷新列表） */
void file_browser_refresh(lv_obj_t *obj);

/* 屏幕旋转后重排（list 高度/按钮宽按新分辨率；阅读器打开时重建） */
void file_browser_relayout(lv_obj_t *obj);

#endif /* FILE_BROWSER_H */
