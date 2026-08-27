#ifndef EPUB_H
#define EPUB_H

/* epub —— EPUB 转 TXT 转换模块（apps/epub）。
 * 参考 M5Stack Paper S3 中文电子书实现路线：手写 ZIP 中央目录解析 +
 * zlib inflate + XHTML 实体解码/文本抽取（不解析内嵌图片——主流墨水屏
 * 阅读器均略过正文插图，仅保留文本与章节结构）。
 *
 * 输出：把 EPUB 正文按专辑顺序提取为纯文本（章节间加分隔行），
 * 写到 SD 卡指定 txt 路径（复用 Reader 打开 .txt 的全部阅读能力）。
 *
 * 线程：须在 LVGL 线程或持 esp_lv_adapter 锁调用（SD 与 LCD 共享 SPI）。
 */

#include "esp_err.h"

/* 转换 EPUB → 纯文本文件。
 * src_path  源 .epub 路径
 * dst_path  目标 .txt 路径（须以 .txt 结尾，Reader 直接打开）
 * 返回 ESP_OK / ESP_ERR_*（解码失败、内存不足、无正文等） */
esp_err_t epub_convert(const char *src_path, const char *dst_path);

#endif /* EPUB_H */