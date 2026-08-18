#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

/* file_browser：SD 卡目录浏览器界面（LVGL）。
 *
 * 布局（全屏）：
 *   顶部 路径 label（深色底白字）
 *   中部 lv_list（文件夹黄色、文件白色，点击文件夹进入下级）
 *   底部 两个按钮：返回上级 / 返回根目录
 *   点击 .txt → 打开共享阅读组件 reader_view（返回按钮回本浏览器）
 *
 * 依赖：drv_sdcard 已挂载、app_sdcard_list_dir 枚举。
 */

#include "lvgl.h"

/* 返回回调（APP 模式：顶部 ← 按钮，返回桌面；NULL = 不显示返回按钮） */
typedef void (*file_browser_back_cb_t)(void *ctx);

/* 创建目录浏览器（parent 通常为当前 screen）。
 * 根目录默认 /sdcard，可用 file_browser_set_root 修改（创建后、使用前）。 */
lv_obj_t *file_browser_create(lv_obj_t *parent, file_browser_back_cb_t back_cb, void *ctx);

/* 销毁目录浏览器（释放行数据/内部状态后删除对象树）。之后 obj 不可用 */
void file_browser_destroy(lv_obj_t *obj);

/* 设置根目录（如 /sdcard），并立即刷新到根目录 */
void file_browser_set_root(lv_obj_t *obj, const char *root_path);

/* 获取当前路径（内部缓冲，只读） */
const char *file_browser_get_current_path(lv_obj_t *obj);

/* 重新枚举当前目录（SD 挂载完成后调用，刷新列表） */
void file_browser_refresh(lv_obj_t *obj);

/* 屏幕旋转后重排（list 高度/按钮宽按新分辨率） */
void file_browser_relayout(lv_obj_t *obj);

/* 右滑返回手势（launcher 分发调用）：阅读器开→关阅读器回列表；
 * 非根目录→返回上一级（返回 false，留在浏览器）；根目录→返回 true（回桌面）。
 * 须在 LVGL 线程/持锁调用 */
bool file_browser_swipe_back(lv_obj_t *obj);

/* 调试事件（测试模块用）：evt 自定义事件码，内部打印状态供验证。
 * 须在 LVGL 线程/持锁调用 */
void file_browser_debug_event(lv_obj_t *obj, int evt);

#endif /* FILE_BROWSER_H */
