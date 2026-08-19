/* drv_sdcard.c —— SD 卡 SPI 模式挂载（与 LCD 共享 SPI2 总线，分时复用） */

#include "drv_sdcard.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "driver/sdspi_host.h"
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "drv_sdcard";

/* Waveshare ESP32-S3-Touch-LCD-2 SD 卡槽与 LCD 共用 SPI2 总线：
 *   SCLK/MOSI/MISO 与 LCD 相同（39/38/40），仅 CS 独立（41）。
 * 总线由 LCD（drv_display）初始化，此处只挂载 SD 设备，无需重新初始化。 */
#define SD_PIN_CS        41

static sdmmc_card_t *s_card;      /* VFS 挂载路径持有的卡 */

bool drv_sdcard_is_mounted(void)
{
    return s_card != NULL;
}

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

/* ── 原始卡访问（USB 读卡器）：不挂 FatFs，仅初始化 SPI 设备 + 识别卡 ── */

static sdspi_dev_handle_t s_raw_dev;   /* 原始卡路径的 sdspi 设备（与 VFS 挂载互斥） */

esp_err_t drv_sdcard_card_init(sdmmc_card_t **card_out)
{
    if (!card_out) return ESP_ERR_INVALID_ARG;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_PIN_CS;
    slot_cfg.host_id = host.slot;

    esp_err_t ret = sdspi_host_init_device(&slot_cfg, &s_raw_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        sdspi_host_remove_device(s_raw_dev);
        s_raw_dev = 0;
        return ESP_ERR_NO_MEM;
    }
    host.slot = s_raw_dev;
    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "card init failed: %s", esp_err_to_name(ret));
        free(card);
        sdspi_host_remove_device(s_raw_dev);
        s_raw_dev = 0;
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    *card_out = card;
    return ESP_OK;
}

void drv_sdcard_card_deinit(sdmmc_card_t *card)
{
    if (!card) return;
    if (s_raw_dev) {
        sdspi_host_remove_device(s_raw_dev);
        s_raw_dev = 0;
    }
    free(card);
}
