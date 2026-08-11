/* flow_model.c —— 可滚动内容流数据模型（纯 C，零 LVGL/平台依赖） */

#include "flow_model.h"
#include <string.h>

/* 完成当前行：写入环形历史，清空当前行输入状态 */
static void fv_line_break(flow_model_t *m)
{
    if (m->cur_len > 0) {
        int slot = m->line_count % m->max_lines;
        memcpy(m->lines[slot], m->cur, m->cur_len);
        m->lines[slot][m->cur_len] = '\0';
        if (m->styles) m->styles[slot] = 0;
        m->line_count++;
    }
    m->cur_len = 0;
    m->cur_w = 0;
}

void flow_model_init(flow_model_t *m,
                     char (*lines)[FLOW_VIEW_LINE_CHARS_DEF + 1], uint8_t *styles,
                     int max_lines, int visible_lines, int max_line_width,
                     flow_glyph_w_cb_t glyph_w, void *glyph_ctx)
{
    m->lines = lines;
    m->styles = styles;
    m->max_lines = max_lines > 0 ? max_lines : 1;
    m->visible_lines = visible_lines > 0 ? visible_lines : 1;
    m->line_count = 0;
    m->view_top = 0;
    m->follow = true;
    m->max_line_width = max_line_width > 0 ? max_line_width : FLOW_VIEW_LINE_CHARS_DEF;
    m->glyph_w = glyph_w;
    m->glyph_ctx = glyph_ctx;
    m->cur_len = 0;
    m->cur_w = 0;
}

void flow_model_append(flow_model_t *m, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            fv_line_break(m);
            continue;
        }
        if ((uint8_t)c < 0x20) continue;   /* 其他控制字符跳过 */
        if (m->cur_len >= FLOW_VIEW_LINE_CHARS_DEF) fv_line_break(m);

        if (m->glyph_w) {
            uint32_t next = (i + 1 < len) ? (uint32_t)(uint8_t)data[i + 1] : 0;
            int32_t w = m->glyph_w(m->glyph_ctx, (uint32_t)(uint8_t)c, next);
            if (w <= 0) continue;          /* 无字形字符跳过 */
            if (m->cur_w + w > m->max_line_width) fv_line_break(m);
            m->cur_w += w;
        }
        m->cur[m->cur_len++] = c;
    }
}

void flow_model_append_line(flow_model_t *m, const char *line, uint8_t style)
{
    fv_line_break(m);   /* 先结束未完成行 */
    size_t n = strlen(line);
    if (n > FLOW_VIEW_LINE_CHARS_DEF) n = FLOW_VIEW_LINE_CHARS_DEF;
    int slot = m->line_count % m->max_lines;
    memcpy(m->lines[slot], line, n);
    m->lines[slot][n] = '\0';
    if (m->styles) m->styles[slot] = style;
    m->line_count++;
}

void flow_model_load_text(flow_model_t *m, const char *text)
{
    flow_model_append(m, text, strlen(text));
}

void flow_model_clear(flow_model_t *m)
{
    m->line_count = 0;
    m->view_top = 0;
    m->follow = true;
    m->cur_len = 0;
    m->cur_w = 0;
}

int flow_model_line_count(const flow_model_t *m)
{
    return m->line_count;
}

const char *flow_model_line(const flow_model_t *m, int row)
{
    if (row < 0 || row >= m->line_count) return "";
    return m->lines[row % m->max_lines];
}

uint8_t flow_model_style(const flow_model_t *m, int row)
{
    if (!m->styles || row < 0 || row >= m->line_count) return 0;
    return m->styles[row % m->max_lines];
}

void flow_model_set_follow(flow_model_t *m, bool follow)
{
    m->follow = follow;
}

bool flow_model_is_following(const flow_model_t *m)
{
    return m->follow;
}

int flow_model_max_top(const flow_model_t *m)
{
    int max = m->line_count - m->visible_lines;
    return max > 0 ? max : 0;
}

int flow_model_view_top(const flow_model_t *m)
{
    return m->view_top;
}

void flow_model_set_view_top(flow_model_t *m, int top)
{
    int max = flow_model_max_top(m);
    if (top < 0) top = 0;
    if (top > max) top = max;
    m->view_top = top;
    if (top >= max) m->follow = true;   /* 滑到最底部 → 恢复跟随 */
}
