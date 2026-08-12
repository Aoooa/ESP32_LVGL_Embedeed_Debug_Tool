#ifndef APP_SDCARD_H
#define APP_SDCARD_H

/* app_sdcard：SD 卡文件浏览（应用层）。
 *
 * 职责：在 SD 卡已挂载（drv_sdcard_init）后，提供目录枚举能力，
 * 供 UI（file_browser）消费。
 */

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

/* 目录条目回调：枚举时逐项调用（name 不含路径，不含 "." / ".."；
 * mtime 为修改时间（FAT 时间），排序用 */
typedef void (*app_sdcard_dir_cb_t)(void *ctx, const char *name, bool is_dir,
                                    long size, time_t mtime);

/* 枚举目录内容（非递归），逐项回调。返回 ESP_OK 或 ESP_FAIL。 */
esp_err_t app_sdcard_list_dir(const char *path, app_sdcard_dir_cb_t cb, void *ctx);

#endif /* APP_SDCARD_H */
