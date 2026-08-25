/* sd_async.c —— 异步 SD 小文件写入（见 sd_async.h） */

#include "sd_async.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "sd_async";

#define SD_ASYNC_DIR    "/sdcard/.reader"
#define SD_Q_LEN        4

typedef enum {
    SD_OP_FAV_SAVE,
    SD_OP_PROG_SAVE,
    SD_OP_FAVBOOK_SET,
} sd_op_t;

typedef struct {
    sd_op_t op;
    char path[128];
    int line;
    bool done;
    bool fav_book;
    reader_fav_list_t fav;   /* 仅 FAV_SAVE 用（~8KB PSRAM） */
} sd_job_t;

static QueueHandle_t s_q;
static bool s_started;

/* 完整路径 → 文件名（FNV-1a 32）：不同目录同名 txt 互不干扰
 * （.prog/.favbook 不能用 basename，否则同名书共用同一文件而串数据） */
static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void sd_write_prog(const char *path, int line, bool done)
{
    mkdir(SD_ASYNC_DIR, 0777);
    char fp[180];
    snprintf(fp, sizeof(fp), SD_ASYNC_DIR "/%08X.prog", (unsigned)path_hash(path));
    FILE *f = fopen(fp, "w");
    if (!f) {
        ESP_LOGW(TAG, "prog: open failed %s", fp);
        return;
    }
    if (done) fprintf(f, "done\n");
    else fprintf(f, "%d\n", line);
    fclose(f);
}

static void sd_write_favbook(const char *path, bool fav)
{
    mkdir(SD_ASYNC_DIR, 0777);
    char fp[180];
    snprintf(fp, sizeof(fp), SD_ASYNC_DIR "/%08X.favbook", (unsigned)path_hash(path));
    if (fav) {
        FILE *f = fopen(fp, "w");
        if (f) fclose(f);
    } else {
        remove(fp);
    }
}

static void sd_task(void *arg)
{
    (void)arg;
    for (;;) {
        sd_job_t *j = NULL;
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE || !j) continue;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            switch (j->op) {
            case SD_OP_FAV_SAVE:
                reader_fav_save(j->path, &j->fav);
                break;
            case SD_OP_PROG_SAVE:
                sd_write_prog(j->path, j->line, j->done);
                break;
            case SD_OP_FAVBOOK_SET:
                sd_write_favbook(j->path, j->fav_book);
                break;
            }
            esp_lv_adapter_unlock();
        }
        heap_caps_free(j);
    }
}

static bool sd_enqueue(sd_op_t op, const char *path, int line, bool done,
                       bool fav_book, const reader_fav_list_t *fav)
{
    if (!path) return false;
    if (!s_started) {
        s_q = xQueueCreate(SD_Q_LEN, sizeof(sd_job_t *));
        if (!s_q) return false;
        if (xTaskCreateWithCaps(sd_task, "sd_async", 4096, NULL, 3, NULL,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            vQueueDelete(s_q);
            s_q = NULL;
            return false;
        }
        s_started = true;
    }
    sd_job_t *j = heap_caps_calloc(1, sizeof(sd_job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j) return false;
    j->op = op;
    strncpy(j->path, path, sizeof(j->path) - 1);
    j->path[sizeof(j->path) - 1] = '\0';
    j->line = line;
    j->done = done;
    j->fav_book = fav_book;
    if (fav) j->fav = *fav;
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        heap_caps_free(j);
        return false;   /* 队列满：丢弃（下次操作会再存） */
    }
    return true;
}

bool sd_async_save_fav(const char *path, const reader_fav_list_t *fav)
{
    return sd_enqueue(SD_OP_FAV_SAVE, path, 0, false, false, fav);
}

bool sd_async_save_prog(const char *path, int line, bool done)
{
    return sd_enqueue(SD_OP_PROG_SAVE, path, line, done, false, NULL);
}

bool sd_async_set_favbook(const char *path, bool fav)
{
    return sd_enqueue(SD_OP_FAVBOOK_SET, path, 0, false, fav, NULL);
}

int sd_read_prog(const char *path)
{
    char fp[180];
    snprintf(fp, sizeof(fp), SD_ASYNC_DIR "/%08X.prog", (unsigned)path_hash(path));
    FILE *f = fopen(fp, "r");
    if (!f) return -1;
    int line = -1;
    char buf[32];
    if (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strcmp(buf, "done") == 0) line = -2;
        else line = atoi(buf);
    }
    fclose(f);
    return line;
}

bool sd_favbook_exists(const char *path)
{
    char fp[180];
    snprintf(fp, sizeof(fp), SD_ASYNC_DIR "/%08X.favbook", (unsigned)path_hash(path));
    struct stat st;
    return stat(fp, &st) == 0;
}