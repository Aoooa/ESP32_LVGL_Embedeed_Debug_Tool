/* drv_sdcard.c —— SD 卡 SPI 模式挂载（与 LCD 共享 SPI2 总线，分时复用） */

#include "drv_sdcard.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "drv_sdcard";

/* Waveshare ESP32-S3-Touch-LCD-2 SD 卡槽与 LCD 共用 SPI2 总线：
 *   SCLK/MOSI/MISO 与 LCD 相同（39/38/40），仅 CS 独立（41）。
 * 总线由 LCD（drv_display）初始化，此处只挂载 SD 设备，无需重新初始化。 */
#define SD_PIN_CS        41

static sdmmc_card_t *s_card;

esp_err_t drv_sdcard_init(void)
{
    /* SDSPI_HOST_DEFAULT: host=SPI2（与 LCD 共享），频率上限 20MHz */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    ESP_LOGI(TAG, "init: shared SPI%d bus, CS=%d", host.slot + 1, SD_PIN_CS);

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_PIN_CS;
    slot_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* 只读挂载，绝不格式化 */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    ESP_LOGI(TAG, "mounting %s...", DRV_SDCARD_MOUNT_POINT);

    esp_err_t ret = esp_vfs_fat_sdspi_mount(DRV_SDCARD_MOUNT_POINT,
                                            &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed (%s), is the card inserted?", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", DRV_SDCARD_MOUNT_POINT);
    return ESP_OK;
}

void drv_sdcard_deinit(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(DRV_SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
}
