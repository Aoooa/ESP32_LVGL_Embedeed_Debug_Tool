/* flow_model.c —— 可滚动内容流数据模型（纯 C，零 LVGL/平台依赖） */

#include "flow_model.h"
#include <string.h>

/* 完成当前行：写入环形历史，清空当前行输入状态。
 * pos = 行结束位置（下一个字符的字节偏移，供 break 回调） */
static void fv_line_break(flow_model_t *m, size_t pos)
{
    if (m->cur_len > 0) {
        int slot = m->line_count % m->max_lines;
        memcpy(m->lines[slot], m->cur, m->cur_len);
        m->lines[slot][m->cur_len] = '\0';
        if (m->styles) m->styles[slot] = 0;
        m->line_count++;
        if (m->break_cb) m->break_cb(m->break_ctx, pos, (uint32_t)m->line_count);
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
    m->stream_off = 0;
    m->break_cb = NULL;
    m->break_ctx = NULL;
    m->pend_len = 0;
    m->pend_total = 0;
}

/* UTF-8 首字节 → 字符字节数（ASCII=1；孤立续字节返回 1，由调用方跳过） */
static int fv_utf8_len(uint8_t c0)
{
    if (c0 >= 0xF0) return 4;
    if (c0 >= 0xE0) return 3;
    if (c0 >= 0xC2) return 2;
    return 1;
}

static uint32_t fv_utf8_decode(const char *s, int n)
{
    static const uint8_t first_mask[5] = { 0, 0x7F, 0x1F, 0x0F, 0x07 };
    uint32_t cp = (uint8_t)s[0] & first_mask[n];
    for (int i = 1; i < n; i++) cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
    return cp;
}

/* 处理一个完整字符：字符数/宽度折行后写入当前行。
 * s = 字符 UTF-8 字节；n = 字节数；next_cp = 相邻下一字符（kern 用，未知传 0）；
 * pos = 该字符的流位置（折行 break 回调位置） */
static void fv_char(flow_model_t *m, const char *s, int n, uint32_t next_cp, size_t pos)
{
    uint32_t cp = fv_utf8_decode(s, n);

    if (m->cur_len + n > FLOW_VIEW_LINE_CHARS_DEF) fv_line_break(m, pos);

    if (m->glyph_w) {
        int32_t w = m->glyph_w(m->glyph_ctx, cp, next_cp);
        if (w <= 0) return;            /* 无字形：整个字符跳过 */
        if (m->cur_w + w > m->max_line_width) fv_line_break(m, pos);
        m->cur_w += w;
    }
    memcpy(m->cur + m->cur_len, s, n);
    m->cur_len += n;
}

void flow_model_append(flow_model_t *m, const char *data, size_t len)
{
    size_t i = 0;

    /* 1) 续接上一 append 遗留的未完多字节字符（pend） */
    while (m->pend_len > 0 && i < len) {
        uint8_t b = (uint8_t)data[i];
        if ((b & 0xC0) != 0x80) {
            m->pend_len = 0;               /* 非法续字节：丢弃未完字符，本字节重新处理 */
            break;
        }
        i++;
        m->pend[m->pend_len++] = b;
        if (m->pend_len == m->pend_total) {
            int total = m->pend_total;
            m->pend_len = 0;
            fv_char(m, m->pend, total, 0, m->stream_off);
        }
    }

    /* 2) 主循环：完整字符处理 */
    while (i < len) {
        uint8_t c0 = (uint8_t)data[i];
        if (c0 == '\r') { i++; continue; }
        if (c0 == '\n') { fv_line_break(m, m->stream_off + i + 1); i++; continue; }
        if (c0 < 0x20) { i++; continue; }   /* 其他控制字符跳过 */

        /* UTF-8 解码：按完整字符处理，避免多字节汉字被拆散 */
        int n = fv_utf8_len(c0);
        if ((c0 & 0xC0) == 0x80) { i++; continue; }   /* 孤立续字节：跳过 */
        if (n > 1 && i + n > len) {
            /* 字符跨 append 边界：暂存待续（首字节 + 已有续字节），
             * 下一 append 续接，保证多字节字符不被拆分损坏 */
            m->pend[0] = c0;
            m->pend_total = n;
            size_t have = len - i;
            if (have > (size_t)n) have = n;
            memcpy(m->pend + 1, data + i + 1, have - 1);
            m->pend_len = (int)have;
            i = len;
            break;
        }
        if (n > 1) {
            bool valid = true;
            for (int k = 1; k < n; k++) {
                if (((uint8_t)data[i + k] & 0xC0) != 0x80) { valid = false; break; }
            }
            if (!valid) { i++; continue; }             /* 非法序列：跳过首字节 */
        }
        uint32_t next_cp = 0;
        if (i + n < len) {
            int nn = fv_utf8_len((uint8_t)data[i + n]);
            if (nn > 1 && i + n + nn > len) nn = 1;
            next_cp = fv_utf8_decode(data + i + n, nn);
        }
        fv_char(m, data + i, n, next_cp, m->stream_off + i);
        i += n;
    }
    m->stream_off += len;
}

void flow_model_append_line(flow_model_t *m, const char *line, uint8_t style)
{
    fv_line_break(m, m->stream_off);   /* 先结束未完成行 */
    size_t n = strlen(line);
    if (n > FLOW_VIEW_LINE_CHARS_DEF) n = FLOW_VIEW_LINE_CHARS_DEF;
    int slot = m->line_count % m->max_lines;
    memcpy(m->lines[slot], line, n);
    m->lines[slot][n] = '\0';
    if (m->styles) m->styles[slot] = style;
    m->line_count++;
}

void flow_model_set_break_cb(flow_model_t *m, flow_model_break_cb_t cb, void *ctx)
{
    m->break_cb = cb;
    m->break_ctx = ctx;
}

void flow_model_load_text(flow_model_t *m, const char *text)
{
    flow_model_clear(m);              /* 加载新文本前清空旧内容，避免两文件拼接 */
    flow_model_append(m, text, strlen(text));
}

void flow_model_clear(flow_model_t *m)
{
    m->line_count = 0;
    m->view_top = 0;
    m->follow = true;
    m->cur_len = 0;
    m->cur_w = 0;
    m->stream_off = 0;
    m->pend_len = 0;
    m->pend_total = 0;
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
