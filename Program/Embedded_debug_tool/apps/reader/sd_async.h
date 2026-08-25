#ifndef SD_ASYNC_H
#define SD_ASYNC_H

/* sd_async —— 阅读器 SD 小文件异步写入（行收藏 / 阅读进度 / 书收藏标志）。
 *
 * 问题：SD 与 LCD 共享 SPI2 总线，写盘须持 esp_lv_adapter_lock；若在 LVGL
 * 线程同步写会阻塞 UI（收藏/返回时卡顿）。
 * 方案：LVGL 线程只入队（快照已拷入 job，立即返回）；后台任务持
 * esp_lv_adapter_lock 顺序写盘。队列满 → 丢弃并返回 false（收藏/进度可丢，
 * 下一次操作会再存，代价可忽略）。
 *
 * 文件约定（/sdcard/.reader/<basename>.xxx）：
 *   .fav      行收藏（文本 "<行>\t<内容>"，见 reader_favcache）
 *   .prog     阅读进度：内容 "done" = 已读到末行（阅读完成）；否则为行号（0 基）
 *   .favbook  书收藏标志（存在即已收藏，空文件）
 */

#include "reader_favcache.h"
#include <stdbool.h>

/* 行收藏整表异步落盘（快照拷贝，走队列） */
bool sd_async_save_fav(const char *txt_path, const reader_fav_list_t *fav);

/* 阅读进度异步保存：line = 0 基行号；done = 已读到末行（阅读完成） */
bool sd_async_save_prog(const char *txt_path, int line, bool done);

/* 书收藏标志异步保存：fav=true 创建 <name>.favbook，false 删除 */
bool sd_async_set_favbook(const char *txt_path, bool fav);

/* ── 同步读取（书架扫描 / 收藏夹列出用；调用方须持 esp_lv_adapter_lock） ── */

/* 读阅读进度：返回 -1 无进度文件；-2 已完成（done）；>=0 行号（0 基） */
int sd_read_prog(const char *txt_path);

/* 书收藏标志是否存在 */
bool sd_favbook_exists(const char *txt_path);

#endif /* SD_ASYNC_H */