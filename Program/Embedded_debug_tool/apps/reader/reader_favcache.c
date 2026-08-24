/* reader_favcache.c —— TXT 收藏 SD 持久化（见 reader_favcache.h）
 * 文件格式：纯文本，每行 "<行号>\t<内容>\n"（行号十进制，内容为快照）。 */

#include "reader_favcache.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "reader_favcache";
#define SD_HIDDEN_DIR "/sdcard/.reader"

static void fav_path(const char *txt_path, char *out, size_t outsz)
{
    const char *base = strrchr(txt_path, '/');
    base = base ? base + 1 : txt_path;
    snprintf(out, outsz, SD_HIDDEN_DIR "/%s.fav", base);
}

bool reader_fav_load(const char *txt_path, reader_fav_list_t *list)
{
    if (!list) return false;
    list->count = 0;

    char path[200];
    fav_path(txt_path, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char linebuf[FAV_CONTENT_MAX + 16];
    while (list->count < FAV_MAX_ITEMS && fgets(linebuf, sizeof(linebuf), f)) {
        /* 去掉尾部换行 */
        linebuf[strcspn(linebuf, "\r\n")] = '\0';
        char *tab = strchr(linebuf, '\t');
        if (!tab) continue;
        *tab = '\0';
        int ln = atoi(linebuf);
        const char *content = tab + 1;
        recent_fav_item_t *it = &list->items[list->count];
        it->line = ln;
        strncpy(it->content, content, sizeof(it->content) - 1);
        it->content[sizeof(it->content) - 1] = '\0';
        list->count++;
    }
    fclose(f);
    return list->count > 0;
}

void reader_fav_save(const char *txt_path, const reader_fav_list_t *list)
{
    if (!list) return;
    mkdir(SD_HIDDEN_DIR, 0777);
    char path[200];
    fav_path(txt_path, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "save: open failed %s", path);
        return;
    }
    for (int i = 0; i < list->count; i++) {
        fprintf(f, "%d\t%s\n", list->items[i].line, list->items[i].content);
    }
    fclose(f);
    ESP_LOGI(TAG, "saved %d favs", list->count);
}
