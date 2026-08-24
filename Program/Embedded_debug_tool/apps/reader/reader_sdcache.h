#ifndef READER_SDCACHE_H
#define READER_SDCACHE_H

/* reader_sdcache —— TXT 阅读索引的 SD 持久化。
 *
 * 把 reader 全文件扫描得到的块索引（reader_block_t 数组）存到 SD 隐藏目录
 * /sdcard/.reader/<basename>.rdx，首次打开建立后写入；后续打开读回（秒开、
 * 免重扫）。索引依赖折行规则（line_width），读回时校验 file_size 与
 * line_width 均一致才复用，否则视为失效重扫。
 *
 * 均为二进制小段读写，任一步失败即返回失败（调用方回退到扫描建索引）。
 * SD 与 LCD 共享 SPI2 总线：文件操作须在 LVGL 线程或持 esp_lv_adapter 锁，
 * 且操作期间应串行化与 LCD flush（此处不做锁，由调用方保证；本项目约定
 * SD 访问持 esp_lv_adapter_lock）。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reader_index.h"

/* 尝试读回索引。成功填 out_count/out_total_lines 并拷入 blocks（≤cap）返回 true；
 * 无缓存/校验失败/IO 失败返回 false（不修改输出）。 */
bool reader_sdcache_load(const char *txt_path, int line_width, size_t file_size,
                         reader_block_t *blocks, uint32_t cap,
                         uint32_t *out_count, uint32_t *out_total_lines);

/* 写索引（覆盖）。任一步失败静默返回。 */
void reader_sdcache_save(const char *txt_path, int line_width, size_t file_size,
                         const reader_block_t *blocks, uint32_t count,
                         uint32_t total_lines);

#endif /* READER_SDCACHE_H */
