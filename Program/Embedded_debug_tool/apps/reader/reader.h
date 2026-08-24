#ifndef READER_H
#define READER_H

/* reader：大文件 TXT 按需阅读数据层。
 *
 * 打开文件后由后台任务全文件扫描建立块索引（{文件偏移, 起始行号}，
 * 行号按渲染折行规则解析，进度可查）；索引完成后，通过
 * flow_view 的 line provider 回调按需读取：渲染取行时若块不在缓存，
 * 同步从 SD 读入固定槽位（LVGL 线程内短读），未命中/读失败返回占位行。
 *
 * 内存：块索引 ~8B/1KB 文件 + 固定 8 槽 × ~125KB 行缓冲（PSRAM）。
 * 文件大小仅受 PSRAM 中索引空间限制（100MB 文件 ≈ 800KB 索引）。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct reader reader_t;

/* 打开文件并启动后台索引。line_width = 折行像素宽（视口宽 - 边距）。
 * 失败（打不开/文件为空/内存不足）返回 NULL。 */
reader_t *reader_open(const char *path, int line_width, const lv_font_t *font);

/* 关闭：取消后台索引任务（等待其退出）并释放全部资源 */
void reader_close(reader_t *r);

/* 索引状态：is_indexing=false 后可正常取行；progress 0..100 */
bool reader_is_indexing(const reader_t *r);
int  reader_progress(const reader_t *r);

/* 索引完成后的总渲染行数（索引中返回 0） */
int  reader_total_lines(const reader_t *r);

/* 取第 row 行文本（同 provider 语义：未完成/越界返回 NULL）。
 * 行缓存可能被后续读取覆盖，返回指针仅当次有效。用于收藏快照等。 */
const char *reader_line_at(const reader_t *r, int row);

/* 折行像素宽（旋转重建判断用） */
int  reader_line_width(const reader_t *r);

/* ── flow_view line provider 回调（LVGL 线程内调用） ── */

/* 当前已知总行数 */
int  reader_count(void *ctx);

/* 返回第 row 行文本；缓存未命中时同步读块。返回 NULL 表示该行
 * 不可读（索引未完成/超出范围/IO 失败），渲染层显示占位符。 */
const char *reader_line(void *ctx, int row, uint8_t *style);

#endif /* READER_H */
