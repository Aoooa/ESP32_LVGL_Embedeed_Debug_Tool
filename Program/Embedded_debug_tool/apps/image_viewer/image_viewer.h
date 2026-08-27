#ifndef IMAGE_VIEWER_H
#define IMAGE_VIEWER_H

/* image_viewer —— 图片浏览器 APP（apps/image_viewer）。
 *
 * 两种模式：
 *  - 浏览模式（默认）：递归扫描 SD 卡图片（.jpg/.jpeg/.png/.bmp），
 *    按修改时间降序（最新在前），缩略图网格分页显示；底部页码栏
 *    （◀ / "页 N/M" / ▶），点击缩略图进入查看模式。
 *  - 查看模式：全屏显示单图（contain 适配），右滑/按钮返回浏览模式。
 *
 * 带参启动：file_browser 点图片文件 → launcher_app_launch 传路径 →
 * 直接进入该图的查看模式（返回后仍是浏览模式，已扫描列表保留）。
 *
 * 解码：img_decode（JPG 硬件缩放 / PNG 逐行 / BMP）。
 * 线程：扫描/解码在 LVGL 线程（持 esp_lv_adapter 锁，SD 与 LCD 共享 SPI）。
 *       大图解码可能耗时 → 查看模式用同步单发（≤几十 ms @240x320），
 *       浏览缩略图按页懒解码。
 */

#include "lvgl.h"

typedef void (*image_viewer_back_cb_t)(void *ctx);

/* 创建（parent 通常为当前 screen） */
lv_obj_t *image_viewer_create(lv_obj_t *parent, image_viewer_back_cb_t back_cb, void *ctx);

/* 销毁 */
void image_viewer_destroy(lv_obj_t *root);

/* 右滑返回（launcher 分发）：查看模式 → 图库回浏览（true 放行拖动），浏览模式 → 关闭 APP（true） */
bool image_viewer_swipe_back(lv_obj_t *root);

/* 拖动返回目标/滑出收尾（launcher 分发）：查看模式拖 view 覆盖层露出相册，
 * 滑出关覆盖层回相册；浏览模式整 root，滑出关闭 APP */
lv_obj_t *image_viewer_drag_root(void *app);
void image_viewer_drag_exit(void *app);

/* 旋转：重排 */
void image_viewer_rotate(lv_obj_t *root, int deg);

/* 调试事件 */
void image_viewer_debug_event(lv_obj_t *root, int evt);

#endif /* IMAGE_VIEWER_H */