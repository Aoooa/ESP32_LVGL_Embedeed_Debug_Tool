#ifndef FLOW_MODEL_H
#define FLOW_MODEL_H

/* flow_model：可滚动内容流的纯 C 数据模型（零 LVGL/平台依赖）。
 *
 * 职责：环形行历史 + 逐字符解析（折行）+ 滚动窗口计算。
 * 折行需要字形像素宽度，通过 flow_glyph_w_cb_t 回调注入（宿主提供），
 * 因此本模块可独立编译与单元测试。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── 编译期配置（可用 -DFLOW_VIEW_* 覆盖） ── */
#ifndef FLOW_VIEW_LINE_CHARS_DEF
#define FLOW_VIEW_LINE_CHARS_DEF 60      /* 单行字符上限，超出自动折行/截断 */
#endif

/* 字形宽度回调：返回字符 code 的像素宽度（含相邻字符 next 的间距修正）。
 * 返回 <= 0 表示该字符无字形（跳过）。 */
typedef int32_t (*flow_glyph_w_cb_t)(void *ctx, uint32_t code, uint32_t next);

/* 行 break 回调（索引/统计用）：每完成一行调用一次。
 * byte_off = 该行结束位置在当前流中的绝对字节偏移（下一个字符的位置，
 * 折行/换行均适用，跨 append 调用延续）；line_no = 完成后的总行数。
 * NULL 时不调用，无额外开销。 */
typedef void (*flow_model_break_cb_t)(void *ctx, size_t byte_off, uint32_t line_no);

/* 内容流模型实例（全部状态在此，支持多实例） */
typedef struct {
    char  (*lines)[FLOW_VIEW_LINE_CHARS_DEF + 1];  /* 行文本环形缓冲（外部提供） */
    uint8_t *styles;       /* 每行样式（外部提供，可为 NULL） */
    int max_lines;         /* 环形历史上限 */
    int visible_lines;     /* 视口可见行数 */
    int line_count;        /* 总行数（含环形覆盖） */
    int view_top;          /* 视口窗口起始行号 */
    bool follow;           /* 跟随最新（数据到达即显示末尾） */
    int max_line_width;    /* 一行最大像素宽度（视口宽 - 边距），折行依据 */
    flow_glyph_w_cb_t glyph_w;   /* 字形宽度回调（可为 NULL，则只按字符数截断） */
    void *glyph_ctx;
    char  cur[FLOW_VIEW_LINE_CHARS_DEF];   /* 正在输入的行 */
    int   cur_len;
    int32_t cur_w;
    size_t stream_off;           /* 当前流累计字节偏移（跨 append 延续） */
    flow_model_break_cb_t break_cb;  /* 行 break 回调（NULL 不调用） */
    void *break_ctx;
    char  pend[4];               /* 跨 append 的未完多字节字符（续字节缓冲） */
    int   pend_len;              /* 已收集续字节数 */
    int   pend_total;            /* 目标字符总字节数 */
} flow_model_t;

/* 初始化/重置模型。lines/styles 由调用方提供（行数 = max_lines）。 */
void flow_model_init(flow_model_t *m,
                     char (*lines)[FLOW_VIEW_LINE_CHARS_DEF + 1], uint8_t *styles,
                     int max_lines, int visible_lines, int max_line_width,
                     flow_glyph_w_cb_t glyph_w, void *glyph_ctx);

/* 流式追加（解析换行/控制字符/按像素宽度折行） */
void flow_model_append(flow_model_t *m, const char *data, size_t len);

/* 直接追加一行完整文本（自动结束未完成行；超长截断；style 用于着色预留） */
void flow_model_append_line(flow_model_t *m, const char *line, uint8_t style);

/* 设置行 break 回调（索引/统计用；NULL 取消） */
void flow_model_set_break_cb(flow_model_t *m, flow_model_break_cb_t cb, void *ctx);

/* 整段文本加载（内部等价于 append 全部） */
void flow_model_load_text(flow_model_t *m, const char *text);

/* 清空（保留容量配置） */
void flow_model_clear(flow_model_t *m);

int flow_model_line_count(const flow_model_t *m);

/* 读取行内容（row 可为任意历史行号，环形自动取模） */
const char *flow_model_line(const flow_model_t *m, int row);
uint8_t flow_model_style(const flow_model_t *m, int row);

/* 跟随语义与滚动窗口 */
void flow_model_set_follow(flow_model_t *m, bool follow);
bool flow_model_is_following(const flow_model_t *m);
int  flow_model_max_top(const flow_model_t *m);     /* view_top 允许的最大值（>=0） */
int  flow_model_view_top(const flow_model_t *m);
/* 设置窗口起始行（自动 clamp；滑到最底部时恢复 follow） */
void flow_model_set_view_top(flow_model_t *m, int top);

#endif /* FLOW_MODEL_H */
