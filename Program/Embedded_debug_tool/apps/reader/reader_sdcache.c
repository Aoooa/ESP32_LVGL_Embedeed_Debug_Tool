/* reader_sdcache.c —— TXT 索引 SD 持久化（见 reader_sdcache.h） */

#include "reader_sdcache.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/dirent.h>

static const char *TAG = "reader_sdcache";

#define SD_HIDDEN_DIR "/sdcard/.reader"
#define IX_MAGIC "RIDX"   /* 文件头魔数 */

/* 小端逐字段读写（结构含 padding，不能直接 memcpy 结构体） */

static void wr_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    fwrite(b, 1, 4, f);
}

static bool rd_u32(FILE *f, uint32_t *out)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

/* txt 路径 → 索引文件路径（/sdcard/.reader/<basename>.rdx） */
static void ix_path(const char *txt_path, char *out, size_t outsz)
{
    const char *base = strrchr(txt_path, '/');
    base = base ? base + 1 : txt_path;
    snprintf(out, outsz, SD_HIDDEN_DIR "/%s.rdx", base);
}

void reader_sdcache_save(const char *txt_path, int line_width, size_t file_size,
                         const reader_block_t *blocks, uint32_t count,
                         uint32_t total_lines)
{
    /* 确保隐藏目录存在（无则创建） */
    mkdir(SD_HIDDEN_DIR, 0777);

    char path[200];
    ix_path(txt_path, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "save: open failed %s", path);
        return;
    }
    fwrite(IX_MAGIC, 1, 4, f);
    wr_u32(f, (uint32_t)file_size);
    wr_u32(f, (uint32_t)line_width);
    wr_u32(f, count);
    wr_u32(f, total_lines);
    for (uint32_t i = 0; i < count; i++) {
        wr_u32(f, blocks[i].offset);
        wr_u32(f, blocks[i].first_line);
    }
    fclose(f);
    ESP_LOGI(TAG, "saved index %u blocks (%u lines), %zu bytes file",
             count, total_lines, file_size);
}

bool reader_sdcache_load(const char *txt_path, int line_width, size_t file_size,
                         reader_block_t *blocks, uint32_t cap,
                         uint32_t *out_count, uint32_t *out_total_lines)
{
    char path[200];
    ix_path(txt_path, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, IX_MAGIC, 4) != 0) {
        fclose(f);
        return false;
    }
    uint32_t fs, lw, cnt, tl;
    if (!rd_u32(f, &fs) || !rd_u32(f, &lw) || !rd_u32(f, &cnt) || !rd_u32(f, &tl)) {
        fclose(f);
        return false;
    }
    /* 校验折行规则（file_size + line_width）必须一致，否则索引不匹配 */
    if (fs != (uint32_t)file_size || lw != (uint32_t)line_width || cnt == 0 || cnt > cap) {
        ESP_LOGW(TAG, "load: mismatch fs=%u/%u lw=%u/%u cnt=%u cap=%u",
                 fs, (uint32_t)file_size, lw, (uint32_t)line_width, cnt, cap);
        fclose(f);
        return false;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t o, fl;
        if (!rd_u32(f, &o) || !rd_u32(f, &fl)) {
            fclose(f);
            return false;
        }
        blocks[i].offset = o;
        blocks[i].first_line = fl;
    }
    fclose(f);
    *out_count = cnt;
    *out_total_lines = tl;
    ESP_LOGI(TAG, "loaded index %u blocks (%u lines)", cnt, tl);
    return true;
}
