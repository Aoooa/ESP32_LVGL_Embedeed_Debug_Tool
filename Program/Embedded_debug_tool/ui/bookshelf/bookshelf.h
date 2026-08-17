#ifndef BOOKSHELF_H
#define BOOKSHELF_H

/* bookshelf：TXT 书架（桌面启动器进入阅读器的入口界面）。
 *
 * 打开时扫描 /sdcard 全部子目录收集 .txt 文件，以书名列表呈现
 * （名称去后缀，右上角显示数量）；点击书名进入共享阅读组件
 * reader_view（返回回书架）；左上角 ← 返回上一级（回桌面）。
 * SD 未挂载时显示"无 SD 卡"提示并自动返回。
 *
 * 线程：创建/刷新须在 LVGL 线程（或持 esp_lv_adapter 锁）。
 */

#include "lvgl.h"

typedef struct bookshelf bookshelf_t;

/* 返回回调（左上角 ← 按钮；无 SD 卡自动退出时也会调用） */
typedef void (*bookshelf_back_cb_t)(void *ctx);

/* 创建书架（parent 通常为当前 screen） */
bookshelf_t *bookshelf_create(lv_obj_t *parent, bookshelf_back_cb_t back_cb, void *ctx);

/* 销毁书架（释放扫描结果/阅读器后删除对象树）。之后 bs 不可用 */
void bookshelf_destroy(bookshelf_t *bs);

/* SD 卡就绪后重新扫描（挂载延迟场景，由上层通知调用） */
void bookshelf_refresh(bookshelf_t *bs);

#endif /* BOOKSHELF_H */
