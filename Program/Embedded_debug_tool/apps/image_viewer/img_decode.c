/* img_decode.c —— 图片解码（JPG 硬件 / PNG 逐行 / BMP 简易），统一输出 RGB565。
 * JPG：esp_new_jpeg（S3 硬件 JPEG，解码时 scale 到目标尺寸，RGB565_LE 直出）。
 * PNG：libpng 逐行读取 + 边读边最近邻降采样（内存小，任意尺寸）。
 * BMP：仅支持 24/32bpp 非压缩（常见截图/壁纸），自绘 RGB565。
 */

#include "img_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"   /* jpeg_calloc_align / jpeg_free_align */
#undef PNG_DEBUG
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>   /* mkdir */
#include <dirent.h>

#define ID_TAG "img_decode"

/* ── 缩略图 SD 缓存（/sdcard/.ivthumbs/<fnv32>.thb） ──
 * 格式：[4B magic "IVTH"][2B w][2B h][RGB565 LE 像素 w*h*2] */
#define IVC_MAGIC "IVTH"
#define IVC_DIR   "/sdcard/.ivthumbs"

static uint32_t fnv1a_32(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

bool img_decode_cache_write(const char *src_path, const uint16_t *pixels, int w, int h)
{
    if (!src_path || !pixels || w <= 0 || h <= 0) return false;
    char path[320];
    snprintf(path, sizeof(path), "%s/%08X.thb", IVC_DIR, (unsigned)fnv1a_32(src_path));
    mkdir(IVC_DIR, 0777);   /* 幂等 */
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = false;
    uint8_t hdr[8];
    memcpy(hdr, IVC_MAGIC, 4);
    hdr[4] = (uint8_t)w; hdr[5] = (uint8_t)(w >> 8);
    hdr[6] = (uint8_t)h; hdr[7] = (uint8_t)(h >> 8);
    if (fwrite(hdr, 1, 8, f) == 8 &&
        fwrite(pixels, 2, (size_t)w * h, f) == (size_t)w * h) {
        ok = true;
    }
    fclose(f);
    if (!ok) remove(path);
    return ok;
}

bool img_decode_cache_read(const char *src_path, img_decode_result_t *out)
{
    if (!out) return false;
    out->pixels = NULL;
    out->w = out->h = 0;
    out->jpeg_buf = false;
    if (!src_path) return false;
    char path[320];
    snprintf(path, sizeof(path), "%s/%08X.thb", IVC_DIR, (unsigned)fnv1a_32(src_path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, IVC_MAGIC, 4) != 0) {
        fclose(f);
        return false;
    }
    int w = hdr[4] | (hdr[5] << 8);
    int hh = hdr[6] | (hdr[7] << 8);
    if (w <= 0 || hh <= 0 || w > 512 || hh > 512) {   /* 缩略图尺寸上限防御 */
        fclose(f);
        return false;
    }
    uint16_t *px = heap_caps_malloc((size_t)w * hh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px) { fclose(f); return false; }
    if (fread(px, 2, (size_t)w * hh, f) != (size_t)w * hh) {
        heap_caps_free(px);
        fclose(f);
        return false;
    }
    fclose(f);
    out->pixels = px;
    out->w = w;
    out->h = hh;
    out->jpeg_buf = false;
    return true;
}

/* 文件 → 内存（JPEG 解码需要完整输入；PNG 用自定义 read_fn 流式亦可，统一读入） */
static bool read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (32 * 1024 * 1024)) {   /* 上限 32MB 防御 */
        fclose(f);
        return false;
    }
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        heap_caps_free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    *data = buf;
    *len = (size_t)sz;
    return true;
}

/* 目标尺寸：contain=等比，每维 ≤ max（不放大）；cover=等比填满（可放大，
 * 至少一边 == max，另一边可能超出）。 */
static void fit_size(int src_w, int src_h, int max_w, int max_h,
                     bool cover, int *dw, int *dh)
{
    if (src_w <= 0 || src_h <= 0) { *dw = 1; *dh = 1; return; }
    float s = (float)max_w / src_w;
    float sh = (float)max_h / src_h;
    if (cover) {
        /* 取大者：至少一边填满（可能越界） */
        if (sh > s) s = sh;
    } else {
        if (sh < s) s = sh;
        if (s > 1.0f) s = 1.0f;   /* contain 不放大 */
    }
    int w = (int)(src_w * s + 0.5f);
    int h = (int)(src_h * s + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *dw = w; *dh = h;
}

/* ── JPEG（esp_new_jpeg 硬件全解码 + 软件最近邻缩放到目标） ──
 * 官方 test 仅验证全解码（scale 未验证可直接用），且硬件 scale 有 1/8 下限，
 * 对缩略图（72px）不可靠。路线：硬件解出原尺寸 RGB565_LE → 软件降采样。
 * 注意：大图内存 = 原尺寸×2（PSRAM 足够，8MB）；缩略图场景原图多 < 8MB。 */

static bool decode_jpeg(uint8_t *data, size_t len, int max_w, int max_h,
                        bool cover, img_decode_result_t *out, const char **err)
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;   /* 直接 RGB565 LE */

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&config, &dec) != JPEG_ERR_OK) {
        if (err) *err = "jpeg open failed";
        return false;
    }

    jpeg_dec_io_t io = {0};
    io.inbuf = data;
    io.inbuf_len = (int)len;

    jpeg_dec_header_info_t info;
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        jpeg_dec_close(dec);
        if (err) *err = "jpeg header";
        return false;
    }

    int iw = info.width, ih = info.height;
    if (iw <= 0 || ih <= 0 || iw > 8192 || ih > 8192) {
        jpeg_dec_close(dec);
        if (err) *err = "jpeg size";
        return false;
    }

    int out_len = iw * ih * 2;
    uint8_t *buf = jpeg_calloc_align(out_len, 16);
    if (!buf) {
        jpeg_dec_close(dec);
        if (err) *err = "jpeg outbuf oom";
        return false;
    }
    io.outbuf = buf;

    bool ok = false;
    if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
        /* 软件缩放（contain 或 cover） */
        int dw, dh;
        fit_size(iw, ih, max_w, max_h, cover, &dw, &dh);
        if (dw == iw && dh == ih) {
            out->pixels = (uint16_t *)buf;
            out->w = iw;
            out->h = ih;
            out->jpeg_buf = true;
            ok = true;
        } else {
            uint16_t *dst = heap_caps_malloc((size_t)dw * dh * 2,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (dst) {
                const uint16_t *src = (const uint16_t *)buf;
                for (int y = 0; y < dh; y++) {
                    int sy = (int)((int64_t)y * ih / dh);
                    for (int x = 0; x < dw; x++) {
                        int sx = (int)((int64_t)x * iw / dw);
                        dst[(size_t)y * dw + x] = src[(size_t)sy * iw + sx];
                    }
                }
                out->pixels = dst;
                out->w = dw;
                out->h = dh;
                out->jpeg_buf = false;
                ok = true;
            } else {
                if (err) *err = "jpeg scale oom";
            }
            jpeg_free_align(buf);
        }
    } else {
        jpeg_free_align(buf);
        if (err) *err = "jpeg decode";
    }
    jpeg_dec_close(dec);
    return ok;
}

/* ── PNG（libpng 逐行 + 最近邻降采样；内存 = 目标 + 1 行） ── */

typedef struct {
    const uint8_t *p;
    size_t left;
} png_src_t;

static void png_read_cb(png_structp png_ptr, png_bytep data, png_size_t length)
{
    png_src_t *st = (png_src_t *)png_get_io_ptr(png_ptr);
    size_t n = st->left < length ? st->left : length;
    memcpy(data, st->p, n);
    st->p += n;
    st->left -= n;
}

static bool decode_png(uint8_t *data, size_t len, int max_w, int max_h,
                       bool cover, img_decode_result_t *out, const char **err)
{
    png_structp pg = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop pi = pg ? png_create_info_struct(pg) : NULL;
    if (!pg || !pi) {
        if (pg) png_destroy_read_struct(&pg, &pi, NULL);
        if (err) *err = "png alloc";
        return false;
    }
    if (setjmp(png_jmpbuf(pg))) {
        png_destroy_read_struct(&pg, &pi, NULL);
        if (err) *err = "png corrupt";
        return false;
    }

    png_src_t st = { data, len };
    png_set_read_fn(pg, &st, png_read_cb);
    png_read_info(pg, pi);

    png_uint_32 w = 0, h = 0;
    int bit_depth = 0, color_type = 0;
    png_get_IHDR(pg, pi, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);
    if (w == 0 || h == 0 || w > 20000 || h > 20000) {
        png_destroy_read_struct(&pg, &pi, NULL);
        if (err) *err = "png size";
        return false;
    }

    /* 标准化为 8-bit RGB（每像素 3 字节，无 alpha 用黑底注释：body 为黑） */
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(pg);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(pg);
    if (png_get_valid(pg, pi, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(pg);
    if (bit_depth == 16) png_set_strip_16(pg);
    png_set_filler(pg, 0xFF, PNG_FILLER_AFTER);   /* 补 alpha=255 → 4 字节 */
    png_set_bgr(pg);                              /* BGR(A) → 我们转 RGB565 用 RGB 顺序 */

    png_read_update_info(pg, pi);
    int row_bytes = (int)png_get_rowbytes(pg, pi);

    int dw, dh;
    fit_size((int)w, (int)h, max_w, max_h, cover, &dw, &dh);
    uint16_t *pix = heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pix) {
        png_destroy_read_struct(&pg, &pi, NULL);
        if (err) *err = "png out oom";
        return false;
    }

    png_bytep row = (png_bytep)heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) {
        heap_caps_free(pix);
        png_destroy_read_struct(&pg, &pi, NULL);
        if (err) *err = "png row oom";
        return false;
    }

    /* 逐行读 + 最近邻竖直降采样 */
    int cur_dy = 0;
    for (png_uint_32 y = 0; y < h; y++) {
        png_read_row(pg, row, NULL);
        int target_y = (int)((int64_t)y * dh / h);
        if (target_y != cur_dy) continue;   /* 该行不属于目标行 */
        cur_dy++;
        /* 水平降采样 */
        const uint8_t *p = row;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * w / dw);
            const uint8_t *px = row + (size_t)sx * 4;
            uint8_t r = px[2], g = px[1], b = px[0];
            pix[(size_t)target_y * dw + x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
        if (cur_dy >= dh) break;
    }

    heap_caps_free(row);
    png_destroy_read_struct(&pg, &pi, NULL);

    out->pixels = pix;
    out->w = dw;
    out->h = dh;
    out->jpeg_buf = false;
    return true;
}

/* ── BMP（24/32bpp 非压缩；向下读取像素区） ── */

static bool decode_bmp(uint8_t *data, size_t len, int max_w, int max_h,
                       bool cover, img_decode_result_t *out, const char **err)
{
    if (len < 54 || data[0] != 'B' || data[1] != 'M') {
        if (err) *err = "bmp sig";
        return false;
    }
    int off = data[10] | (data[11] << 8) | (data[12] << 16) | ((int)data[13] << 24);
    int w = data[18] | (data[19] << 8) | (data[20] << 16) | ((int)data[21] << 24);
    int h = data[22] | (data[23] << 8) | (data[24] << 16) | ((int)data[25] << 24);
    uint16_t bpp = data[28] | (data[29] << 8);
    uint32_t comp = data[30] | (data[31] << 8) | (data[32] << 16) | ((uint32_t)data[33] << 24);
    if (w <= 0 || h == 0 || (bpp != 24 && bpp != 32) || comp != 0) {
        if (err) *err = "bmp unsupported";
        return false;
    }
    bool flip = h > 0;   /* 正高度 = 底部向上存储 */
    uint32_t ah = (uint32_t)(h < 0 ? -h : h);
    if (ah > 20000 || (size_t)off >= len) {
        if (err) *err = "bmp size";
        return false;
    }
    int row_stride = (int)(((w * bpp / 8) + 3) & ~3);
    if (off + row_stride * ah > (int)len + 4) {   /* 防御越界（允许轻微） */
        if (err) *err = "bmp short";
        return false;
    }

    int dw, dh;
    fit_size(w, (int)ah, max_w, max_h, cover, &dw, &dh);
    uint16_t *pix = heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pix) {
        if (err) *err = "bmp oom";
        return false;
    }
    const uint8_t *px = data + off;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * (int)ah / dh);
        int line = flip ? (int)ah - 1 - sy : sy;
        const uint8_t *rowp = px + (size_t)line * row_stride;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * w / dw);
            const uint8_t *q = rowp + (size_t)sx * (bpp / 8);
            uint8_t b = q[0], g = q[1], r = q[2];
            pix[(size_t)y * dw + x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    out->pixels = pix;
    out->w = dw;
    out->h = dh;
    out->jpeg_buf = false;
    return true;
}

/* ── 公共入口 ── */

/* 内部核心：按 cover 标志分派解码（img_decode_file = contain；_cover = cover） */
static bool img_decode_internal(const char *path, int max_w, int max_h,
                                bool cover, img_decode_result_t *out, const char **err)
{
    if (!path || !out) return false;
    out->pixels = NULL;
    out->w = out->h = 0;

    const char *dot = strrchr(path, '.');
    if (!dot) {
        if (err) *err = "no ext";
        return false;
    }
    char ext[8];
    strncpy(ext, dot + 1, sizeof(ext) - 1);
    ext[sizeof(ext) - 1] = '\0';
    for (char *p = ext; *p; p++) *p = (char)tolower((unsigned char)*p);

    uint8_t *data = NULL;
    size_t len = 0;
    if (!read_file(path, &data, &len)) {
        if (err) *err = "read fail";
        return false;
    }

    bool ok = false;
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) {
        ok = decode_jpeg(data, len, max_w, max_h, cover, out, err);
    } else if (strcmp(ext, "png") == 0) {
        ok = decode_png(data, len, max_w, max_h, cover, out, err);
    } else if (strcmp(ext, "bmp") == 0) {
        ok = decode_bmp(data, len, max_w, max_h, cover, out, err);
    } else {
        if (err) *err = "unsupported";
    }
    heap_caps_free(data);
    return ok;
}

bool img_decode_file(const char *path, int max_w, int max_h,
                     img_decode_result_t *out, const char **err)
{
    return img_decode_internal(path, max_w, max_h, false, out, err);
}

bool img_decode_file_cover(const char *path, int max_w, int max_h,
                           img_decode_result_t *out, const char **err)
{
    return img_decode_internal(path, max_w, max_h, true, out, err);
}

void img_decode_free(img_decode_result_t *r)
{
    if (!r || !r->pixels) return;
    if (r->jpeg_buf) {
        jpeg_free_align(r->pixels);   /* esp_new_jpeg 对齐缓冲专用释放 */
    } else {
        heap_caps_free(r->pixels);
    }
    r->pixels = NULL;
    r->w = r->h = 0;
    r->jpeg_buf = false;
}