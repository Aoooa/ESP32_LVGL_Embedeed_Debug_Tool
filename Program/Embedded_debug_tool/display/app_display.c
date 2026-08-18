#include "app_display.h"
#include "drv_display.h"
#include "app_uart.h"
#include "esp_lv_adapter.h"
#include "flow_view.h"
#include "launcher.h"
#include "gesture.h"
#include "terminal.h"
#include "app_font.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_display";

/* ── flow_view 内存/锁钩�?── */
static void *fv_psram_alloc(size_t size)
{
    return heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void fv_acquire(void)
{
    esp_lv_adapter_lock(-1);
}

/* ── Layout ── */

/* ── State ── */
static int s_orientation_deg;   /* 当前逻辑方向 0/90/180/270（0=竖屏 240x320） */
static lv_obj_t *s_launcher;    /* 桌面启动器 root */


/* ── 硬件滚动接口（保留：驱动�?0x33/0x37 命令；完整滚动（flush 映射）留待未来优化） ──
 * 注意：需组合 A（硬件竖�?swap=false）时 0x37 �?= 屏幕垂直�?
 * app_display_hw_scroll_set：同�?0x37 偏移；app_display_hw_scroll_enable：设置滚动区�?
 * 当前阅读器未启用硬件滚动（软件滚动），接口供后续优化使用�?*/
void app_display_hw_scroll_set(int offset, int height)
{
    int h = height > 0 ? height : 240;
    drv_display_set_scroll_start(offset % h);
}

void app_display_hw_scroll_enable(bool en)
{
    if (en) {
        drv_display_set_scroll_area(0, 240);
    }
}

/* ── 运行时旋转（官方路径：LVGL 逻辑分辨�?+ 硬件 MADCTL 同向切换�?──
 * SPI 面板官方不用软件旋转（lv_display_set_rotation �?adapter 直写 flush 不兼容）�?
 * 旋转 = 硬件 MADCTL（esp_lcd_panel_swap_xy/mirror �?0x36 命令），LVGL 只换分辨率�?
 * 硬件 MADCTL 矩阵（官�?lcd_orientation_helper，绝对标志，基准 0°=竖屏）：
 *   0°  (F,F,F)    90°  (T,T,F)    180° (F,T,T)    270° (T,F,T)
 * 调用上下文：LVGL 线程（定时器/事件回调）或持有 esp_lv_adapter_lock�?*/
void app_display_set_rotation(int deg)
{
    deg = ((deg % 360) + 360) % 360;
    if (deg % 90 != 0) {
        ESP_LOGW(TAG, "invalid rotation %d°, only 0/90/180/270", deg);
        return;
    }
    if (deg == s_orientation_deg) return;

    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;

    bool swap = (deg == 90 || deg == 270);
    /* 1. LVGL 逻辑分辨率（SIZE_CHANGED + 全屏重绘由 LVGL 内部处理） */
    lv_display_set_resolution(disp, swap ? DRV_LCD_V_RES : DRV_LCD_H_RES,
                              swap ? DRV_LCD_H_RES : DRV_LCD_V_RES);
    /* 2. 硬件 MADCTL */
    bool mx = (deg == 90 || deg == 180);
    bool my = (deg == 180 || deg == 270);
    drv_display_set_hw_rotation(swap, mx, my);
    /* 3. 旋转事件路由：栈顶 APP rotate 回调（无则弹栈关闭），桌面直接重排 */
    launcher_event_rotate(deg);

    s_orientation_deg = deg;
    gesture_set_rotation(deg);   /* 输入层同步触摸旋转映射 */
    ESP_LOGI(TAG, "orientation %d° (LVGL %dx%d)", deg,
             (int)lv_display_get_horizontal_resolution(disp),
             (int)lv_display_get_vertical_resolution(disp));
}




/* ── UI Construction ── */

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    /* 桌面启动器（默认界面）：黑夜主题起步，全屏背景由 launcher root 承担 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_launcher = launcher_create(scr);
}




/* ── Public API ── */

void app_display_start(void)
{
    drv_display_t disp = {0};
    drv_display_init(&disp);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_CONFIG(
            disp.panel, disp.io,
            /* 固定注册 320x240（宽=320）：draw buffer 320x40 双缓冲，
             * partial tile �?area 宽动�?reshape，横竖方向均不越界�?
             * 内部 RAM = 320x40x2x2 = 51.2KB（组�?B 横屏已实测可运行�?*/
            ESP_LV_ADAPTER_DISPLAY_PROFILE_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
                DRV_LCD_V_RES, DRV_LCD_H_RES,
                ESP_LV_ADAPTER_ROTATE_0),
            ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
            ESP_LV_ADAPTER_TE_SYNC_DISABLED());
    /* PSRAM 缓冲会让 spi_master �?ISR 中分配内�?DMA priv buffer（每�?flush），
     * 实测分配失败（ESP_ERR_NO_MEM）→ 显示中断。改用内�?RAM 双缓冲�?
     * WIFI/LWIP 缓冲已放 PSRAM（CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP），
     * 40 行双缓冲 = 320×40×2×2 = 51.2KB 内部 RAM */
    disp_cfg.profile.buffer_height = 40;
    disp_cfg.profile.require_double_buffer = true;
    lv_display_t *lv_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (lv_disp == NULL) {   /* 内存不足时回退�?24 �?*/
        disp_cfg.profile.buffer_height = 24;
        lv_disp = esp_lv_adapter_register_display(&disp_cfg);
    }
    assert(lv_disp != NULL);

    /* 初始方向：竖屏（逻辑 240x320 + 硬件 MADCTL 0°，与注册�?320x240 交换�?*/
    lv_display_set_resolution(lv_disp, DRV_LCD_H_RES, DRV_LCD_V_RES);
    drv_display_set_hw_rotation(false, false, false);
    s_orientation_deg = 0;

    esp_lv_adapter_touch_config_t tp_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lv_disp, disp.touch);
    tp_cfg.callbacks.custom_touch_read = gesture_read_cb;   /* 输入层：旋转映射 + 手势检测 */
    lv_indev_t *lv_tp = esp_lv_adapter_register_touch(&tp_cfg);
    assert(lv_tp != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());


    /* flow_view 内存钩子：位�?行历史放 PSRAM（纯 CPU 访问，无 DMA 限制�?*/
    flow_view_malloc = fv_psram_alloc;
    flow_view_free = heap_caps_free;

    /* flow_view 锁钩子：�?LVGL 渲染共享同一把递归�?*/
    static const flow_view_lock_t fv_lock_cb = {
        .lock = fv_acquire,
        .unlock = esp_lv_adapter_unlock,
    };
    flow_view_set_lock(&fv_lock_cb);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        build_ui();
        esp_lv_adapter_unlock();
    }

    /* 终端 APP 常驻任务（消费 UART 数据队列；UI 由 launcher 按需创建） */
    terminal_init();

    ESP_LOGI(TAG, "Serial monitor UI ready");
}


/* SD 挂载后异步加载中文字体（LVGL 线程执行，加载完成后通知当前 APP 刷新） */
static void sd_font_retry_timer_cb(lv_timer_t *t)
{
    (void)t;
    app_font_retry();
    app_font_get(16);
    launcher_event_sd_ready();
}

void app_display_notify_sd_ready(void)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        /* 字体 2.3MB �?SD 读取较慢，移�?LVGL 定时器异步加载，不阻塞本�?*/
        lv_timer_t *t = lv_timer_create(sd_font_retry_timer_cb, 1, NULL);
        lv_timer_set_repeat_count(t, 1);
        lv_timer_set_auto_delete(t, true);
        esp_lv_adapter_unlock();
    }
}
