#ifndef READER_APP_H
#define READER_APP_H

/* reader_app：阅读器 APP（apps/reader）。
 *
 * 两种模式（由 launch arg 决定，launcher_app_get_arg() 查询）：
 *   arg=NULL      → 书架模式：扫描 SD 收集 txt 列书名，点书进入阅读页
 *   arg=路径      → 直接打开模式：跳过书架，直接打开指定 txt（file_browser 跳转）
 *
 * 页面：书架页（列表）↔ 阅读页（reader_view 全屏覆盖），back 事件：
 *   阅读页打开 → 关闭阅读页回书架（返回 false，留在 APP）
 *   书架页     → 返回 true（请求关闭 APP，回来源）
 *
 * 线程：创建/回调全部在 LVGL 线程（或持 esp_lv_adapter 锁）。
 */

#include "lvgl.h"

typedef struct reader_app reader_app_t;

/* 返回回调（无 UI 按钮，仅无 SD 卡自动退出时调用） */
typedef void (*reader_app_back_cb_t)(void *ctx);

/* 创建阅读器 APP（arg 已由 launcher 保存，内部查询） */
reader_app_t *reader_app_create(lv_obj_t *parent, reader_app_back_cb_t back_cb, void *ctx);

/* 销毁（释放扫描结果/阅读器后删除对象树）。之后 app 不可用 */
void reader_app_destroy(reader_app_t *app);

/* 右滑返回（launcher 分发）：阅读页开→关阅读页留书架（false）；书架页→true */
bool reader_app_swipe_back(reader_app_t *app);

/* SD 卡就绪后重新扫描书架（挂载延迟场景） */
void reader_app_refresh(reader_app_t *app);

/* 调试事件（测试模块用）：打印内部状态 */
void reader_app_debug_event(reader_app_t *app, int evt);

/* 进入动画完成（launcher 回调）：此时才扫描书架/打开文件（滑入期间只渲染 UI） */
void reader_app_entered(reader_app_t *app);

#endif /* READER_APP_H */
