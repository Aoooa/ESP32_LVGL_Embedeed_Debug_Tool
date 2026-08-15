/* reader.c —— 大文件 TXT 按需阅读数据层 */

#include "reader.h"
#include "flow_model.h"
#include "reader_index.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "reader";

/* 块缓存槽数与行容量（行容量 = 锚点块行数上界，见 reader_index.h） */
#define READER_SLOTS      8                 /* 块缓存槽数（8 × 2048×61B ≈ 1MB PSRAM） */
#define READER_SLOT_LINES READER_INDEX_MAX_LINES
#define READER_IO_BLK     16384             /* 索引任务读缓冲 */

/* 文件大小上限：受索引数组内存约束（2KB/块 × 8B；512MB ≈ 2MB PSRAM） */
#define READER_MAX_FILE   (512llu * 1024 * 1024)

/* 槽：一个字节块的解析结果（flow_model 行缓冲） */
typedef struct {
    bool valid;
    uint32_t offset;                         /* 块起始文件偏移（命中判断） */
    flow_model_t model;
    char (*lines)[FLOW_VIEW_LINE_CHARS_DEF + 1];
    uint8_t *styles;
} reader_slot_t;

struct reader {
    char path[128];
    FILE *file;                              /* 阅读句柄（随机读） */
    size_t file_size;
    int line_width;
    const lv_font_t *font;

    reader_index_t index;                    /* 块索引（后台任务构建） */
    reader_block_t *blocks;

    reader_slot_t slots[READER_SLOTS];
    uint32_t slot_ring;                      /* 槽替换指针 */
    char io_buf[READER_IO_BLK];              /* 块读取缓冲（分段读用） */

    volatile bool index_done;
    volatile int  index_progress;            /* 0..100 */
    volatile bool cancel;
    SemaphoreHandle_t done_sem;              /* 索引任务结束信号 */
    uint32_t total_lines;
};

/* 字形宽度回调（折行用；lv_font_get_glyph_width 纯只读，线程安全） */
static int32_t reader_glyph_w(void *ctx, uint32_t code, uint32_t next)
{
    return (int32_t)lv_font_get_glyph_width((const lv_font_t *)ctx, code, next);
}

/* ── 后台索引任务：全文件扫描建块索引，大块读 + 每块让出总线 ──
 * SD 与 LCD 共享 SPI2 总线：所有 SD 访问（fopen/fread/fclose）持 LVGL 锁
 * 与 LCD flush 串行化（spi_master 混用 interrupt/polling 传输会触发
 * spi_hal_setup_trans 断言）。每轮锁后立即释放，进度条可正常更新。 */

static void reader_index_task(void *arg)
{
    reader_t *r = arg;

    FILE *f = NULL;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        f = fopen(r->path, "r");
        esp_lv_adapter_unlock();
    }
    if (f) {
        char *buf = heap_caps_malloc(READER_IO_BLK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf) {
            size_t read_total = 0;
            while (!r->cancel) {
                size_t n = 0;
                if (esp_lv_adapter_lock(-1) == ESP_OK) {
                    n = fread(buf, 1, READER_IO_BLK, f);
                    esp_lv_adapter_unlock();
                }
                if (n == 0) break;
                size_t off = 0;
                while (off < n) {
                    size_t sub = n - off;
                    if (sub > READER_INDEX_BLOCK) sub = READER_INDEX_BLOCK;
                    reader_index_append(&r->index, buf + off, sub);
                    off += sub;
                }
                read_total += n;
                r->index_progress = (int)(read_total * 100 / r->file_size);
                vTaskDelay(1);   /* 让出 CPU/总线给 LCD flush */
            }
            heap_caps_free(buf);
        } else {
            ESP_LOGE(TAG, "index task: buf alloc failed");
        }
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            fclose(f);
            esp_lv_adapter_unlock();
        }
    } else {
        ESP_LOGE(TAG, "index task: fopen failed");
    }

    if (!r->cancel) {
        /* 收尾（EOF 语义，与文件大小无关）：补尾行 + 哨兵锚点 */
        reader_index_finish(&r->index);
        r->total_lines = reader_index_total_lines(&r->index);
        r->index_done = true;
    }
    xSemaphoreGive(r->done_sem);
    vTaskDelete(NULL);
}

/* ── 块缓存（LVGL 线程内使用，无并发） ── */

static reader_slot_t *reader_slot_find(reader_t *r, uint32_t offset)
{
    for (int i = 0; i < READER_SLOTS; i++) {
        if (r->slots[i].valid && r->slots[i].offset == offset) return &r->slots[i];
    }
    return NULL;
}

/* 同步读块 [offset, offset+len) 分段流式解析入槽（覆盖最旧槽）；
 * 块区间以行尾为界（锚点对齐），独立解析与索引行划分一致。
 * len 可达约 90KB（行数上限内的长行密集区），分段读不受缓冲限制 */
static reader_slot_t *reader_slot_load(reader_t *r, uint32_t offset, uint32_t len)
{
    reader_slot_t *slot = &r->slots[r->slot_ring % READER_SLOTS];
    r->slot_ring++;

    if (!r->file || len == 0 || (uint64_t)offset + len > r->file_size ||
        fseek(r->file, (long)offset, SEEK_SET) != 0) {
        slot->valid = false;
        return NULL;
    }

    flow_model_clear(&slot->model);
    uint32_t remain = len;
    while (remain > 0) {
        size_t n = remain > sizeof(r->io_buf) ? sizeof(r->io_buf) : remain;
        size_t got = fread(r->io_buf, 1, n, r->file);
        if (got == 0) break;
        flow_model_append(&slot->model, r->io_buf, got);
        remain -= (uint32_t)got;
    }
    /* 末尾无 '\n' 的残留行：补 '\n' 成完整行，与索引侧收尾一致。
     * 中间块边界必为行尾（cur_len==0），补入无副作用。 */
    if (slot->model.cur_len > 0) {
        flow_model_append(&slot->model, "\n", 1);
    }
    slot->offset = offset;
    slot->valid = true;
    return slot;
}

/* ── 资源释放（调用前提：索引任务已退出） ── */

static void reader_free_resources(reader_t *r)
{
    if (r->file) {
        fclose(r->file);
        r->file = NULL;
    }
    if (r->index.lines) {
        free(r->index.lines);
        r->index.lines = NULL;
    }
    if (r->index.styles) {
        free(r->index.styles);
        r->index.styles = NULL;
    }
    if (r->blocks) {
        heap_caps_free(r->blocks);
        r->blocks = NULL;
    }
    for (int i = 0; i < READER_SLOTS; i++) {
        if (r->slots[i].lines) {
            heap_caps_free(r->slots[i].lines);
            r->slots[i].lines = NULL;
        }
        if (r->slots[i].styles) {
            heap_caps_free(r->slots[i].styles);
            r->slots[i].styles = NULL;
        }
    }
}

/* ── 公共 API ── */

reader_t *reader_open(const char *path, int line_width, const lv_font_t *font)
{
    struct stat st;
    if (!path || !font || stat(path, &st) != 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > READER_MAX_FILE) {
        ESP_LOGW(TAG, "open: invalid path/size");
        return NULL;
    }

    reader_t *r = heap_caps_calloc(1, sizeof(reader_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) return NULL;

    r->file = fopen(path, "r");
    if (!r->file) {
        ESP_LOGW(TAG, "open: fopen failed");
        heap_caps_free(r);
        return NULL;
    }
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';
    r->file_size = (size_t)st.st_size;
    r->line_width = line_width;
    r->font = font;

    /* 块索引锚点数组（最密 1KB/锚点 + 哨兵） */
    uint32_t cap = (uint32_t)(r->file_size / READER_INDEX_BLOCK) + 2;
    r->blocks = heap_caps_malloc((size_t)cap * sizeof(reader_block_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->blocks) {
        ESP_LOGE(TAG, "open: blocks alloc %u x %u failed", cap, (unsigned)sizeof(reader_block_t));
        reader_free_resources(r);
        heap_caps_free(r);
        return NULL;
    }
    reader_index_init(&r->index, r->blocks, cap, line_width, reader_glyph_w, (void *)font);
    if (!r->index.lines || !r->index.styles) {
        ESP_LOGE(TAG, "open: index parser alloc failed");
        reader_free_resources(r);
        heap_caps_free(r);
        return NULL;
    }

    /* 块缓存槽行缓冲 */
    for (int i = 0; i < READER_SLOTS; i++) {
        reader_slot_t *s = &r->slots[i];
        s->lines = heap_caps_malloc((size_t)READER_SLOT_LINES * (FLOW_VIEW_LINE_CHARS_DEF + 1),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s->styles = heap_caps_malloc(READER_SLOT_LINES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->lines || !s->styles) {
            ESP_LOGE(TAG, "open: slot %d alloc failed", i);
            reader_free_resources(r);
            heap_caps_free(r);
            return NULL;
        }
        flow_model_init(&s->model, s->lines, s->styles, READER_SLOT_LINES, 17,
                        line_width, reader_glyph_w, (void *)font);
    }

    r->done_sem = xSemaphoreCreateBinary();
    if (!r->done_sem) {
        ESP_LOGE(TAG, "open: sem alloc failed");
        reader_free_resources(r);
        heap_caps_free(r);
        return NULL;
    }

    if (xTaskCreateWithCaps(reader_index_task, "rdr_idx", 4096, r, 3, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "open: index task create failed");
        reader_free_resources(r);
        vSemaphoreDelete(r->done_sem);
        heap_caps_free(r);
        return NULL;
    }

    return r;
}

void reader_close(reader_t *r)
{
    if (!r || !r->done_sem) return;
    r->cancel = true;
    /* 等待索引任务退出（cancel 后最多一个 16KB 块读完即退）。
     * 超时说明任务被饿死/IO 挂起：此时释放会让任务写已释放内存
     * （UAF），宁可泄漏本实例（记日志），恢复靠重启 */
    if (xSemaphoreTake(r->done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "close: index task timeout, reader leaked to avoid UAF");
        return;
    }
    vSemaphoreDelete(r->done_sem);
    r->done_sem = NULL;
    reader_free_resources(r);
    heap_caps_free(r);
}

bool reader_is_indexing(const reader_t *r)
{
    if (!r) return false;
    /* 任务完成信号（give 后 take 成功即完成；take 后 give 回去保持可重入） */
    if (xSemaphoreTake(r->done_sem, 0) == pdTRUE) {
        xSemaphoreGive(r->done_sem);
        return false;
    }
    return true;
}

int reader_progress(const reader_t *r)
{
    return r ? r->index_progress : 0;
}

int reader_total_lines(const reader_t *r)
{
    return r ? (int)r->total_lines : 0;
}

int reader_line_width(const reader_t *r)
{
    return r ? r->line_width : 0;
}

int reader_count(void *ctx)
{
    reader_t *r = ctx;
    return r ? (int)r->total_lines : 0;
}

const char *reader_line(void *ctx, int row, uint8_t *style)
{
    reader_t *r = ctx;
    if (!r || !r->index_done) return NULL;

    /* acquire 屏障：索引数据（blocks/total_lines）在索引任务置 index_done
     * 后经信号量发布；take 成功即保证其后续读取可见（双核内存序） */
    if (xSemaphoreTake(r->done_sem, 0) != pdTRUE) return NULL;
    xSemaphoreGive(r->done_sem);

    if (row < 0 || (uint32_t)row >= r->total_lines) return NULL;

    int b = reader_index_find(&r->index, (uint32_t)row);
    if (b < 0) return NULL;
    const reader_block_t *blk = &r->index.blocks[b];
    /* 块区间 [blk->offset, next)：下一锚点或文件尾（哨兵锚点保证） */
    uint32_t next = (b + 1 < (int)r->index.count) ? r->index.blocks[b + 1].offset : r->file_size;
    uint32_t len = next - blk->offset;

    reader_slot_t *slot = reader_slot_find(r, blk->offset);
    if (!slot) {
        slot = reader_slot_load(r, blk->offset, len);
        if (!slot) return NULL;
    }

    uint32_t in_blk = (uint32_t)row - blk->first_line;
    if (in_blk >= (uint32_t)flow_model_line_count(&slot->model)) return NULL;
    if (style) *style = flow_model_style(&slot->model, in_blk);
    return flow_model_line(&slot->model, in_blk);
}
