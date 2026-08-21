#ifndef DRV_WAVE_H
#define DRV_WAVE_H

/* drv_wave：波形输出驱动（多通道）。
 *
 * 硬件：ESP32-S3 LEDC（PWM，4 定时器 / 8 通道）+ GPIO 直驱，无 DAC。
 *   PWM   —— LEDC 硬件 PWM（5Hz..1MHz，占空比 1..99%）【占定时器】
 *   SQUARE—— LEDC 硬件方波（5Hz..1MHz，占空比 1..99%）【占定时器】
 *   SINE  —— LEDC 载波 + 软件占空比正弦调制（1..200Hz，幅度 0..50）【占定时器】
 *   PULSE —— GPIO 电平脉冲（模拟按钮：默认高，脉冲期拉低，宽度 10..5000ms，
 *            完成后自动停止；UI 用轮询 drv_wave_ch_active 同步状态）【不占定时器】
 *
 * 定时器资源池：PWM/SQUARE/SINE 每通道独占 1 个 LEDC 定时器（S3 共 4 个），
 * 池满时再申请 PWM 类模式返回 ESP_ERR_INVALID_STATE；PULSE 不占定时器。
 *
 * 线程：apply/stop/get 任意线程可调（内部互斥锁）。
 */

#include "esp_err.h"
#include <stdbool.h>

#define WAVE_CH_COUNT 7           /* 最大通道数（= 可用 IO 数） */
#define WAVE_TIMER_COUNT 4        /* LEDC 定时器数（PWM 类模式独占） */

typedef enum {
    WAVE_OFF = 0,   /* 关闭：释放 IO 与定时器 */
    WAVE_PWM,       /* LEDC PWM（占定时器） */
    WAVE_SQUARE,    /* LEDC 方波（占定时器） */
    WAVE_SINE,      /* LEDC 载波 + 软件调制正弦（占定时器） */
    WAVE_PULSE,     /* GPIO 电平脉冲（不占定时器，完成后自动停止） */
} wave_type_t;

const char *drv_wave_type_str(wave_type_t t);

/* 该模式是否占用 LEDC 定时器（PWM/SQUARE/SINE=true，PULSE/OFF=false） */
bool drv_wave_type_uses_timer(wave_type_t t);

typedef struct {
    int io;              /* 输出 GPIO（>=0 生效） */
    wave_type_t type;
    int freq_hz;         /* PWM/SQUARE: 5..1000000；SINE: 1..200 */
    int duty_pct;        /* PWM/SQUARE: 1..99（占空比）；SINE: 0..50（幅度） */
    int pulse_ms;        /* PULSE: 低电平宽度 10..5000ms */
} wave_ch_cfg_t;

/* 初始化（幂等；创建每通道常驻脉冲任务 + 互斥锁） */
esp_err_t drv_wave_init(void);

/* 应用通道配置（停止旧波形，按新配置启动） */
esp_err_t drv_wave_ch_apply(int ch, const wave_ch_cfg_t *cfg);

/* 停止通道输出（等同 apply(WAVE_OFF)；释放 IO 与定时器） */
esp_err_t drv_wave_ch_stop(int ch);

/* 通道是否正在输出非 OFF 波形（PULSE 完成后自动变 false） */
bool drv_wave_ch_active(int ch);

/* 读取当前生效配置 */
void drv_wave_ch_get(int ch, wave_ch_cfg_t *out);

/* 当前占用 LEDC 定时器的通道数（用于 UI 资源判断） */
int drv_wave_timers_in_use(void);

#endif /* DRV_WAVE_H */
