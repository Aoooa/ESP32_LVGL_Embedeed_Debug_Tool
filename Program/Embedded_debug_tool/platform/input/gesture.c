/* gesture.c —— 输入层：触摸坐标旋转映射 + 左缘右滑返回手势检测（LVGL 线程） */

#include "gesture.h"
#include "drv_display.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "gesture";

/* ── 左缘右滑返回手势参数 ── */
#define SWIPE_EDGE_X      15     /* 起点距逻辑左边缘 ≤15px 才触发（严格贴边） */
#define SWIPE_MIN_DX      30     /* 累计水平右移 ≥30px 即触发返回（移动中触发，不等释放） */
#define SWIPE_CANDIDATE_DX 25    /* 累计右移 >25px 即判定为返回手势候选 → 官方 wait_release 禁单击 */

/* ── 状态（read_cb 同一线程访问，无需锁） ── */
static int s_orientation_deg;                 /* 当前逻辑方向 0/90/180/270 */
static gesture_back_cb_t s_back_cb;           /* 返回事件回调（launcher 注册） */
static void *s_back_ctx;
static bool s_swipe_tracking;                 /* 本次是否处于按下 */
static lv_coord_t s_swipe_start_x, s_swipe_start_y;
static lv_coord_t s_swipe_last_x, s_swipe_last_y;
static bool s_swipe_candidate;                /* 已判定为返回手势候选（禁单击） */
static bool s_swipe_triggered;                /* 已触发返回 */

/* 返回事件（read_cb 上下文直调，LVGL 线程） */
static void fire_back_event(void)
{
    if (s_back_cb) {
        ESP_LOGI(TAG, "[EVT] swipe-back fired (start=%d,%d last=%d,%d)",
                 s_swipe_start_x, s_swipe_start_y, s_swipe_last_x, s_swipe_last_y);
        s_back_cb(s_back_ctx);
    } else {
        ESP_LOGW(TAG, "[EVT] swipe-back fired but no handler registered");
    }
}

/*
 * 触摸坐标旋转：CST816S 原始坐标 → LVGL 逻辑坐标。
 * 4 方向映射（与官方 touch_rotation_helper 矩阵一致，面板 240x320 物理）：
 *   0°  (240x320 逻辑，硬件竖屏)：直连（已验证）
 *   90° (320x240 逻辑，硬件 swap+mirror_x)：x=(320-1)-raw_y, y=raw_x（已验证）
 *   180°：x=(240-1)-raw_x, y=(320-1)-raw_y
 *   270°：x=raw_y, y=(240-1)-raw_x
 */
esp_err_t gesture_read_cb(esp_lcd_touch_handle_t tp,
                          esp_lcd_touch_point_data_t *points,
                          uint8_t *count, uint8_t max_count,
                          void *user_ctx)
{
    (void)user_ctx;
    (void)max_count;

    uint16_t raw_x = 0, raw_y = 0;
    uint8_t cnt = 0;

    esp_lcd_touch_read_data(tp);
    esp_lcd_touch_get_coordinates(tp, &raw_x, &raw_y, NULL, &cnt, 1);

    if (cnt > 0) {
        *count = 1;
        switch (s_orientation_deg) {
        case 90:
            /* 实测基准：第一版 (319-raw_y, raw_x) 显示正确但触摸差 180°，
             * 逻辑坐标翻转修正：x=raw_y, y=(240-1)-raw_x */
            points[0].x = raw_y;
            points[0].y = (DRV_LCD_H_RES - 1) - raw_x;
            break;
        case 180:
            points[0].x = (DRV_LCD_H_RES - 1) - raw_x;
            points[0].y = (DRV_LCD_V_RES - 1) - raw_y;
            break;
        case 270:
            points[0].x = (DRV_LCD_V_RES - 1) - raw_y;
            points[0].y = raw_x;
            break;
        default:   /* 0°：竖屏直连 */
            points[0].x = raw_x;
            points[0].y = raw_y;
            break;
        }
        points[0].strength = 1;
    } else {
        *count = 0;
    }

    /* 左缘右滑返回手势：移动中累计判定（不等释放），识别候选后抑制单击。
     * 全程用本帧真实逻辑坐标（points 尚未被改写）跟踪/判定 */
    if (cnt > 0) {
        lv_coord_t lx = points[0].x;
        lv_coord_t ly = points[0].y;
        if (!s_swipe_tracking) {
            s_swipe_tracking = true;
            s_swipe_candidate = false;
            s_swipe_triggered = false;
            s_swipe_start_x = lx;
            s_swipe_start_y = ly;
        }
        s_swipe_last_x = lx;
        s_swipe_last_y = ly;

        if (!s_swipe_triggered) {
            int dx = lx - s_swipe_start_x;
            if (s_swipe_start_x <= SWIPE_EDGE_X) {
                /* 候选：dx>25 即禁单击 → 官方 lv_indev_wait_release：
                 * 释放时 LVGL 走 PRESS_LOST 分支，act_obj=NULL，CLICKED 不派发 */
                if (!s_swipe_candidate && dx > SWIPE_CANDIDATE_DX) {
                    s_swipe_candidate = true;
                    lv_indev_wait_release(lv_indev_active());
                    ESP_LOGI(TAG, "[SWIPE] CANDIDATE dx=%d -> wait_release (suppress click)", dx);
                }
                /* 触发：dx≥30 → 立即返回（不等释放，Y 轴偏差不限）。
                 * 触发后上报 RELEASED，让 LVGL 先释放按住再删 APP，
                 * 避免按住中删对象导致整屏重绘闪烁 */
                if (dx >= SWIPE_MIN_DX) {
                    s_swipe_triggered = true;
                    ESP_LOGI(TAG, "[SWIPE] TRIGGER start_x=%d dx=%d -> back event", s_swipe_start_x, dx);
                    fire_back_event();
                }
            }
        }
    } else if (s_swipe_tracking) {
        s_swipe_tracking = false;
    }

    /* 已触发返回 → 本帧及后续帧上报 RELEASED（保证手指抬起前 LVGL 保持 RELEASED，
     * 避免 APP 销毁后残留 PRESSED 造成二次点击） */
    if (cnt > 0 && s_swipe_triggered) {
        *count = 0;
    }

    return ESP_OK;
}

void gesture_set_rotation(int deg)
{
    s_orientation_deg = deg;
    ESP_LOGI(TAG, "orientation set %d°", deg);
}

void gesture_set_back_handler(gesture_back_cb_t cb, void *ctx)
{
    s_back_cb = cb;
    s_back_ctx = ctx;
    ESP_LOGI(TAG, "back handler %s", cb ? "registered" : "cleared");
}

bool gesture_is_pressed(void)
{
    return s_swipe_tracking;
}
