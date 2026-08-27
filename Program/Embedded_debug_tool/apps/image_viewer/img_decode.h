#ifndef IMG_DECODE_H
#define IMG_DECODE_H

/* img_decode —— 图片解码公共模块（apps/image_viewer）。
 * 统一入口：按扩展名分派 → 解码并缩放到目标矩形（宽高适配），输出 RGB565。
 *   JPG/JPEG —— esp_new_jpeg（S3 硬件 JPEG 引擎，解码时 scale，输出 RGB565_LE）
 *   PNG      —— libpng 逐行读取 + 边读边降采样（内存 = 目标帧 + 单行缓冲）
 *   BMP      —— 简易解析（24/32bpp 非压缩；BMP 头 + 像素转换 RGB565）
 * 内存：输出缓冲 = 目标宽×高×2（PSRAM 分配）；解码过程峰值 ≈ 输出缓冲 + 行缓冲。
 */

#include "lvgl.h"
#include <stdbool.h>

/* 解码结果：RGB565 LE 像素 + 实际尺寸（填充目标框后的绘图用）
 * stride = w*2。alpha 无（BMP/JPEG/PNG 的 alpha 舍弃或合成黑底）。 */
typedef struct {
    uint16_t *pixels;   /* 目标尺寸 RGB565 LE（PSRAM，img_free 释放） */
    int w, h;           /* 目标输出宽高 */
    bool jpeg_buf;      /* true=JPEG 缓冲（jpeg_free_align 释放）；false=heap_caps_free */
} img_decode_result_t;

/* 解码文件到目标尺寸（保持纵横比 contain，每维 ≤ max_w/max_h，at least 1）。
 * 返回 true=成功；false=失败（资源/格式/内存）。失败原因可查 err（可 NULL）。 */
bool img_decode_file(const char *path, int max_w, int max_h,
                     img_decode_result_t *out, const char **err);

/* 解码到"封面全屏"尺寸：等比缩放使至少一边等于 max_w/max_h（可放大），
 * 输出完整缩放图（另一边可能超出 max），由调用方居中 + 裁剪显示。
 * 用于图片全屏查看（无黑边）。 */
bool img_decode_file_cover(const char *path, int max_w, int max_h,
                           img_decode_result_t *out, const char **err);

/* 缩略图缓存：把解码结果写 SD 缓存（/sdcard/.ivthumbs/<fnv>.thb），
 * 下次可用 img_decode_cached 直接命中。data 为 RGB565 LE + w/h。 */
bool img_decode_cache_write(const char *src_path, const uint16_t *pixels, int w, int h);

/* 读缩略图缓存：命中返回 true 且 out 指向新分配像素（heap_caps，img_decode_free 释放）；
 * miss 返回 false（out->pixels 保持 NULL）。内部含最大尺寸校验（缩略图场景）。 */
bool img_decode_cache_read(const char *src_path, img_decode_result_t *out);

/* 释放解码结果（pixels 归零） */
void img_decode_free(img_decode_result_t *r);

#endif /* IMG_DECODE_H */