#ifndef DRV_METER_H
#define DRV_METER_H

/* drv_meter：多路电压表采集驱动（官方 adc_continuous DMA）。
 *
 * 硬件：ESP32-S3 ADC1（GPIO1..10，连续 DMA 仅 ADC1）。atten 12dB 量程
 *   0~3.1V，12bit + eFuse 曲线校准（与 drv_scope 相同）。ADC2 不接受。
 *
 * 采样：固定 aggregate 100kHz（vendored 上限 1M 以内，12bit 精度最佳点），
 *   N 路时分 → 每通道实际采样率 = 100kHz / N。
 * 窗口：固定 METER_WINDOW_S=4s，每通道环形缓冲（PSRAM）容量 =
 *   采样率×4s（多路共享总量 ~800KB，不随路数翻倍）。
 * 测量：滚动 min/max（自 start 起累计，UI 读快照）；波形取 ring 最近窗口
 *   min/max 列（V 轴自适应）。
 *
 * 线程：init/start/stop/get 任意线程可调（内部互斥锁）；采集任务自管理。
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define METER_CH_MAX     8    /* 最大通道数（ADC1 可用脚 2,4,5..10） */
#define METER_RATE_HZ    100000
#define METER_WINDOW_S   4

typedef struct {
    int io;                /* 测量 GPIO（>=0 生效） */
    int rate_hz;           /* 该通道实际采样率（100k/N） */
    bool running;          /* 采集进行中 */
    float v_now;           /* 最新电压（V） */
    float v_min, v_max;    /* 自 start 起累计 min/max（V） */
    uint32_t points;       /* 窗口样本数（=ring 容量，4s×rate） */
} meter_ch_snap_t;

/* 初始化（幂等，惰性）：互斥锁 + adc_continuous 句柄 + 校准。
 * 首次 start 前未 init 会自动 init；APP 退出调 deinit 释放内部 RAM */
esp_err_t drv_meter_init(void);

/* 释放全部资源（停采集 + 卸载 ADC + 校准 + 缓冲） */
esp_err_t drv_meter_deinit(void);

/* 启动采集：io[] 为 METER_CH_MAX 槽位的 GPIO 映射，io[i] < 0 = 该槽未激活
 * （Channel 槽位索引即 io[] 下标，UI 取快照/波形用同一索引）。
 * 激活通道须全为 ADC1（IO1..10）。停旧启新（同 drv_wave_ch_apply 语义） */
esp_err_t drv_meter_start(const int io[METER_CH_MAX]);

/* 停止采集（保留校准句柄） */
esp_err_t drv_meter_stop(void);

/* 取单通道最新快照（锁内计算）。ch=通道索引（与 start 的 io[] 对应） */
esp_err_t drv_meter_get_snap(int ch, meter_ch_snap_t *out);

/* 波形：把该通道 ring 最近窗口 content 压缩到 ncols 列，
 * 输出每列 min/max（V，已校准）与窗口整体 min/max（V）。
 * 未出数据（窗口未满但已有样本）按已有样本绘制 */
esp_err_t drv_meter_get_wave(int ch, int ncols,
                             float *col_min, float *col_max,
                             float *win_min, float *win_max);

#endif /* DRV_METER_H */