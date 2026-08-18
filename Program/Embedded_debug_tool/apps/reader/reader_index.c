/* reader_index.c —— 大文件 TXT 块索引（纯 C） */

#include "reader_index.h"
#include <stdlib.h>
#include <string.h>

static void ix_line_break_cb(void *ctx, size_t byte_off, uint32_t line_no);

bool reader_index_init(reader_index_t *ix, reader_block_t *blocks, uint32_t capacity,
                       int max_line_width, flow_glyph_w_cb_t glyph_w, void *glyph_ctx)
{
    memset(ix, 0, sizeof(*ix));
    ix->blocks = blocks;
    ix->capacity = capacity;
    ix->max_line_width = max_line_width;
    ix->glyph_w = glyph_w;
    ix->glyph_ctx = glyph_ctx;

    /* 解析器只统计行数不读行：max_lines=1 足够（环形覆盖不影响计数） */
    ix->lines = malloc(FLOW_VIEW_LINE_CHARS_DEF + 1);
    ix->styles = malloc(1);
    if (ix->lines && ix->styles) {
        flow_model_init(&ix->model, ix->lines, ix->styles, 1, 17,
                        max_line_width, glyph_w, glyph_ctx);
        flow_model_set_break_cb(&ix->model, ix_line_break_cb, ix);
        /* 首锚点：文件头（行 0 起）——块 0 覆盖文件开头到首个行尾锚点 */
        if (capacity > 0) {
            ix->blocks[0].offset = 0;
            ix->blocks[0].first_line = 0;
            ix->count = 1;
        }
        return true;
    }
    free(ix->lines);
    free(ix->styles);
    ix->lines = NULL;
    ix->styles = NULL;
    return false;
}

/* 行 break 回调：行结束位置（byte_off）与完成后的行号。
 * 距上一锚点 ≥ READER_INDEX_BLOCK 字节且行增量 ≥ READER_INDEX_MAX_LINES/2
 * 时记录锚点。块行数精确 = READER_INDEX_MAX_LINES/2：
 * 行长 ≥ 2B 时（1024 行 × 2B = 2048B ≥ READER_INDEX_BLOCK）字节条件
 * 在行增量首次达标时必然满足。 */
static void ix_line_break_cb(void *ctx, size_t byte_off, uint32_t line_no)
{
    reader_index_t *ix = ctx;
    if (ix->count >= ix->capacity) return;

    uint32_t abs_off = (uint32_t)byte_off;

    if (abs_off - ix->block_start < READER_INDEX_BLOCK) return;
    if (line_no - ix->block_start_lines < READER_INDEX_MAX_LINES / 2) return;

    ix->blocks[ix->count].offset = abs_off;
    ix->blocks[ix->count].first_line = line_no;
    ix->count++;
    ix->block_start = abs_off;
    ix->block_start_lines = line_no;
}

void reader_index_append(reader_index_t *ix, const char *data, size_t len)
{
    if (!ix->lines || !ix->styles) return;   /* 解析器不可用 */
    if (len == 0) return;

    flow_model_append(&ix->model, data, len);
    ix->bytes_parsed += (uint32_t)len;
    ix->total_lines = (uint32_t)flow_model_line_count(&ix->model);
}

void reader_index_finish(reader_index_t *ix)
{
    if (ix->finished) return;
    ix->finished = true;

    /* 末尾无 '\n' 的残留行：补 '\n' 成完整行（与缓存侧最后一块解析一致）。
     * 补行是虚拟字节（位置在文件尾之外），禁用回调避免产生越界锚点 */
    if (ix->model.cur_len > 0) {
        flow_model_set_break_cb(&ix->model, NULL, NULL);
        flow_model_append(&ix->model, "\n", 1);
        flow_model_set_break_cb(&ix->model, ix_line_break_cb, ix);
        ix->total_lines = (uint32_t)flow_model_line_count(&ix->model);
    }
    /* 哨兵锚点：offset = 文件尾，first_line = 总行数（供缓存侧确定最后一块）。
     * 合法行号 < total_lines 恒不命中哨兵，且哨兵 offset 严格大于前锚点 */
    if (ix->count < ix->capacity) {
        ix->blocks[ix->count].offset = ix->bytes_parsed;
        ix->blocks[ix->count].first_line = ix->total_lines;
        ix->count++;
    }
}

int reader_index_find(const reader_index_t *ix, uint32_t line_no)
{
    if (ix->count == 0 || line_no >= ix->total_lines) return -1;

    /* 最后一个 first_line <= line_no 的块 */
    int lo = 0, hi = (int)ix->count - 1, ans = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (ix->blocks[mid].first_line <= line_no) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

uint32_t reader_index_total_lines(const reader_index_t *ix)
{
    return ix->total_lines;
}

void reader_index_clear(reader_index_t *ix)
{
    ix->count = 0;
    ix->total_lines = 0;
    ix->bytes_parsed = 0;
    ix->block_start = 0;
    ix->block_start_lines = 0;
    ix->finished = false;
    flow_model_clear(&ix->model);
}
