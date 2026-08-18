#ifndef READER_INDEX_H
#define READER_INDEX_H

/* reader_index：大文件 TXT 块索引（纯 C，无 LVGL/FreeRTOS 依赖）。
 *
 * 通过 flow_model 的行 break 回调（flow_model_set_break_cb）精确感知
 * 行尾位置与行号，在"距上一锚点 ≥ READER_INDEX_BLOCK 字节 且
 * 行增量 ≥ READER_INDEX_MAX_LINES/2"时记录锚点 {行尾文件偏移, 全局行号}。
 *
 * 锚点必然落在行尾（下一块起点 = 行首），因此缓存侧对
 * [锚点_k, 锚点_{k+1}) 做独立解析，得到与索引完全一致的行划分与行文本。
 * 块行数精确 = READER_INDEX_MAX_LINES/2（行长 ≥ 2B 时字节条件自动满足），
 * 槽缓存行容量取 READER_INDEX_MAX_LINES 即有充分余量。
 *
 * 文件末尾：reader_index_finish 收尾——末尾无换行的残留行补 '\n'
 * 成完整行，并追加哨兵锚点（offset = 文件尾），供缓存侧确定最后一块。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flow_model.h"

/* 锚点行增量目标（块行数 = 该值的一半） */
#define READER_INDEX_BLOCK 1024

/* 槽缓存行容量（块行数精确 = READER_INDEX_BLOCK，此值含补行等余量） */
#define READER_INDEX_MAX_LINES 2048

/* 块锚点：块起始文件偏移（行尾对齐）+ 该块第 1 行的全局渲染行号 */
typedef struct {
    uint32_t offset;       /* 块起始字节偏移（行尾对齐） */
    uint32_t first_line;   /* 块内第 1 行对应的全局行号 */
} reader_block_t;

typedef struct {
    reader_block_t *blocks;    /* 锚点数组（调用方分配，PSRAM） */
    uint32_t count;            /* 已记录块数 */
    uint32_t capacity;         /* 锚点数组容量 */
    uint32_t total_lines;      /* 累计渲染行数 */
    flow_model_t model;        /* 连续解析器（只统计行数） */
    char (*lines)[FLOW_VIEW_LINE_CHARS_DEF + 1];  /* 解析器行缓冲（1 行） */
    uint8_t *styles;
    int max_line_width;
    flow_glyph_w_cb_t glyph_w;
    void *glyph_ctx;
    uint32_t bytes_parsed;     /* 已解析文件字节 */
    uint32_t block_start;      /* 当前未锚定区间的起始字节（上一锚点） */
    uint32_t block_start_lines; /* 当前未锚定区间的起始行数 */
    bool finished;             /* 是否已收尾 */
} reader_index_t;

/* 初始化：blocks 由调用方提供（容量 = 文件字节数/READER_INDEX_BLOCK + 2）。
 * 返回 false 表示解析器内存分配失败（索引不可用）。 */
bool reader_index_init(reader_index_t *ix, reader_block_t *blocks, uint32_t capacity,
                       int max_line_width, flow_glyph_w_cb_t glyph_w, void *glyph_ctx);

/* 流式追加文件内容（连续解析，任意字节块，跨块行正确延续）。
 * 行 break 回调自动记录行尾锚点。 */
void reader_index_append(reader_index_t *ix, const char *data, size_t len);

/* 收尾（EOF 后调用一次）：末尾无 '\n' 的残留行补 '\n' 成完整行，
 * 并追加哨兵锚点（offset = 文件尾，first_line = 总行数）。 */
void reader_index_finish(reader_index_t *ix);

/* 渲染行号 → 块下标（二分，最后一个 first_line <= line_no 的块；
 * 哨兵锚点 first_line = total_lines，合法行号永不命中）；
 * 未找到返回 -1。 */
int reader_index_find(const reader_index_t *ix, uint32_t line_no);

uint32_t reader_index_total_lines(const reader_index_t *ix);

/* 清空（保留容量与配置，可复用） */
void reader_index_clear(reader_index_t *ix);

#endif /* READER_INDEX_H */
