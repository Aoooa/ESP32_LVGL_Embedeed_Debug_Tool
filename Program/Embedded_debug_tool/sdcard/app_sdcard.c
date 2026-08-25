/* app_sdcard.c —— SD 卡目录枚举 */

#include "app_sdcard.h"
#include "drv_sdcard.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "app_sdcard";

/* 系统/隐藏项过滤（Windows 卷信息、回收站、点开头隐藏文件） */
static bool sdcard_is_hidden(const char *name)
{
    if (name[0] == '.') return true;
    if (strcmp(name, "System Volume Information") == 0) return true;
    if (strcmp(name, "$RECYCLE.BIN") == 0) return true;
    return false;
}

/* 枚举目录内容（非递归），逐项回调 */
esp_err_t app_sdcard_list_dir(const char *path, app_sdcard_dir_cb_t cb, void *ctx)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "opendir failed: %s", path);
        return ESP_FAIL;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (sdcard_is_hidden(ent->d_name)) continue;
        if (cb) {
            long size = 0;
            time_t mtime = 0;
            char fp[512];
            snprintf(fp, sizeof(fp), "%s/%s", path, ent->d_name);
            struct stat st;
            if (stat(fp, &st) == 0) {   /* 填充 size/mtime（书架排序用） */
                size = (long)st.st_size;
                mtime = st.st_mtime;
            }
            cb(ctx, ent->d_name, ent->d_type == DT_DIR, size, mtime);
        }
    }
    closedir(dir);
    return ESP_OK;
}
