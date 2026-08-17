#ifndef READER_VIEW_H
#define READER_VIEW_H

/* reader_view：TXT 阅读界面（LVGL 全屏覆盖层，共享组件）。
 *
 * 供 file_browser（文件浏览器点 txt 进入）与 bookshelf（书架点书名进入）
 * 共用同一套阅读 UI：白底黑字 + 顶部状态栏（← 返回 + 文件名）+ 底部
 * 进度条/百分比气泡 + 中央点击切换栏显隐 + 后台索引进度提示。
 *
 * 数据层为 ui/reader（按需加载大文件索引），行渲染经 flow_view provider。
 * 打开/关闭只切换本层显隐，不影响下层界面对象树。
 *
 * 线程：创建/打开/关闭须在 LVGL 线程（或持 esp_lv_adapter 锁）。
 */

#include "lvgl.h"

typedef struct reader_view reader_view_t;

/* 返回按钮回调（阅读器左上角 ←） */
typedef void (*reader_view_back_cb_t)(void *ctx);

/* 创建全屏阅读覆盖层（初始隐藏）。parent 通常为当前 screen */
reader_view_t *reader_view_create(lv_obj_t *parent);

/* 打开 txt 并启动后台索引。失败（打不开/内存不足）返回 false */
bool reader_view_open(reader_view_t *rv, const char *path);

/* 关闭阅读器（释放数据层、隐藏覆盖层） */
void reader_view_close(reader_view_t *rv);

/* 当前是否处于阅读状态 */
bool reader_view_active(const reader_view_t *rv);

/* 设置返回按钮回调（file_browser：关阅读器回列表；bookshelf：回书架） */
void reader_view_set_back_cb(reader_view_t *rv, reader_view_back_cb_t cb, void *ctx);

/* 释放阅读器（删除对象树 + 释放内存）。之后 rv 不可用 */
void reader_view_destroy(reader_view_t *rv);

#endif /* READER_VIEW_H */
