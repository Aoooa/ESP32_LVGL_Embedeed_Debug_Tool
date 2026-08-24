#ifndef READER_APP_H
#define READER_APP_H

/* reader_app：阅读器 APP（apps/reader）。
 *
 * 两种模式（由 launch arg 决定）：
 *   arg=NULL      → 书架模式：扫描 SD 收集 txt 列书名，点书进入阅读页
 *   arg=路径      → 直接打开模式：跳过书架，直接打开指定 txt（file_browser 跳转）
 *
 * 页面：书架页（列表）↔ 阅读页（reader_view 全屏覆盖）。右滑返回语义
 * （用户约定：全屏 UI 跟手滑动，返回目标按入口/栈确定——书架与文件
 * 浏览器都能打开 txt）：
 *   书架模式阅读页 → 跟手滑动，滑出后关阅读层回书架（APP 保留）
 *   direct 模式    → 跟手滑动，滑出后弹栈销毁回 file_browser
 *   书架页         → 跟手滑动，滑出后弹栈销毁回来源/桌面
 *
 * 线程：创建/回调全部在 LVGL 线程（或持 esp_lv_adapter 锁）。
 */

#include "lvgl.h"

typedef struct reader_app reader_app_t;

/* 返回回调（无 UI 按钮，仅无 SD 卡自动退出时调用） */
typedef void (*reader_app_back_cb_t)(void *ctx);

/* 创建阅读器 APP（arg 由 launcher 传入，create 时保存副本） */
reader_app_t *reader_app_create(lv_obj_t *parent, reader_app_back_cb_t back_cb, void *ctx);

/* 销毁（释放扫描结果/阅读器后删除对象树）。之后 app 不可用 */
void reader_app_destroy(reader_app_t *app);

/* 右滑返回（launcher 分发）：全屏 UI 一律允许跟手拖动（返回 true） */
bool reader_app_swipe_back(reader_app_t *app);

/* 拖动返回滑出动画完成（launcher 分发）：按入口决定返回目标
 * （direct 弹栈 / 书架关阅读层复位） */
void reader_app_drag_exit(reader_app_t *app);

/* SD 卡就绪后重新扫描书架（挂载延迟场景） */
void reader_app_refresh(reader_app_t *app);

/* 调试事件（测试模块用）：打印内部状态 */
void reader_app_debug_event(reader_app_t *app, int evt);

/* 进入动画完成（launcher 回调）：此时才扫描书架/打开文件（滑入期间只渲染 UI） */
void reader_app_entered(reader_app_t *app);

#endif /* READER_APP_H */
