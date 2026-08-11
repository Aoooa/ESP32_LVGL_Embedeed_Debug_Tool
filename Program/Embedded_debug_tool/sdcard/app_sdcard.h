#ifndef APP_SDCARD_H
#define APP_SDCARD_H

/* app_sdcard：SD 卡文件浏览（应用层）。
 *
 * 职责：在 SD 卡已挂载（drv_sdcard_init）后，提供目录浏览能力：
 *   列出目录内容（文件/文件夹名、大小、类型）、进入子目录。
 * 当前通过串口日志输出调试；后续接入 UI 后由本层提供数据。
 */

#include "esp_err.h"

/* 启动自检：挂载后列出根目录，并尝试进入第一个文件夹列出内容。
 * 供启动流程/调试调用。 */
esp_err_t app_sdcard_self_test(void);

#endif /* APP_SDCARD_H */
