/* gesture.c —— 输入层：触摸坐标旋转映射 + 防抖 + 左缘右滑返回手势检测（LVGL 线程） */

#include "gesture.h"
#include "drv_display.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "gesture";

/* ── 手势参数 ── */
#define SWIPE_EDGE_X      40     /* 起点距逻辑左边缘 ≤40px → 贴边右滑（返回） */
#define SWIPE_MIN_DX      20     /* 贴边右滑：累计右移 ≥20px 即触发返回（移动中触发，不等释放） */
#define SWIPE_CANDIDATE_DX 10    /* 贴边候选：右移 >10px 即判定候选 → 官方 wait_release 禁单击 */
#define SWIPE_GLOBAL_DX   50     /* 全局手势（任意起点，仅识别不触发功能）：|dx| ≥50
                                  *   右滑 → right 事件，左滑 → left 事件（接口预留，UI 后续接） */

/* 边缘垂直手势：
 *   上边缘下滑（起点顶部 SWIPE_EDGE_TOP，向下滑 ≥MIN_DY）→ topdrop
 *   下边缘上滑（起点底部，向上滑 ≥MIN_DY）→ bottomup
 * 与水平手势（返回/左右滑）方向正交，同一手势只触发一种；触发后并入
 * s_swipe_triggered（与非单击抑制共用），避免一次手势触发多个事件。 */
#define SWIPE_EDGE_TOP    40
#define SWIPE_EDGE_BOT    40
#define SWIPE_MIN_DY      20

/* ── 触摸防抖（时间戳锁存） ──
 * 物理层按下/松开各需持续 TOUCH_DEBOUNCE_MS 才确认上报给 LVGL：
 *   · 按下 <50ms 即消失 → 抖动，直接丢弃（不上报按下）
 *   · 松开 <50ms 又按下 → 保持按下（平滑接触不良的"松开→按下"波动）
 * 时间戳方案不依赖 LVGL 轮询频率，精确可控；对 LVGL 完全透明。 */
#define TOUCH_DEBOUNCE_MS  50    /* 30-60ms 区间取 50 */

/* ── 状态（read_cb 同一线程访问，无需锁） ── */
static int s_orientation_deg;                 /* 当前逻辑方向 0/90/180/270 */
static gesture_back_cb_t s_back_cb;           /* 返回事件回调（launcher 注册） */
static void *s_back_ctx;
static gesture_right_cb_t s_right_cb;         /* 全局右滑事件回调（接口预留，未注册） */
static void *s_right_ctx;
static gesture_left_cb_t s_left_cb;           /* 全局左滑事件回调（接口预留，未注册） */
static void *s_left_ctx;
static gesture_drag_cb_t s_drag_cb;           /* 拖动返回回调（手机式滑动返回） */
static void *s_drag_ctx;
static gesture_topdrop_cb_t s_topdrop_cb;     /* 上边缘下滑回调 */
static void *s_topdrop_ctx;
static gesture_bottomup_cb_t s_bottomup_cb;   /* 下边缘上滑回调 */
static void *s_bottomup_ctx;
static bool s_global_swipe_en = true;         /* 全局右/左滑开关（贴边返回不受影响） */
static bool s_swipe_tracking;                 /* 本次是否处于按下 */
static lv_coord_t s_swipe_start_x, s_swipe_start_y;
static lv_coord_t s_swipe_last_x, s_swipe_last_y;
static bool s_swipe_candidate;                /* 已判定为返回手势候选（禁单击） */
static bool s_swipe_triggered;                /* 已触发返回 */
static bool s_drag_active;                    /* 正在拖动返回（跟随手指） */

/* 防抖状态 */
static bool s_touch_started;                  /* 物理层检测到按下（未确认） */
static bool s_touch_confirmed;                /* 已确认按下（已上报 LVGL） */
static uint64_t s_touch_start_us;             /* 检测到按下的时间戳 */
static bool s_release_started;                /* 松开确认计时中 */
static uint64_t s_release_start_us;
static lv_coord_t s_touch_x, s_touch_y;       /* 有效坐标（确认后最新帧，物理坐标） */

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

/* 全局右滑事件（仅识别不触发功能；read_cb 上下文直调，LVGL 线程） */
static void fire_right_event(void)
{
    if (s_right_cb) {
        ESP_LOGI(TAG, "[EVT] swipe-right fired (start=%d,%d last=%d,%d)",
                 s_swipe_start_x, s_swipe_start_y, s_swipe_last_x, s_swipe_last_y);
        s_right_cb(s_right_ctx);
    } else {
        ESP_LOGW(TAG, "[EVT] swipe-right fired but no handler registered");
    }
}

/* 全局左滑事件（仅识别不触发功能；read_cb 上下文直调，LVGL 线程） */
static void fire_left_event(void)
{
    if (s_left_cb) {
        ESP_LOGI(TAG, "[EVT] swipe-left fired (start=%d,%d last=%d,%d)",
                 s_swipe_start_x, s_swipe_start_y, s_swipe_last_x, s_swipe_last_y);
        s_left_cb(s_left_ctx);
    } else {
        ESP_LOGW(TAG, "[EVT] swipe-left fired but no handler registered");
    }
}

/* 上边缘下滑（read_cb 上下文直调，LVGL 线程） */
static void fire_topdrop_event(void)
{
    if (s_topdrop_cb) {
        ESP_LOGI(TAG, "[EVT] top-drop fired (start=%d,%d dy=%d)",
                 s_swipe_start_x, s_swipe_start_y, s_swipe_last_y - s_swipe_start_y);
        s_topdrop_cb(s_topdrop_ctx);
    } else {
        ESP_LOGW(TAG, "[EVT] top-drop fired but no handler registered");
    }
}

/* 下边缘上滑 */
static void fire_bottomup_event(void)
{
    if (s_bottomup_cb) {
        ESP_LOGI(TAG, "[EVT] bottom-up fired (start=%d,%d dy=%d)",
                 s_swipe_start_x, s_swipe_start_y, s_swipe_last_y - s_swipe_start_y);
        s_bottomup_cb(s_bottomup_ctx);
    } else {
        ESP_LOGW(TAG, "[EVT] bottom-up fired but no handler registered");
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

    /* ── 触摸防抖：时间戳锁存 ── */
    bool raw_pressed = (cnt > 0);
    uint64_t now = (uint64_t)esp_timer_get_time();
    bool out_pressed = false;

    if (raw_pressed) {
        if (!s_touch_started) {
            s_touch_started = true;
            s_touch_start_us = now;
            s_touch_confirmed = false;
            s_release_started = false;
        }
        if (!s_touch_confirmed) {
            if ((uint64_t)(now - s_touch_start_us) >= TOUCH_DEBOUNCE_MS * 1000ULL) {
                s_touch_confirmed = true;   /* 按下持续达标 → 确认上报 */
            }
        }
        if (s_touch_confirmed) {
            out_pressed = true;
            s_touch_x = raw_x;              /* 有效坐标 = 最新帧（确认期间可能已移动） */
            s_touch_y = raw_y;
        }
    } else {
        if (s_touch_started) {
            if (s_touch_confirmed) {
                if (!s_release_started) {
                    s_release_started = true;
                    s_release_start_us = now;
                }
                if ((uint64_t)(now - s_release_start_us) >= TOUCH_DEBOUNCE_MS * 1000ULL) {
                    /* 松开持续达标 → 确认松开 */
                    s_touch_started = false;
                    s_touch_confirmed = false;
                    s_release_started = false;
                } else {
                    out_pressed = true;     /* 松开未确认：保持按下（平滑波动） */
                }
            } else {
                /* 按下未确认即消失 → 抖动，整段丢弃 */
                s_touch_started = false;
            }
        }
    }

    /* ── 上报 LVGL（防抖后状态 + 有效坐标旋转映射） ── */
    if (out_pressed) {
        *count = 1;
        switch (s_orientation_deg) {
        case 90:
            /* 实测基准：第一版 (319-raw_y, raw_x) 显示正确但触摸差 180°，
             * 逻辑坐标翻转修正：x=raw_y, y=(240-1)-raw_x */
            points[0].x = s_touch_y;
            points[0].y = (DRV_LCD_H_RES - 1) - s_touch_x;
            break;
        case 180:
            points[0].x = (DRV_LCD_H_RES - 1) - s_touch_x;
            points[0].y = (DRV_LCD_V_RES - 1) - s_touch_y;
            break;
        case 270:
            points[0].x = (DRV_LCD_V_RES - 1) - s_touch_y;
            points[0].y = s_touch_x;
            break;
        default:   /* 0°：竖屏直连 */
            points[0].x = s_touch_x;
            points[0].y = s_touch_y;
            break;
        }
        points[0].strength = 1;
    } else {
        *count = 0;
    }

    /* 手势检测：贴边右滑返回 + 全局右滑/左滑事件（仅识别不触发功能）。
     * 移动中累计判定（不等释放），识别候选后抑制单击。
     * 全程用本帧真实逻辑坐标（points 尚未被改写）跟踪/判定 */
    if (out_pressed) {
        lv_coord_t lx = points[0].x;
        lv_coord_t ly = points[0].y;
        if (!s_swipe_tracking) {
            s_swipe_tracking = true;
            s_swipe_candidate = false;
            s_swipe_triggered = false;
            s_drag_active = false;
            s_swipe_start_x = lx;
            s_swipe_start_y = ly;
        }
        s_swipe_last_x = lx;
        s_swipe_last_y = ly;

        if (!s_swipe_triggered) {
            int dx = lx - s_swipe_start_x;

            if (s_swipe_start_x <= SWIPE_EDGE_X) {
                /* 贴边右滑：dx≥20 →
                 *   有拖动回调 → 进入拖动模式（持续上报，界面跟随手指）
                 *   无拖动回调 → 立即返回（原逻辑）
                 * 触发点（dx≥20）才 wait_release 禁单击：轻触/微移（10~20px）
                 * 保留 CLICKED，否则顶栏小按钮（收藏⭐等，起点在左缘带内）
                 * 手指轻微漂移就被吞掉点击 */
                if (dx >= SWIPE_MIN_DX) {
                    if (!s_swipe_candidate) {
                        s_swipe_candidate = true;
                        lv_indev_wait_release(lv_indev_active());
                        ESP_LOGI(TAG, "[SWIPE] edge TRIGGER dx=%d -> wait_release (suppress click)", dx);
                    }
                    if (s_drag_cb) {
                        s_swipe_triggered = true;
                        s_drag_active = true;
                        s_drag_cb(s_drag_ctx, dx, true);   /* 首帧即上报 */
                        ESP_LOGI(TAG, "[SWIPE] edge drag START dx=%d", dx);
                    } else {
                        s_swipe_triggered = true;
                        ESP_LOGI(TAG, "[SWIPE] edge TRIGGER start_x=%d dx=%d -> back event",
                                 s_swipe_start_x, dx);
                        fire_back_event();
                    }
                }
            } else if (s_global_swipe_en && dx >= SWIPE_GLOBAL_DX) {
                /* 全局右滑（任意起点）：dx≥50 → 仅识别，发 right 事件（不触发返回） */
                if (!s_swipe_candidate) {
                    s_swipe_candidate = true;
                    lv_indev_wait_release(lv_indev_active());
                    ESP_LOGI(TAG, "[SWIPE] global-right CANDIDATE start_x=%d dx=%d -> wait_release",
                             s_swipe_start_x, dx);
                }
                s_swipe_triggered = true;
                ESP_LOGI(TAG, "[SWIPE] global-right TRIGGER start_x=%d dx=%d -> right event",
                         s_swipe_start_x, dx);
                fire_right_event();
            } else if (s_global_swipe_en && dx <= -SWIPE_GLOBAL_DX) {
                /* 全局左滑（任意起点）：dx≤-50 → 仅识别，发 left 事件 */
                if (!s_swipe_candidate) {
                    s_swipe_candidate = true;
                    lv_indev_wait_release(lv_indev_active());
                    ESP_LOGI(TAG, "[SWIPE] global-left CANDIDATE start_x=%d dx=%d -> wait_release",
                             s_swipe_start_x, dx);
                }
                s_swipe_triggered = true;
                ESP_LOGI(TAG, "[SWIPE] global-left TRIGGER start_x=%d dx=%d -> left event",
                         s_swipe_start_x, dx);
                fire_left_event();
            } else if (s_global_swipe_en) {
                /* 垂直边缘手势（上缘下滑 / 下缘上滑；起点落在对应边缘区且垂直位移够才判定） */
                int dy = ly - s_swipe_start_y;
                int sh = lv_display_get_vertical_resolution(lv_display_get_default());
                if ((s_swipe_start_y <= SWIPE_EDGE_TOP && dy >= SWIPE_MIN_DY) ||
                    (s_swipe_start_y >= sh - SWIPE_EDGE_BOT && dy <= -SWIPE_MIN_DY)) {
                    if (!s_swipe_candidate) {
                        s_swipe_candidate = true;
                        lv_indev_wait_release(lv_indev_active());
                        ESP_LOGI(TAG, "[SWIPE] edge-vert CANDIDATE start=%d,%d dy=%d -> wait_release",
                                 s_swipe_start_x, s_swipe_start_y, dy);
                    }
                    if (s_swipe_start_y <= SWIPE_EDGE_TOP && dy >= SWIPE_MIN_DY) {
                        s_swipe_triggered = true;
                        fire_topdrop_event();
                    } else if (s_swipe_start_y >= sh - SWIPE_EDGE_BOT && dy <= -SWIPE_MIN_DY) {
                        s_swipe_triggered = true;
                        fire_bottomup_event();
                    }
                }
            }
        } else if (s_drag_active && s_drag_cb) {
            /* 拖动中：持续上报位移（界面跟随手指） */
            int dx = lx - s_swipe_start_x;
            s_drag_cb(s_drag_ctx, dx, true);
        }
    } else if (s_swipe_tracking) {
        s_swipe_tracking = false;
        if (s_drag_active && s_drag_cb) {
            /* 松手：上报最终位移（调用方判定滑出/回弹） */
            int dx = s_swipe_last_x - s_swipe_start_x;
            s_drag_active = false;
            s_drag_cb(s_drag_ctx, dx, false);
            ESP_LOGI(TAG, "[SWIPE] edge drag RELEASE dx=%d", dx);
        }
    }

    /* 已触发返回 → 本帧及后续帧上报 RELEASED（保证手指抬起前 LVGL 保持 RELEASED，
     * 避免 APP 销毁后残留 PRESSED 造成二次点击） */
    if (out_pressed && s_swipe_triggered) {
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

void gesture_set_drag_handler(gesture_drag_cb_t cb, void *ctx)
{
    s_drag_cb = cb;
    s_drag_ctx = ctx;
    ESP_LOGI(TAG, "drag handler %s", cb ? "registered" : "cleared");
}

void gesture_set_right_handler(gesture_right_cb_t cb, void *ctx)
{
    s_right_cb = cb;
    s_right_ctx = ctx;
    ESP_LOGI(TAG, "right handler %s", cb ? "registered" : "cleared");
}

void gesture_set_left_handler(gesture_left_cb_t cb, void *ctx)
{
    s_left_cb = cb;
    s_left_ctx = ctx;
    ESP_LOGI(TAG, "left handler %s", cb ? "registered" : "cleared");
}

void gesture_set_global_swipe(bool en)
{
    s_global_swipe_en = en;
    ESP_LOGI(TAG, "global swipe %s", en ? "enabled" : "disabled");
}

void gesture_set_topdrop_handler(gesture_topdrop_cb_t cb, void *ctx)
{
    s_topdrop_cb = cb;
    s_topdrop_ctx = ctx;
    ESP_LOGI(TAG, "top-drop handler %s", cb ? "registered" : "cleared");
}

void gesture_set_bottomup_handler(gesture_bottomup_cb_t cb, void *ctx)
{
    s_bottomup_cb = cb;
    s_bottomup_ctx = ctx;
    ESP_LOGI(TAG, "bottom-up handler %s", cb ? "registered" : "cleared");
}

bool gesture_is_pressed(void)
{
    return s_swipe_tracking;
}

void gesture_consume_current(void)
{
    /* 终止本次手势：取消拖动，标记已触发（此后至松手不再触发任何返回/拖动）。
     * 供调用方实现"一步一动作"——如阅读器状态栏显示时右滑只隐藏栏。 */
    if (s_drag_active && s_drag_cb) {
        int dx = s_swipe_last_x - s_swipe_start_x;
        s_drag_active = false;
        s_drag_cb(s_drag_ctx, dx, false);   /* 上报松手状，让 launcher 复位资源 */
    }
    s_swipe_triggered = true;
    ESP_LOGI(TAG, "[SWIPE] gesture consumed (current)");
}
