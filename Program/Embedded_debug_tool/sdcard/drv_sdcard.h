#ifndef DRV_SDCARD_H
#define DRV_SDCARD_H

/* drv_sdcard：SD 卡（SPI 模式）挂载/卸载驱动。
 *
 * 硬件（Waveshare ESP32-S3-Touch-LCD-2 SD 卡槽）：
 *   CS=IO41，MOSI/SCLK/MISO 与 LCD 共用（IO38/39/40），共享 SPI2 总线
 *   （总线由 LCD 初始化，SD 设备分时复用）
 * 挂载点：/sdcard（FAT12/16/32）。 */

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>

#define DRV_SDCARD_MOUNT_POINT "/sdcard"

/* 挂载 SD 卡（共享已初始化的 SPI 总线）。成功后可通过标准 VFS 文件 API
 * （opendir/readdir/fopen 等）访问 /sdcard/ 下的文件。 */
esp_err_t drv_sdcard_init(void);

/* 卸载（总线归属 LCD，不释放）。与卡相关的 sdspi 设备/卡结构一并释放。 */
void drv_sdcard_deinit(void);

/* SD 卡是否已通过 VFS 挂载（drv_sdcard_init 成功） */
bool drv_sdcard_is_mounted(void);

/* ── 原始卡访问（USB 读卡器等场景） ──
 * 初始化 SD 卡但不挂 FatFs（SPI 慢操作，调用方须持 esp_lv_adapter 锁防 LCD 渲染争抢）。
 * 返回的 card 与 VFS 挂载互斥：必须先 drv_sdcard_deinit 释放 VFS，再用完
 * drv_sdcard_card_deinit 后重新 drv_sdcard_init。 */
esp_err_t drv_sdcard_card_init(sdmmc_card_t **card_out);

/* 释放原始卡（card 为 drv_sdcard_card_init 返回值） */
void drv_sdcard_card_deinit(sdmmc_card_t *card);

#endif /* DRV_SDCARD_H */
