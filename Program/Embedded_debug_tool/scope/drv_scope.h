#ifndef DRV_SCOPE_H
#define DRV_SCOPE_H

/* drv_scope：示波器采集驱动（官方 adc_continuous DMA）。
 *
 * 硬件：ESP32-S3 ADC1（GPIO1-10）。连续模式官方硬限 611..83333 SPS
 *   （SOC_ADC_SAMPLE_FREQ_THRES_HIGH=83333，soc_caps.h）；
 *   ADC2 连续 DMA 因 errata 禁用（soc_caps.h SOC_ADC_DIG_SUPPORTED_UNIT），
 *   双通道 = ADC1 两路 MUX 分时，每通道采样率 = sample_rate / 2。
 *   输入 0~3.1V 单极性（atten 11dB，无前端调理）；ENOB ~9-10bit。
 *
 * 数据流：采集任务 adc_continuous_read_parse() 拉帧（官方读+解析一步，
 *   返回 adc_continuous_data_t{unit,channel,raw_data,valid}）→ 应用层
 *   环形缓冲（PSRAM，SCOPE_RING_N/通道）→ 软件触发（边沿+电平+预触发
 *   25%）→ 测量（过零频率/Vpp/占空比/脉宽）→ 互斥锁快照（scope_frame_t）。
 *   UI（LVGL 线程）drv_scope_get_frame() 锁内 memcpy 取最新帧。
 *
 * 线程：init/start/stop/get_frame 任意线程可调（内部互斥锁）；
 *   采集任务内部自管理，不碰 LVGL。
 *
 * 高速采样备选（未启用）：官方硬限 83.3kSPS 满足不了串口/数字信号
 *   （115200 波特率翻转 ~57.6kHz）。如需可改 vendored 驱动突破：
 *   soc_caps.h THRES_HIGH 提高 + adc_hal.c digi 时钟（5MHz）提高 +
 *   sar_clk_div 配合，可达 1M~2MSPS；代价 ENOB 降至 ~7-8bit（官方不
 *   保证精度），可配合过采样平均（4x 过采样 ≈ +1bit）补偿。当前按官方
 *   驱动交付，此备选留作"数字信号分析档"后续实现。
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define SCOPE_CH_MAX        2
#define SCOPE_FRAME_POINTS  2048    /* 显示窗口点数（预触发 25% + 触发后 75%） */
#define SCOPE_RING_N        8192    /* 应用层环形缓冲（每通道） */

/* 触发模式 */
typedef enum {
    SCOPE_TRIG_AUTO = 0,   /* 有触发用触发窗口，无触发滚动最新（保证显示动） */
    SCOPE_TRIG_NORM,       /* 必须触发才更新快照 */
    SCOPE_TRIG_SINGLE,     /* 触发一次后自动停止（捕捉瞬态） */
} scope_trig_mode_t;

/* 触发边沿 */
typedef enum {
    SCOPE_EDGE_RISING = 0,
    SCOPE_EDGE_FALLING,
} scope_edge_t;

typedef struct {
    int sample_rate_hz;        /* 官方合法 611..83333 */
    int io[2];                 /* ADC1 输入 GPIO（io[1]=-1 单通道） */
    scope_trig_mode_t trig_mode;
    scope_edge_t edge;
    int trigger_level;         /* 0..4095（主通道 = io[0]） */
} scope_cfg_t;

typedef struct {
    uint32_t frameno;          /* 帧号（UI 判重绘） */
    int sample_rate_hz;        /* 实际采样率 */
    int channels;              /* 1 或 2 */
    int points;                /* 每通道点数（=SCOPE_FRAME_POINTS） */
    bool running;              /* 采集进行中（SINGLE 触发后 false） */
    uint16_t ch[2][SCOPE_FRAME_POINTS];  /* 窗口数据（0..4095 raw） */
    /* 主通道测量（ch[0]） */
    float freq_hz;             /* 频率（过零法） */
    float vpp, vmax, vmin;     /* 电压（esp_adc_cal 换算，V） */
    float duty_pct;            /* 占空比 % */
    float pw_ms;               /* 脉宽 ms */
} scope_frame_t;

/* 初始化（幂等）：创建互斥锁 + adc_continuous handle + esp_adc_cal 校准句柄。
 * 惰性调用：首次 drv_scope_start 前未 init 会自动 init（避开启动期内部 RAM
 * 竞争）；Scope APP 退出时调 drv_scope_deinit 释放全部内部 RAM 资源 */
esp_err_t drv_scope_init(void);

/* 释放全部资源（停采集 + adc_continuous_deinit + 校准 + 缓冲），
 * Scope APP 退出时调用，给其他 APP 腾内部 RAM；下次 start 自动重新 init */
esp_err_t drv_scope_deinit(void);

/* 启动采集（停旧启新，同 drv_wave_ch_apply 语义）：
 * 按 cfg 配置 adc_continuous（pattern/频率）→ start → 创建采集任务 */
esp_err_t drv_scope_start(const scope_cfg_t *cfg);

/* 停止采集：置停止标志 + 删任务 + adc_continuous_stop（保留校准句柄） */
esp_err_t drv_scope_stop(void);

/* 取最新帧快照（锁内 memcpy；未出帧 frameno=0） */
esp_err_t drv_scope_get_frame(scope_frame_t *out);

#endif /* DRV_SCOPE_H */
