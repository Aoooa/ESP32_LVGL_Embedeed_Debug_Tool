/* image_viewer.c —— 图片浏览器：SD 扫描（按日期排序）+ 缩略图分页 + 全屏查看。
 * 复用 img_decode 解码；目录递归用 POSIX dirent（FATFS VFS）。
 */

#include "image_viewer.h"
#include "img_decode.h"
#include "launcher.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_jpeg_dec.h"
#include "misc/lv_timer_private.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#define IV_TAG "image_viewer"

/* ── 布局/常量 ── */
#define IV_IMG_MAX      512            /* 扫描图片数上限 */
#define IV_PATH_MAX     260
#define IV_THUMB_W      72             /* 缩略图尺寸 */
#define IV_THUMB_H      72
#define IV_PAGE_BAR_H   32             /* 页码栏高 */
#define IV_COLS         1              /* 每行 1 列：左缩略图 + 右文件名 */
#define IV_ROWS         3              /* 每页 3 行 */
#define IV_ROW_H        (IV_THUMB_H + 8)  /* 行高（缩略图 + 间距） */
#define IV_NAME_W       (240 - 12 - IV_THUMB_W - 16)  /* 名字区宽（缩略图右侧） */

/* 配色（与工程一致） */
#define IV_BG        0x000000
#define IV_PANEL     0x0E141C
#define IV_BORDER    0x1F2A36
#define IV_TEXT      0xE6F0EE
#define IV_HI        0x7FE5DC
#define IV_GREEN     0x39C5BB
#define IV_DIM       0x94A3B8
#define IV_BTN_TXT   0x39C5BB

/* 图片条目 */
typedef struct {
    char path[IV_PATH_MAX];
    time_t mtime;
} iv_entry_t;

/* 缩略图解码缓冲*/
typedef struct {
    lv_image_dsc_t dsc;
    uint16_t *pixels;
    bool jpeg_buf;
} iv_thumb_t;

typedef enum { IV_MODE_BROWSE = 0, IV_MODE_VIEW } iv_mode_t;

typedef struct {
    lv_obj_t *root;
    image_viewer_back_cb_t back_cb;
    void *ctx;

    iv_mode_t mode;
    iv_entry_t *entries;
    int entry_count;
    int page;                /* 当前页（0-based） */
    int pages;

    /* 浏览模式 UI */
    lv_obj_t *grid;          /* 缩略图网格容器（滚动，但用固定页） */
    lv_obj_t *page_lbl;
    lv_obj_t *btn_prev, *btn_next;

    /* 查看模式 UI */
    lv_obj_t *view;          /* 全屏容器 */
    lv_obj_t *view_img;      /* 图像对象 */
    lv_obj_t *view_info;     /* 文件名+页码 */
    int view_idx;            /* 当前查看的条目索引 */
    img_decode_result_t view_dec;
    lv_image_dsc_t view_dsc; /* 查看模式的图像描述（独立，非静态） */

    /* 缩略图解码缓冲（每实例独立，随 APP 生命周期） */
    iv_thumb_t *thumbs;      /* IV_COLS*IV_ROWS 个（懒分配） */
    int thumb_n;

    /* 异步缩略图解码（lv_timer 分步，快渲染：先建框+提示，再逐张解码刷新） */
    lv_timer_t *load_timer;
    int load_start;          /* 本页起始条目索引 */
    int load_next;           /* 下一个待解码条目（绝对索引） */
    int load_end;            /* 本页结束条目（不含） */
    lv_obj_t *loading;       /* 半透明加载提示层（NULL=无） */

    bool arg_open;           /* 带参启动：直接查看该文件 */
} iv_t;

static iv_t *s_iv;

static int iv_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_horizontal_resolution(d) : 240;
}
static int iv_screen_h(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? lv_display_get_vertical_resolution(d) : 320;
}

/* ── 扫描：递归收集图片（锁内：SD/LCD 共享 SPI） ── */

static bool iv_is_image(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    const char *ext = dot + 1;
    return strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
           strcasecmp(ext, "png") == 0 || strcasecmp(ext, "bmp") == 0;
}

static void iv_scan_dir(const char *dir, int depth)
{
    iv_t *iv = s_iv;
    if (depth > 4 || iv->entry_count >= IV_IMG_MAX) return;

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && iv->entry_count < IV_IMG_MAX) {
        if (de->d_name[0] == '.') continue;   /* 跳过隐藏/./.. */
        if (strcmp(de->d_name, "System Volume Information") == 0) continue;
        char full[IV_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            iv_scan_dir(full, depth + 1);
        } else if (S_ISREG(st.st_mode) && iv_is_image(de->d_name)) {
            iv_entry_t *e = &iv->entries[iv->entry_count++];
            strncpy(e->path, full, sizeof(e->path) - 1);
            e->path[sizeof(e->path) - 1] = '\0';
            e->mtime = st.st_mtime;
        }
    }
    closedir(d);
}

static int iv_cmp(const void *a, const void *b)
{
    const iv_entry_t *ea = a, *eb = b;
    if (ea->mtime > eb->mtime) return -1;      /* 新 → 旧 */
    if (ea->mtime < eb->mtime) return 1;
    return strcasecmp(ea->path, eb->path);
}

/* ── 缩略图页渲染（懒解码：只解码当前页 9 张） ── */

static void iv_show_page(iv_t *iv);
static void iv_build_browse(iv_t *iv);
static void iv_build_view(iv_t *iv);
static void iv_close_view(iv_t *iv);

/* ── 缩略图解码缓冲管理（每实例独立，杜绝跨实例全局残留） ── */

static void iv_thumbs_init(iv_t *iv)
{
    if (!iv->thumbs) {
        iv->thumbs = calloc(IV_COLS * IV_ROWS, sizeof(iv_thumb_t));
    }
    iv->thumb_n = 0;
}

static void iv_thumbs_clear(iv_t *iv)
{
    if (!iv->thumbs) return;
    for (int i = 0; i < IV_COLS * IV_ROWS; i++) {
        if (iv->thumbs[i].pixels) {
            if (iv->thumbs[i].jpeg_buf) jpeg_free_align(iv->thumbs[i].pixels);
            else heap_caps_free(iv->thumbs[i].pixels);
            iv->thumbs[i].pixels = NULL;
            memset(&iv->thumbs[i].dsc, 0, sizeof(iv->thumbs[i].dsc));
        }
    }
    iv->thumb_n = 0;
}

static lv_obj_t *iv_make_thumb_box(lv_obj_t *parent, int idx)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_size(t, IV_THUMB_W, IV_THUMB_H);
    lv_obj_set_style_bg_color(t, lv_color_hex(IV_PANEL), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(IV_BORDER), 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_radius(t, 4, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_set_user_data(t, (void *)(intptr_t)idx);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    return t;
}

/* 加载提示层：半透明黑 + 白 spinner（LVGL 9: lv_spinner_create） */
static void iv_show_loading(iv_t *iv, bool show, const char *msg)
{
    if (show && !iv->loading) {
        lv_obj_t *m = lv_obj_create(iv->root);
        lv_obj_set_size(m, lv_pct(100), lv_pct(100));
        lv_obj_set_pos(m, 0, 0);
        lv_obj_set_style_bg_color(m, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_50, 0);
        lv_obj_set_style_border_width(m, 0, 0);
        lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(m, 0, 0);
        lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *sp = lv_spinner_create(m);
        lv_obj_set_size(sp, 40, 40);
        lv_obj_center(sp);
        lv_spinner_set_anim_params(sp, 900, 60);

        lv_obj_t *lbl = lv_label_create(m);
        lv_label_set_text(lbl, msg ? msg : "Loading...");
        lv_obj_set_style_text_color(lbl, lv_color_hex(IV_HI), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 34);
        iv->loading = m;
        lv_obj_move_foreground(m);
    } else if (!show && iv->loading) {
        lv_obj_delete(iv->loading);
        iv->loading = NULL;
    }
}

/* ── 异步缩略图加载（每 tick 解码 1 张，期间 loading 提示可刷新） ── */

static void iv_thumb_click(lv_event_t *e)
{
    iv_t *iv = s_iv;
    if (!iv) return;
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx < 0 || idx >= iv->entry_count) return;
    iv->mode = IV_MODE_VIEW;
    iv->view_idx = idx;
    iv_show_page(iv);
}

/* 解码一张缩略图到指定 thumb 槽（缓存命中直接读；miss 解码+写缓存） */
static bool iv_decode_thumb(iv_t *iv, int entry_idx, iv_thumb_t *th)
{
    iv_entry_t *e = &iv->entries[entry_idx];
    img_decode_result_t dec;

    /* 1. 试缓存 */
    if (img_decode_cache_read(e->path, &dec)) {
        th->pixels = dec.pixels;
        th->jpeg_buf = dec.jpeg_buf;
        memset(&th->dsc, 0, sizeof(th->dsc));
        th->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        th->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        th->dsc.header.w = dec.w;
        th->dsc.header.h = dec.h;
        th->dsc.header.stride = dec.w * 2;
        th->dsc.data_size = dec.w * dec.h * 2;
        th->dsc.data = (const uint8_t *)dec.pixels;
        return true;
    }

    /* 2. 实时解码 */
    if (!img_decode_file(e->path, IV_THUMB_W, IV_THUMB_H, &dec, NULL)) {
        return false;
    }
    /* 3. 写缓存（缩略图固定 72x72 内，安全） */
    if (dec.w <= IV_THUMB_W && dec.h <= IV_THUMB_H) {
        img_decode_cache_write(e->path, dec.pixels, dec.w, dec.h);
    }
    th->pixels = dec.pixels;
    th->jpeg_buf = dec.jpeg_buf;
    memset(&th->dsc, 0, sizeof(th->dsc));
    th->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    th->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    th->dsc.header.w = dec.w;
    th->dsc.header.h = dec.h;
    th->dsc.header.stride = dec.w * 2;
    th->dsc.data_size = dec.w * dec.h * 2;
    th->dsc.data = (const uint8_t *)dec.pixels;
    return true;
}

/* 逐张解码入口：先建 loading + 空框，timer 后续解码每张并替换 "?" 为图。
 * 行结构：row 容器（flex row） = 左 thumb box + 右文件名 label（DOT 省略） */
static void iv_page_load_tick(lv_timer_t *t)
{
    iv_t *iv = t->user_data;
    if (!iv || iv->mode != IV_MODE_BROWSE) {
        lv_timer_delete(t);
        iv->load_timer = NULL;
        iv_show_loading(iv, false, NULL);
        return;
    }
    /* 找到下一个待解码条目（跳过无框的） */
    if (iv->load_next >= iv->load_end) {
        lv_timer_delete(t);
        iv->load_timer = NULL;
        iv_show_loading(iv, false, NULL);
        return;
    }
    int idx = iv->load_next++;
    /* 该条目在本页网格中的 thumb 槽 = idx - load_start */
    if (idx - iv->load_start < 0 || idx - iv->load_start >= IV_COLS * IV_ROWS) return;
    iv_thumb_t *th = &iv->thumbs[idx - iv->load_start];
    if (iv_decode_thumb(iv, idx, th)) {
        /* 找对应 row（grid 子对象 user_data == idx），更新其 box 内图片 */
        uint32_t n = lv_obj_get_child_count(iv->grid);
        for (uint32_t ci = 0; ci < n; ci++) {
            lv_obj_t *row = lv_obj_get_child(iv->grid, ci);
            if ((int)(intptr_t)lv_obj_get_user_data(row) != idx) continue;
            /* row 的第一个子对象 = thumb box（清 "?" 换图） */
            lv_obj_t *box = lv_obj_get_child(row, 0);
            if (box) {
                lv_obj_clean(box);
                lv_obj_t *im = lv_image_create(box);
                lv_image_set_src(im, &th->dsc);
                lv_obj_center(im);
            }
            break;
        }
    }
    /* 继续：一个 tick 一帧解码（LVGL 有足够时间渲染 loading） */
}

/* 建本页行列表 + 启动异步加载。每行：左缩略图 box(72x72) + 右文件名 label */
static void iv_rebuild_page_grid(iv_t *iv)
{
    if (!iv->grid) return;
    lv_obj_clean(iv->grid);
    iv_thumbs_clear(iv);
    iv_thumbs_init(iv);

    int sw = iv_screen_w();
    int start = iv->page * (IV_COLS * IV_ROWS);
    iv->load_start = start;
    iv->load_next = start;
    iv->load_end = start + IV_COLS * IV_ROWS;
    if (iv->load_end > iv->entry_count) iv->load_end = iv->entry_count;

    /* 先铺空行（thumb box "?" + 文件名），同时显示 loading。
     * 行 = button（可点击整行）+ 底部下划线（列表感）；贴左贴顶（row 从 (0,0) 起） */
    iv_show_loading(iv, true, "Loading thumbs...");
    for (int r = 0; r < IV_ROWS && (start + r) < iv->entry_count; r++) {
        int idx = start + r;
        if (idx >= iv->entry_count) break;

        lv_obj_t *row = lv_button_create(iv->grid);
        lv_obj_set_pos(row, 0, r * IV_ROW_H);
        lv_obj_set_size(row, sw, IV_THUMB_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(IV_PANEL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(IV_BORDER), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);   /* 底部下划线 */
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)idx);
        lv_obj_add_event_cb(row, iv_thumb_click, LV_EVENT_CLICKED, iv);
        /* 按键按压缩放效果取消（保持列表稳定） */
        lv_obj_set_style_transform_width(row, 0, LV_STATE_PRESSED);
        lv_obj_set_style_transform_height(row, 0, LV_STATE_PRESSED);

        /* 左：缩略图 box（72x72，占位 "?"）——紧贴行左缘 */
        lv_obj_t *box = iv_make_thumb_box(row, idx);
        lv_obj_set_pos(box, 4, 2);
        lv_obj_t *x = lv_label_create(box);
        lv_label_set_text(x, "?");
        lv_obj_set_style_text_color(x, lv_color_hex(IV_DIM), 0);
        lv_obj_center(x);

        /* 右：文件名 label（DOTS 省略，超出加 …）垂直居中，贴缩略图右侧 */
        const char *name = strrchr(iv->entries[idx].path, '/');
        name = name ? name + 1 : iv->entries[idx].path;
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, name);
        lv_obj_set_style_text_color(nm, lv_color_hex(IV_TEXT), 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
        lv_obj_set_width(nm, sw - IV_THUMB_W - 20);
        lv_obj_set_pos(nm, IV_THUMB_W + 10, 0);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_top(nm, IV_THUMB_H / 2 - 8, 0);   /* 垂直居中（约 12px 字） */
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);    /* 超长省略 */
    }

    /* 启动分步解码 timer（每轮 1 张；若已有 timer 先停旧） */
    if (iv->load_timer) lv_timer_delete(iv->load_timer);
    iv->load_timer = lv_timer_create(iv_page_load_tick, 30, iv);
}

/* 释放当前页所有 img 解码缓冲（lv_obj_clean 不会释放 image 数据）。
 * 由 iv_thumbs_clear 统一管理（见上）。 */

/* ── 页切换 ── */

static void iv_show_page(iv_t *iv)
{
    if (iv->mode == IV_MODE_BROWSE) {
        if (iv->entry_count == 0) {
            if (iv->page_lbl) lv_label_set_text(iv->page_lbl, "no images");
            if (iv->grid) lv_obj_clean(iv->grid);
            return;
        }
        if (iv->page >= iv->pages) iv->page = iv->pages - 1;
        if (iv->page < 0) iv->page = 0;
        iv_rebuild_page_grid(iv);
        if (iv->page_lbl) {
            char b[16];
            snprintf(b, sizeof(b), "%d/%d", iv->page + 1, iv->pages);
            lv_label_set_text(iv->page_lbl, b);
        }
        /* 翻页按钮状态 */
        lv_obj_clear_state(iv->btn_prev, LV_STATE_DISABLED);
        lv_obj_clear_state(iv->btn_next, LV_STATE_DISABLED);
        if (iv->page <= 0) lv_obj_add_state(iv->btn_prev, LV_STATE_DISABLED);
        if (iv->page >= iv->pages - 1) lv_obj_add_state(iv->btn_next, LV_STATE_DISABLED);
    } else {
        /* 查看模式：确保 view UI 已构建（浏览模式进入时无 view 对象） */
        if (!iv->view_img) {
            iv_build_view(iv);
        }
        if (iv->view_idx < 0 || iv->view_idx >= iv->entry_count) { iv->mode = IV_MODE_BROWSE; return; }
        /* 先释放旧解码，再显示加载提示并强制渲染一帧（避免解码卡顿无反馈），
         * 然后 cover 解码全屏（等比填满、裁剪由屏幕完成） */
        img_decode_free(&iv->view_dec);
        memset(&iv->view_dsc, 0, sizeof(iv->view_dsc));
        iv_show_loading(iv, true, "Loading...");
        lv_refr_now(NULL);   /* 先渲染 loading 帧，再解码 */

        if (!img_decode_file_cover(iv->entries[iv->view_idx].path,
                                   iv_screen_w(), iv_screen_h(),
                                   &iv->view_dec, NULL)) {
            iv_show_loading(iv, false, NULL);
            if (iv->view_info) lv_label_set_text(iv->view_info, "decode failed");
            return;
        }
        iv_show_loading(iv, false, NULL);
        if (iv->view_img) {
            iv->view_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            iv->view_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            iv->view_dsc.header.w = iv->view_dec.w;
            iv->view_dsc.header.h = iv->view_dec.h;
            iv->view_dsc.header.stride = iv->view_dec.w * 2;
            iv->view_dsc.data_size = iv->view_dec.w * iv->view_dec.h * 2;
            iv->view_dsc.data = (const uint8_t *)iv->view_dec.pixels;
            lv_image_set_src(iv->view_img, &iv->view_dsc);
            lv_obj_center(iv->view_img);   /* 居中，超出屏幕部分被裁剪 */
        }
        if (iv->view_info) {
            const char *name = strrchr(iv->entries[iv->view_idx].path, '/');
            name = name ? name + 1 : iv->entries[iv->view_idx].path;
            char b[160];
            snprintf(b, sizeof(b), "%.80s  %d/%d", name, iv->view_idx + 1, iv->entry_count);
            lv_label_set_text(iv->view_info, b);
        }
    }
}

/* ── 事件 ── */

static void iv_page_prev(lv_event_t *e)
{
    (void)e;
    iv_t *iv = s_iv;
    if (!iv || iv->page <= 0) return;
    iv->page--;
    iv_show_page(iv);
}

static void iv_page_next(lv_event_t *e)
{
    (void)e;
    iv_t *iv = s_iv;
    if (!iv || iv->page >= iv->pages - 1) return;
    iv->page++;
    iv_show_page(iv);
}

/* 查看模式：点击切图（上一张/下一张按左右半区） */
static void iv_view_click(lv_event_t *e)
{
    (void)e;
    iv_t *iv = s_iv;
    if (!iv || iv->mode != IV_MODE_VIEW) return;
    /* 右侧 = 下一张，左侧 = 上一张（简单：整个查看区点击 = 下一张，右滑返回） */
    if (iv->view_idx + 1 < iv->entry_count) {
        iv->view_idx++;
        iv_show_page(iv);
    }
}

/* ── 创建 ── */

static void iv_build_browse(iv_t *iv)
{
    lv_obj_t *root = iv->root;
    int sw = iv_screen_w(), sh = iv_screen_h();

    /* 模式切换：先释放查看模式缓冲 + 清悬垂引用（本对象树将删） */
    img_decode_free(&iv->view_dec);
    iv->view = NULL;
    iv->view_img = NULL;
    iv->view_info = NULL;

    lv_obj_clean(root);

    /* 标题栏：APP 名 + 图片计数 */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, sw, 24);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tl = lv_label_create(bar);
    char tb[32];
    snprintf(tb, sizeof(tb), "Photos (%d)", iv->entry_count);
    lv_label_set_text(tl, tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(IV_HI), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);

    /* 缩略图网格 */
    lv_obj_t *grid = lv_obj_create(root);
    lv_obj_set_size(grid, sw, sh - 24 - IV_PAGE_BAR_H);
    lv_obj_set_pos(grid, 0, 24);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    /* 贴边：grid 自身 padding 归零，行贴左贴顶 */
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(grid, iv);
    iv->grid = grid;

    /* 页码栏 */
    lv_obj_t *pbar = lv_obj_create(root);
    lv_obj_set_size(pbar, sw, IV_PAGE_BAR_H);
    lv_obj_set_pos(pbar, 0, sh - IV_PAGE_BAR_H);
    lv_obj_set_style_bg_color(pbar, lv_color_hex(IV_PANEL), 0);
    lv_obj_set_style_border_color(pbar, lv_color_hex(IV_BORDER), 0);
    lv_obj_set_style_border_width(pbar, 1, 0);
    lv_obj_set_style_border_side(pbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(pbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(pbar, 10, 0);
    lv_obj_clear_flag(pbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev = lv_button_create(pbar);
    lv_obj_set_size(prev, 48, 22);
    lv_obj_set_style_bg_color(prev, lv_color_hex(IV_PANEL), 0);
    lv_obj_set_style_border_color(prev, lv_color_hex(IV_GREEN), 0);
    lv_obj_set_style_border_width(prev, 1, 0);
    lv_obj_set_style_radius(prev, 4, 0);
    lv_obj_set_style_pad_all(prev, 0, 0);
    lv_obj_t *pl = lv_label_create(prev);
    lv_label_set_text(pl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(pl, lv_color_hex(IV_BTN_TXT), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_12, 0);
    lv_obj_center(pl);
    lv_obj_add_event_cb(prev, iv_page_prev, LV_EVENT_CLICKED, iv);
    iv->btn_prev = prev;

    iv->page_lbl = lv_label_create(pbar);
    lv_obj_set_style_text_color(iv->page_lbl, lv_color_hex(IV_HI), 0);
    lv_obj_set_style_text_font(iv->page_lbl, &lv_font_montserrat_12, 0);

    lv_obj_t *next = lv_button_create(pbar);
    lv_obj_set_size(next, 48, 22);
    lv_obj_set_style_bg_color(next, lv_color_hex(IV_PANEL), 0);
    lv_obj_set_style_border_color(next, lv_color_hex(IV_GREEN), 0);
    lv_obj_set_style_border_width(next, 1, 0);
    lv_obj_set_style_radius(next, 4, 0);
    lv_obj_set_style_pad_all(next, 0, 0);
    lv_obj_t *nl = lv_label_create(next);
    lv_label_set_text(nl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nl, lv_color_hex(IV_BTN_TXT), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_12, 0);
    lv_obj_center(nl);
    lv_obj_add_event_cb(next, iv_page_next, LV_EVENT_CLICKED, iv);
    iv->btn_next = next;
}

static void iv_build_view(iv_t *iv)
{
    lv_obj_t *root = iv->root;
    int sw = iv_screen_w(), sh = iv_screen_h();

    /* 覆盖层方案：不 clean root —— 下方保留浏览界面（缩略图），
     * 右滑拖动时拖 view 层露出相册（与阅读器书架模式一致） */
    /* 先删旧的覆盖层（rotate 重建场景） */
    img_decode_free(&iv->view_dec);
    if (iv->view) {
        lv_obj_delete(iv->view);
        iv->view = NULL;
    }
    iv->view_img = NULL;
    iv->view_info = NULL;
    /* 注意：不清理 iv->grid/page_lbl/btn_prev/btn_next —— 下方浏览界面一直保留，
     * 返回相册时直接复用这些引用（不重建，避免缩略图重新加载） */

    /* 全屏覆盖层容器（黑底铺满） */
    lv_obj_t *v = lv_obj_create(root);
    lv_obj_set_size(v, sw, sh);
    lv_obj_set_pos(v, 0, 0);
    lv_obj_set_style_bg_color(v, lv_color_hex(IV_BG), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(v, iv_view_click, LV_EVENT_CLICKED, iv);
    iv->view = v;

    lv_obj_t *img = lv_image_create(v);
    iv->view_img = img;
    lv_obj_center(img);

    /* 文件名+序号：右下角半透明深色底 + 亮色文字 */
    lv_obj_t *info = lv_label_create(v);
    lv_obj_set_style_text_color(info, lv_color_hex(IV_HI), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(info, 6, 0);
    lv_obj_set_style_pad_ver(info, 2, 0);
    lv_obj_set_style_radius(info, 4, 0);
    lv_obj_set_pos(info, sw - 6, sh - 16);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_update_layout(info);   /* 对齐到右下角 */
    lv_obj_align(info, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    iv->view_info = info;

    lv_obj_move_foreground(v);
}

lv_obj_t *image_viewer_create(lv_obj_t *parent, image_viewer_back_cb_t back_cb, void *ctx)
{
    iv_t *iv = calloc(1, sizeof(iv_t));
    if (!iv) return NULL;
    iv->back_cb = back_cb;
    iv->ctx = ctx;
    s_iv = iv;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(IV_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    iv->root = root;

    /* 分配条目表（PSRAM 可，但扫描/排序小，heap 即可） */
    iv->entries = malloc(IV_IMG_MAX * sizeof(iv_entry_t));
    if (!iv->entries) {
        lv_obj_delete(root);
        free(iv);
        s_iv = NULL;
        return NULL;
    }
    iv->entry_count = 0;

    /* 扫描前先建 UI（查看模式由带参路径决定） */
    const char *arg = launcher_app_get_arg();
    if (arg && *arg) {
        /* 带参：直接查看该文件——先把它加为条目。
         * arg_open=true：右滑返回时直接关闭本 APP（回文件浏览器），不进图库 */
        struct stat st;
        if (stat(arg, &st) == 0 && S_ISREG(st.st_mode) && iv_is_image(strrchr(arg, '/'))) {
            strncpy(iv->entries[0].path, arg, sizeof(iv->entries[0].path) - 1);
            iv->entries[0].path[sizeof(iv->entries[0].path) - 1] = '\0';
            iv->entries[0].mtime = st.st_mtime;
            iv->entry_count = 1;
            iv->mode = IV_MODE_VIEW;
            iv->view_idx = 0;
            iv->arg_open = true;
            iv_build_view(iv);
            iv_show_page(iv);
            return root;
        }
    }

    /* 浏览模式：扫描 SD */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        iv_scan_dir("/sdcard", 0);
        esp_lv_adapter_unlock();
    }
    qsort(iv->entries, iv->entry_count, sizeof(iv_entry_t), iv_cmp);
    iv->pages = (iv->entry_count + IV_COLS * IV_ROWS - 1) / (IV_COLS * IV_ROWS);
    if (iv->pages < 1) iv->pages = 1;
    iv->page = 0;
    iv->mode = IV_MODE_BROWSE;

    iv_build_browse(iv);
    iv_show_page(iv);
    ESP_LOGI(IV_TAG, "scanned %d images", iv->entry_count);
    return root;
}

void image_viewer_destroy(lv_obj_t *root)
{
    iv_t *iv = s_iv;
    if (iv) {
        if (iv->load_timer) { lv_timer_delete(iv->load_timer); iv->load_timer = NULL; }
        iv_show_loading(iv, false, NULL);
        iv_thumbs_clear(iv);
        if (iv->thumbs) { free(iv->thumbs); iv->thumbs = NULL; }
        img_decode_free(&iv->view_dec);
        if (iv->entries) free(iv->entries);
    }
    s_iv = NULL;
    if (root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(root);
    }
    free(iv);
}

/* 关闭查看覆盖层：只删除覆盖层（下方浏览界面一直保留，直接复用其引用，
 * 不重建 → 返回相册不重载缩略图）。root x 由调用方归零。 */
static void iv_close_view(iv_t *iv)
{
    if (iv->load_timer) { lv_timer_delete(iv->load_timer); iv->load_timer = NULL; }
    iv_show_loading(iv, false, NULL);
    img_decode_free(&iv->view_dec);
    if (iv->view) {
        lv_obj_delete(iv->view);
        iv->view = NULL;
    }
    iv->view_img = NULL;
    iv->view_info = NULL;
    iv->mode = IV_MODE_BROWSE;
    lv_obj_set_x(iv->root, 0);
}

bool image_viewer_swipe_back(lv_obj_t *root)
{
    (void)root;
    iv_t *iv = s_iv;
    if (!iv) return true;
    if (iv->mode == IV_MODE_VIEW) {
        if (iv->arg_open) {
            /* 带参启动（文件浏览器点图）：右滑 = 关闭本 APP 返回浏览器 */
            return true;
        }
        /* 图库查看：返回 true 放行 launcher 拖动（拖 view 覆盖层，
         * 露出下方相册；滑出时 drag_exit 关覆盖层回相册） */
        iv->mode = IV_MODE_BROWSE;   /* 先置回浏览，drag_exit 判模式 */
        img_decode_free(&iv->view_dec);
        return true;
    }
    return true;   /* 浏览模式：关闭 APP */
}

/* 拖动目标：查看模式拖 view 覆盖层（露出相册）；否则整 root。
 * arg_open（从文件浏览器进入）时拖 root 本身 → 露出下层文件浏览器 */
lv_obj_t *image_viewer_drag_root(void *app)
{
    (void)app;
    iv_t *iv = s_iv;
    if (!iv) return NULL;
    if (iv->arg_open) return iv->root;         /* 整 app 平移，露出来源 APP */
    return iv->view ? iv->view : iv->root;     /* 图库查看：拖覆盖层露出相册 */
}

/* 拖动滑出完成：查看覆盖层 → 关覆盖层回相册；否则关闭 APP */
void image_viewer_drag_exit(void *app)
{
    (void)app;
    iv_t *iv = s_iv;
    if (!iv) {
        launcher_app_close(NULL);
        return;
    }
    if (iv->arg_open) {
        /* 从文件浏览器进入：滑出 = 关闭 APP 返回浏览器 */
        launcher_app_close(NULL);
        return;
    }
    if (iv->mode == IV_MODE_BROWSE && iv->view) {
        /* 从查看滑出：关覆盖层回相册（覆盖层 x 已滑出屏幕，无需动画） */
        iv_close_view(iv);
        lv_obj_set_x(iv->root, 0);
        return;
    }
    launcher_app_close(NULL);
}

void image_viewer_rotate(lv_obj_t *root, int deg)
{
    (void)deg;
    iv_t *iv = s_iv;
    if (!iv) return;
    if (iv->mode == IV_MODE_VIEW) {
        /* 覆盖层重建（下层 browse 尺寸过时，回相册时由 iv_show_page 重建） */
        iv->mode = IV_MODE_BROWSE;   /* 先回到浏览语义，让 view 重建为覆盖层 */
        iv_build_view(iv);
        iv->mode = IV_MODE_VIEW;
        iv_show_page(iv);
    } else {
        iv_build_browse(iv);
        iv_show_page(iv);
    }
}

void image_viewer_debug_event(lv_obj_t *root, int evt)
{
    (void)root;
    (void)evt;
    iv_t *iv = s_iv;
    if (!iv) return;
    ESP_LOGI(IV_TAG, "[DBG] mode=%d entries=%d page=%d/%d view=%d",
             iv->mode, iv->entry_count, iv->page + 1, iv->pages, iv->view_idx);
}