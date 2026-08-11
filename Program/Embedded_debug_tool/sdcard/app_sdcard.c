/* app_sdcard.c —— SD 卡文件浏览（串口日志调试版） */

#include "app_sdcard.h"
#include "drv_sdcard.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "app_sdcard";

/* 列出目录内容（非递归）：
 *   [DIR]  name
 *   [FILE] name  (size bytes)
 * 返回目录内条目数。 */
static int sdcard_list_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "opendir failed: %s", path);
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* 跳过 . 和 .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char full[512];
        snprintf(full, sizeof(full), "%.190s/%s", path, ent->d_name);

        if (ent->d_type == DT_DIR) {
            ESP_LOGI(TAG, "[DIR ] %s", full);
        } else {
            struct stat st;
            long size = 0;
            if (stat(full, &st) == 0) size = (long)st.st_size;
            ESP_LOGI(TAG, "[FILE] %s (%ld bytes)", full, size);
        }
        count++;
    }
    closedir(dir);
    return count;
}

/* 找到目录内第一个文件夹并列出其内容（进入子目录） */
static void sdcard_open_first_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_DIR) continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char sub[512];
        snprintf(sub, sizeof(sub), "%.190s/%s", path, ent->d_name);
        ESP_LOGI(TAG, "--- entering %s ---", sub);
        sdcard_list_dir(sub);
        break;   /* 只进第一个 */
    }
    closedir(dir);
}

esp_err_t app_sdcard_self_test(void)
{
    ESP_LOGI(TAG, "=== SD card self test ===");
    ESP_LOGI(TAG, "--- root dir: %s ---", DRV_SDCARD_MOUNT_POINT);
    int n = sdcard_list_dir(DRV_SDCARD_MOUNT_POINT);
    ESP_LOGI(TAG, "root entries: %d", n);
    if (n > 0) {
        sdcard_open_first_dir(DRV_SDCARD_MOUNT_POINT);
    }
    ESP_LOGI(TAG, "=== SD card self test done ===");
    return ESP_OK;
}
