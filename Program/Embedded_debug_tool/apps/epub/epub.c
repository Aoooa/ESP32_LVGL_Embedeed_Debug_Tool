/* epub.c —— EPUB → 纯文本（手写 ZIP 中央目录 + zlib inflate + XHTML 抽取）。
 * 参考 M5Stack Paper S3 中文电子书（epub_reader.cpp）的成熟路线：
 *   1. 扫 EPUB 末尾 EOCD → Central Directory → 列出条目
 *   2. 找 container.xml → metadata.opf → spine 顺序的 XHTML 章节文件
 *   3. 逐个 inflate 解压章节 → 剥 HTML 标签/解码实体 → 拼成纯文本
 *   4. 章节间加分隔行 → 写目标 .txt（Reader 直接打开）
 * 不处理内嵌图片（主流墨水屏阅读器均略过正文插图）。
 */

#include "epub.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "zlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define E_TAG "epub"

/* ── 小端工具 ── */
static uint16_t zU16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t zU32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* ZIP 本地文件头 */
#define LFH_SIG 0x04034b50u
#define CDH_SIG 0x02014b50u
#define EOCD_SIG 0x06054b50u

typedef struct {
    char name[300];
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t method;        /* 0=store 8=deflate */
    uint32_t local_offset;
} zip_ent_t;

#define MAX_ZIP_ENTRIES 128

/* ── 读整个文件（PSRAM） ── */
static bool read_whole(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64L * 1024 * 1024) { fclose(f); return false; }
    uint8_t *b = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { fclose(f); return false; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { heap_caps_free(b); fclose(f); return false; }
    fclose(f);
    *out = b;
    *out_len = (size_t)sz;
    return true;
}

/* ── 解析 ZIP 中央目录 ── */
static int zip_parse(const uint8_t *buf, size_t len, zip_ent_t *ents, int max)
{
    /* 反向找 EOCD */
    if (len < 22) return 0;
    size_t search = len < 65557 ? len : 65557;
    size_t eocd = len;
    for (size_t i = len - search; i + 22 <= len; i++) {
        if (zU32(buf + i) == EOCD_SIG) { eocd = i; break; }
    }
    if (eocd == len) return 0;

    uint16_t n_entries = zU16(buf + eocd + 10);
    uint32_t cd_start = zU32(buf + eocd + 16);
    if (n_entries > max) n_entries = (uint16_t)max;

    int n = 0;
    uint32_t p = cd_start;
    for (uint16_t i = 0; i < n_entries; i++) {
        if (p + 46 > len || zU32(buf + p) != CDH_SIG) break;
        uint16_t name_len = zU16(buf + p + 28);
        uint16_t extra_len = zU16(buf + p + 30);
        uint16_t comment_len = zU16(buf + p + 32);
        if (p + 46 + name_len > len) break;
        if (name_len >= sizeof(ents[n].name)) name_len = sizeof(ents[n].name) - 1;
        memcpy(ents[n].name, buf + p + 46, name_len);
        ents[n].name[name_len] = '\0';
        ents[n].method = zU16(buf + p + 10);
        ents[n].comp_size = zU32(buf + p + 20);
        ents[n].uncomp_size = zU32(buf + p + 24);
        ents[n].local_offset = zU32(buf + p + 42);
        n++;
        p += 46 + name_len + extra_len + comment_len;
    }
    return n;
}

/* ── 解压单个条目（store/deflate；输出 malloc） ── */
static uint8_t *zip_extract(const uint8_t *buf, size_t len, const zip_ent_t *e, size_t *out_len)
{
    *out_len = 0;
    uint32_t off = e->local_offset;
    if (off + 30 > len || zU32(buf + off) != LFH_SIG) return NULL;
    uint16_t name_len = zU16(buf + off + 26);
    uint16_t extra_len = zU16(buf + off + 28);
    uint32_t data_off = off + 30 + name_len + extra_len;
    if (data_off + e->comp_size > len) return NULL;

    if (e->method == 0) {   /* store */
        uint8_t *o = heap_caps_malloc(e->comp_size ? e->comp_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!o) return NULL;
        memcpy(o, buf + data_off, e->comp_size);
        *out_len = e->comp_size;
        return o;
    }
    if (e->method == 8) {   /* deflate（zlib 裸流 inflateInit2(-15)） */
        size_t cap = e->uncomp_size ? e->uncomp_size : 64 * 1024;
        uint8_t *o = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!o) return NULL;

        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -15) != Z_OK) { heap_caps_free(o); return NULL; }
        zs.next_in = (Bytef *)(buf + data_off);
        zs.avail_in = e->comp_size;
        zs.next_out = o;
        zs.avail_out = (uInt)cap;

        int r = inflate(&zs, Z_FINISH);
        size_t produced = cap - zs.avail_out;
        inflateEnd(&zs);
        if (r != Z_STREAM_END) { heap_caps_free(o); return NULL; }
        *out_len = produced;
        return o;
    }
    return NULL;
}

/* ── URL 解码 + 路径归一（container/spine 里的相对路径） ── */
static void url_decode_to(const char *src, char *dst, size_t dstsz)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dstsz; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            int hi = isdigit((unsigned char)src[i + 1]) ? src[i + 1] - '0'
                       : tolower((unsigned char)src[i + 1]) - 'a' + 10;
            int lo = isdigit((unsigned char)src[i + 2]) ? src[i + 2] - '0'
                       : tolower((unsigned char)src[i + 2]) - 'a' + 10;
            dst[di++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

/* 去掉事目前缀目录（EPUB 相对路径基于 OPF 所在目录）：
 * 返回新串（静态缓冲） */
static void join_relative(char *out, size_t outsz, const char *base_dir, const char *rel)
{
    if (!base_dir[0] || rel[0] == '/') {
        snprintf(out, outsz, "%s", rel);
        return;
    }
    snprintf(out, outsz, "%s/%s", base_dir, rel);
}

/* ── inflate 后的 XHTML → 纯文本流式写入文件 ── */
static void html_append_text(const uint8_t *html, size_t len, FILE *out)
{
    /* 状态：0=文本，1=标签内；script/style 跳过 */
    int in_tag = 0;
    int skip = 0;               /* 1=在 script/style 内 */
    char word[512];
    size_t wi = 0;

    for (size_t i = 0; i < len; i++) {
        char c = (char)html[i];
        if (in_tag) {
            if (c == '>') in_tag = 0;
            continue;
        }
        if (c == '<') {
            /* 检测 <script / <style：进入跳过模式（直到对应闭合） */
            size_t j = i + 1;
            char tag[16]; size_t ti = 0;
            while (j < len && ti < sizeof(tag) - 1 && isalpha((unsigned char)html[j]))
                tag[ti++] = (char)tolower((unsigned char)html[j]);
            tag[ti] = '\0';
            if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0) skip = 1;
            if (skip && tag[0] != '\0' && strcmp(tag, "script") != 0 && strcmp(tag, "style") != 0) {
                /* 非闭合标签（如 <br>）不退出 skip；闭合 </script> 在下方处理 */
            }
            /* 遇到 </script> 或 </style>：退出跳过 */
            if (html[i + 1] == '/' && (strncmp((const char *)html + i + 2, "script", 6) == 0 ||
                                       strncmp((const char *)html + i + 2, "style", 5) == 0)) {
                skip = 0;
            }
            /* <p>/<br>/<div>/<h1..6>/<li> 等换行标签：输出换行 */
            if (!skip) {
                if (strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 || strcmp(tag, "br") == 0 ||
                    strcmp(tag, "li") == 0 || strcmp(tag, "h1") == 0 || strcmp(tag, "h2") == 0 ||
                    strcmp(tag, "h3") == 0 || strcmp(tag, "h4") == 0 || strcmp(tag, "h5") == 0 ||
                    strcmp(tag, "h6") == 0 || strcmp(tag, "tr") == 0 || strcmp(tag, "/p") == 0 ||
                    strcmp(tag, "/div") == 0 || strcmp(tag, "/h1") == 0 || strcmp(tag, "/h2") == 0 ||
                    strcmp(tag, "/h3") == 0 || strcmp(tag, "/h4") == 0 || strcmp(tag, "/h5") == 0 ||
                    strcmp(tag, "/h6") == 0 || strcmp(tag, "/li") == 0 || strcmp(tag, "/tr") == 0) {
                    if (wi) { fwrite(word, 1, wi, out); wi = 0; }
                    fputc('\n', out);
                }
                if (tag[0] == '\0' && html[i + 1] == '/' ) { /* 普通闭合标签无动作 */ }
            }
            in_tag = 1;
            continue;
        }
        if (skip) continue;
        if (c == '&') {
            /* 实体：先找分号 */
            size_t j = i + 1;
            while (j < len && j < i + 12 && html[j] != ';') j++;
            if (j < len && html[j] == ';' && j - i <= 12) {
                size_t el = j - i - 1;
                char ent[16];
                memcpy(ent, html + i + 1, el);
                ent[el] = '\0';
                char out_c = '?';
                if (strcmp(ent, "amp") == 0) out_c = '&';
                else if (strcmp(ent, "lt") == 0) out_c = '<';
                else if (strcmp(ent, "gt") == 0) out_c = '>';
                else if (strcmp(ent, "quot") == 0) out_c = '"';
                else if (strcmp(ent, "apos") == 0) out_c = '\'';
                else if (strcmp(ent, "nbsp") == 0) out_c = ' ';
                else if (ent[0] == '#') {
                    long code = 0;
                    if (ent[1] == 'x' || ent[1] == 'X') code = strtol(ent + 2, NULL, 16);
                    else code = strtol(ent + 1, NULL, 10);
                    if (code >= 0x20 && code <= 0x7E) out_c = (char)code;   /* ASCII 保底；CJK 交给编码（UTF-8 多用原文） */
                    else if (code > 0) {
                        /* 非 ASCII 数字实体：写 UTF-8 */
                        if (code < 0x800) {
                            char u[2] = { (char)(0xC0 | (code >> 6)), (char)(0x80 | (code & 0x3F)) };
                            fwrite(u, 1, 2, out);
                        } else if (code < 0x10000) {
                            char u[3] = { (char)(0xE0 | (code >> 12)),
                                          (char)(0x80 | ((code >> 6) & 0x3F)),
                                          (char)(0x80 | (code & 0x3F)) };
                            fwrite(u, 1, 3, out);
                        }
                        i = j;
                        continue;
                    }
                }
                /* 缓冲字符（累积成词，便于去重空格） */
                if (out_c != ' ' || wi == 0 || word[wi - 1] != ' ') {
                    if (wi < sizeof(word) - 1) word[wi++] = out_c;
                }
                i = j;
                continue;
            }
        }
        /* 普通字符：空格压缩 */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (wi && word[wi - 1] != ' ') {
                if (wi < sizeof(word) - 1) word[wi++] = ' ';
            }
        } else {
            if (wi < sizeof(word) - 1) word[wi++] = c;
        }
    }
    if (wi) { fwrite(word, 1, wi, out); }
    fputc('\n', out);
}

/* 从已解析的文本里提取 href 清单（container → opf → spine → chapters）。
 * 简化实现：container.xml 找 opf 路径；opf 里找 spine itemref + manifest href */
static bool find_str(const uint8_t *buf, size_t len, const char *key, char *out, size_t outsz)
{
    const char *s = (const char *)buf;
    const char *p = s;
    size_t kl = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        /* 找属性值 = "..." */
        const char *eq = strchr(p, '=');
        if (eq && (size_t)(eq - s) < len) {
            const char *q = eq + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"' || *q == '\'') {
                char quot = *q++;
                const char *end = strchr(q, quot);
                if (end) {
                    size_t n = (size_t)(end - q);
                    if (n >= outsz) n = outsz - 1;
                    memcpy(out, q, n);
                    out[n] = '\0';
                    return true;
                }
            }
        }
        p += kl;
    }
    return false;
}

esp_err_t epub_convert(const char *src_path, const char *dst_path)
{
    uint8_t *zip = NULL;
    size_t zip_len = 0;
    if (!read_whole(src_path, &zip, &zip_len)) {
        ESP_LOGE(E_TAG, "read %s failed", src_path);
        return ESP_ERR_NOT_FOUND;
    }

    zip_ent_t ents[MAX_ZIP_ENTRIES];
    int n = zip_parse(zip, zip_len, ents, MAX_ZIP_ENTRIES);
    if (n <= 0) {
        heap_caps_free(zip);
        ESP_LOGE(E_TAG, "bad zip");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(E_TAG, "zip entries: %d", n);

    /* 1. container.xml → OPF 路径 */
    char opf_rel[300] = "";
    for (int i = 0; i < n; i++) {
        /* container.xml 在 META-INF/container.xml 固定路径，但兼容查找 */
        if (strstr(ents[i].name, "container.xml")) {
            size_t cl = 0;
            uint8_t *cb = zip_extract(zip, zip_len, &ents[i], &cl);
            if (cb) {
                /* 找 rootfile full-path */
                const char *k = "full-path";
                char tmp[300];
                if (find_str(cb, cl, k, tmp, sizeof(tmp))) {
                    /* 有时 full-path= 后面是 " 属性，find_str 已取到值 */
                    url_decode_to(tmp, opf_rel, sizeof(opf_rel));
                    ESP_LOGI(E_TAG, "opf: %s", opf_rel);
                }
                heap_caps_free(cb);
            }
            break;
        }
    }
    if (!opf_rel[0]) {
        heap_caps_free(zip);
        ESP_LOGE(E_TAG, "no opf");
        return ESP_ERR_INVALID_STATE;
    }

    /* 2. 定位 OPF 条目并解压 */
    int opf_idx = -1;
    for (int i = 0; i < n; i++) {
        const char *nm = ents[i].name;
        /* 匹配结尾（opf_rel 可能含 ./ 前缀差异）：比较去掉 ./ 后 */
        const char *a = nm, *b = opf_rel;
        while (*b && *a == *b) { a++; b++; }
        /* 用 strstr 宽松匹配 last segment */
        if (strstr(nm, opf_rel) || strstr(opf_rel, nm) ||
            (strcmp(opf_rel, "OEBPS/content.opf") >= 0 && strstr(nm, opf_rel + 6))) {
            if (strstr(nm, ".opf")) { opf_idx = i; break; }
        }
    }
    /* 宽松重试：任何 .opf */
    if (opf_idx < 0) {
        for (int i = 0; i < n; i++) if (strstr(ents[i].name, ".opf")) { opf_idx = i; break; }
    }
    if (opf_idx < 0) {
        heap_caps_free(zip);
        ESP_LOGE(E_TAG, "no opf entry");
        return ESP_ERR_INVALID_STATE;
    }

    size_t opf_len = 0;
    uint8_t *opf = zip_extract(zip, zip_len, &ents[opf_idx], &opf_len);
    if (!opf) {
        heap_caps_free(zip);
        ESP_LOGE(E_TAG, "opf extract fail");
        return ESP_ERR_NO_MEM;
    }
    /* OPF 所在目录（相对路径前缀） */
    char opf_dir[300] = "";
    {
        const char *slash = strrchr(ents[opf_idx].name, '/');
        if (slash) {
            size_t dl = (size_t)(slash - ents[opf_idx].name);
            if (dl >= sizeof(opf_dir)) dl = sizeof(opf_dir) - 1;
            memcpy(opf_dir, ents[opf_idx].name, dl);
            opf_dir[dl] = '\0';
        }
    }

    /* 3. 提取 spine 顺序的章节文件引用：manifest 收集 id→href，spine 给 idref 顺序 */
    /* 简化：直接收集 spine itemref 的 idref，再从 manifest 找对应 href */
    /* 这里采用最鲁棒的做法：扫 OPF 里所有 href="...xhtml/html" 按出现顺序 */
    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        heap_caps_free(opf); heap_caps_free(zip);
        ESP_LOGE(E_TAG, "open dst %s fail", dst_path);
        return ESP_ERR_INVALID_STATE;
    }

    /* 章节计数器：写章节标题分隔 */
    int chapter_no = 0;
    const char *needle = "href";
    const char *p = (const char *)opf;
    size_t remaining = opf_len;
    while ((p = strstr(p, needle)) != NULL && chapter_no < 200) {
        const char *eq = strchr(p, '=');
        if (eq) {
            const char *q = eq + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"' || *q == '\'') {
                char quot = *q++;
                const char *end = strchr(q, quot);
                if (end && end - q < 300) {
                    char href[300];
                    size_t hn = (size_t)(end - q);
                    memcpy(href, q, hn);
                    href[hn] = '\0';
                    /* 只取章节类文件（.xhtml/.html），跳过 css/图片/opf/ncx */
                    const char *dot = strrchr(href, '.');
                    bool chapter = dot && (strcasecmp(dot, ".xhtml") == 0 || strcasecmp(dot, ".html") == 0);
                    if (chapter && !strstr(href, "toc")) {
                        char rel[320];
                        join_relative(rel, sizeof(rel), opf_dir, href);
                        url_decode_to(rel, rel, sizeof(rel));   /* 再解码一次（含合成前缀可能带 %） */
                        /* 匹配 zip 条目：规范是 OPF 目录 + href，允许不匹配则跳过 */
                        int zi = -1;
                        for (int k = 0; k < n; k++) {
                            if (strcmp(ents[k].name, rel) == 0 || strstr(ents[k].name, href)) { zi = k; break; }
                        }
                        if (zi >= 0) {
                            size_t cl = 0;
                            uint8_t *cb = zip_extract(zip, zip_len, &ents[zi], &cl);
                            if (cb) {
                                chapter_no++;
                                char t[32];
                                snprintf(t, sizeof(t), "\n\n===== %d =====\n\n", chapter_no);
                                fwrite(t, 1, strlen(t), out);
                                html_append_text(cb, cl, out);
                                heap_caps_free(cb);
                                ESP_LOGI(E_TAG, "chapter %d: %s", chapter_no, href);
                            }
                        } else {
                            ESP_LOGD(E_TAG, "skip: %s", href);
                        }
                    }
                }
            }
        }
        p += 4;
    }

    fclose(out);
    heap_caps_free(opf);
    heap_caps_free(zip);

    if (chapter_no == 0) {
        ESP_LOGE(E_TAG, "no chapters extracted from %s", src_path);
        remove(dst_path);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(E_TAG, "OK: %d chapters -> %s", chapter_no, dst_path);
    return ESP_OK;
}