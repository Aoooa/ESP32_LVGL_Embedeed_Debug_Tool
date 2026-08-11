#ifndef DRV_SDCARD_H
#define DRV_SDCARD_H

/* drv_sdcard：SD 卡（SPI 模式）挂载/卸载驱动。
 *
 * 硬件（Waveshare ESP32-S3-Touch-LCD-2 SD 卡槽）：
 *   CS=IO41，MOSI/SCLK/MISO 与 LCD 共用（IO38/39/40），共享 SPI2 总线
 *   （总线由 LCD 初始化，SD 设备分时复用）
 * 挂载点：/sdcard（FAT12/16/32）。 */

#include "esp_err.h"

#define DRV_SDCARD_MOUNT_POINT "/sdcard"

/* 挂载 SD 卡（共享已初始化的 SPI 总线）。成功后可通过标准 VFS 文件 API
 * （opendir/readdir/fopen 等）访问 /sdcard/ 下的文件。 */
esp_err_t drv_sdcard_init(void);

/* 卸载（总线归属 LCD，不释放） */
void drv_sdcard_deinit(void);

#endif /* DRV_SDCARD_H */
