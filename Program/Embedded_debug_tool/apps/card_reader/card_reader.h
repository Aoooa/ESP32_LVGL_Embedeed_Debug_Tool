#ifndef CARD_READER_H
#define CARD_READER_H

/* card_reader：USB 读卡器 APP（全屏）。
 *
 * 将 ESP32 变成"SD 卡读卡器"：开启后 SD 卡经 TinyUSB MSC 暴露给 PC，
 * 电脑上出现一个可读写磁盘。关闭/退出 APP 自动恢复 SD 给文件浏览器/阅读器。
 *
 * 服务：sdcard/app_cardreader（enable/disable/状态机）。
 * 线程：UI 操作在 LVGL 线程；状态轮询用 lv_timer（服务事件来自 TinyUSB 任务）。
 */

#include "lvgl.h"

typedef struct card_reader card_reader_t;

typedef void (*card_reader_back_cb_t)(void *ctx);

/* 创建读卡器 APP（parent 通常为当前 screen） */
card_reader_t *card_reader_create(lv_obj_t *parent, card_reader_back_cb_t back_cb, void *ctx);

/* 销毁（若读卡器仍开启会自动关闭并恢复 /sdcard） */
void card_reader_destroy(card_reader_t *cr);

/* 右滑返回（launcher 分发）：直接请求关闭回桌面（destroy 负责收尾） */
bool card_reader_swipe_back(card_reader_t *cr);

/* SD 就绪事件（launcher 分发）：刷新界面 */
void card_reader_refresh(card_reader_t *cr);

/* 调试事件（测试模块用）：打印内部状态 */
void card_reader_debug_event(card_reader_t *cr, int evt);

#endif /* CARD_READER_H */
