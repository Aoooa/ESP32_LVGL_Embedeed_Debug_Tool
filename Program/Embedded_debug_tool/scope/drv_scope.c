/* drv_scope.c —— 示波器采集驱动：官方 adc_continuous DMA + 软件触发 + 测量。
 * 数据流：adc_continuous_read_parse 拉帧 → 应用层环形缓冲（PSRAM）→
 * 触发检测（边沿+电平）→ 窗口快照（预触发 25%）→ 测量 → 互斥锁帧。 */

#include "drv_scope.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

#define S_TAG "drv_scope"

#define SCOPE_ATTEN        ADC_ATTEN_DB_12   /* 满量程 ~3.1V（0..3.1V 单极性） */
#define SCOPE_FRAME_BYTES  512               /* read_parse 每帧最大样本数 */
#define SCOPE_READ_TIMEOUT 100               /* 拉帧阻塞超时 ms */
#define SCOPE_PRE_TRIG     512               /* 预触发 25%（2048×25%） */
#define SCOPE_POST_TRIG    (SCOPE_FRAME_POINTS - SCOPE_PRE_TRIG)

/* 驱动状态（s_lock 保护；采集任务内 s_ring/s_wr 单写者无需锁） */
static adc_continuous_handle_t s_handle;
static adc_cali_handle_t s_cali;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_run;
static scope_cfg_t s_cfg;

static int s_ch_map[10];           /* ADC1 channel(0-9) -> 应用索引(0/1)，-1=不用 */
static uint16_t *s_ring[2];        /* 环形缓冲（PSRAM） */
static uint32_t s_wr;              /* 写指针（环形） */
static uint32_t s_written;         /* 总写入数（不回绕，判断缓冲是否填满） */
static int s_trig_wr;              /* 触发点环形位置（-1=无） */
static scope_frame_t *s_frame;     /* 最新帧快照（PSRAM，s_lock 保护） */
static adc_continuous_data_t *s_parse_buf;  /* 拉帧解析缓冲（PSRAM，避免 8KB 上任务栈） */

/* 电压校准：raw -> mV */
static int scope_raw_to_mv(uint16_t raw)
{
    int mv = 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    /* 无校准时按 3.1V/4095 近似 */
    return (int)((uint32_t)raw * 3100 / 4095);
}

/* ── 测量（主通道窗口） ── */
static void scope_measure(const uint16_t *w, int n, scope_frame_t *f)
{
    uint32_t vmax = 0, vmin = 4096, i;
    for (i = 0; i < (uint32_t)n; i++) {
        if (w[i] > vmax) vmax = w[i];
        if (w[i] < vmin) vmin = w[i];
    }
    f->vmax = scope_raw_to_mv((uint16_t)vmax) / 1000.0f;
    f->vmin = scope_raw_to_mv((uint16_t)vmin) / 1000.0f;
    f->vpp = f->vmax - f->vmin;

    uint32_t mid = (vmax + vmin) / 2;
    /* 过零法频率：上升沿过中线计数（每周期 1 次）→ f = cross × rate / n */
    uint32_t cross = 0;
    for (i = 1; i < (uint32_t)n; i++) {
        if (w[i - 1] < mid && w[i] >= mid) cross++;
    }
    f->freq_hz = (float)cross * s_cfg.sample_rate_hz / (float)n;

    /* 占空比 + 脉宽（中线以上） */
    uint32_t hi = 0, runs = 0, cur = 0;
    for (i = 0; i < (uint32_t)n; i++) {
        if (w[i] >= mid) { hi++; cur++; }
        else if (cur) { runs++; cur = 0; }
    }
    if (cur) runs++;
    f->duty_pct = 100.0f * hi / n;
    f->pw_ms = runs ? (float)hi / runs * 1000.0f / s_cfg.sample_rate_hz : 0.0f;
}

/* ── 采集任务：拉帧 → 环形缓冲 → 触发 → 窗口快照 ── */
static void scope_task(void *arg)
{
    (void)arg;

    while (s_run) {
        uint32_t n = 0;
        esp_err_t ret = adc_continuous_read_parse(s_handle, s_parse_buf, SCOPE_FRAME_BYTES, &n,
                                                  SCOPE_READ_TIMEOUT);
        if (ret != ESP_OK || n == 0) continue;

        for (uint32_t i = 0; i < n; i++) {
            if (!s_parse_buf[i].valid || s_parse_buf[i].unit != ADC_UNIT_1) continue;
            int idx = (s_parse_buf[i].channel < 10) ? s_ch_map[s_parse_buf[i].channel] : -1;
            if (idx < 0) continue;

            uint16_t v = (uint16_t)s_parse_buf[i].raw_data & 0xFFF;
            s_ring[idx][s_wr] = v;

            /* 主通道（idx==0）触发检测：边沿跨过阈值 */
            if (idx == 0 && s_trig_wr < 0) {
                uint32_t prev = (s_wr + SCOPE_RING_N - 1) % SCOPE_RING_N;
                uint16_t pv = s_ring[0][prev];
                bool hit = (s_cfg.edge == SCOPE_EDGE_RISING)
                               ? (pv < (uint16_t)s_cfg.trigger_level && v >= (uint16_t)s_cfg.trigger_level)
                               : (pv > (uint16_t)s_cfg.trigger_level && v <= (uint16_t)s_cfg.trigger_level);
                if (hit) s_trig_wr = (int)s_wr;
            }
            s_wr = (s_wr + 1) % SCOPE_RING_N;
            s_written++;
        }

        /* 帧更新：触发窗口（优先）或 AUTO 滚动窗口 */
        int start = -1;
        if (s_trig_wr >= 0) {
            /* 触发点后的已采点数（当前写指针之前） */
            uint32_t after = (s_wr - 1 - (uint32_t)s_trig_wr + SCOPE_RING_N) % SCOPE_RING_N;
            if (after >= SCOPE_POST_TRIG) {
                start = (s_trig_wr - SCOPE_PRE_TRIG + SCOPE_RING_N) % SCOPE_RING_N;
            }
        }
        if (start < 0) {
            /* NORM 无触发不更新；AUTO 滚动最新窗口；环未填满 2048 不出帧 */
            if (s_cfg.trig_mode == SCOPE_TRIG_NORM || s_written < SCOPE_FRAME_POINTS) continue;
            start = (int)((s_wr - SCOPE_FRAME_POINTS + SCOPE_RING_N) % SCOPE_RING_N);
        }

        /* 快照窗口 + 测量（写 s_frame，get_frame 锁内 memcpy） */
        int nch = (s_cfg.io[1] >= 0) ? 2 : 1;
        for (int c = 0; c < nch; c++) {
            for (int k = 0; k < SCOPE_FRAME_POINTS; k++) {
                s_frame->ch[c][k] = s_ring[c][(start + k) % SCOPE_RING_N];
            }
        }
        s_frame->points = SCOPE_FRAME_POINTS;
        s_frame->channels = nch;
        s_frame->sample_rate_hz = s_cfg.sample_rate_hz;
        s_frame->running = s_run;
        scope_measure(s_frame->ch[0], SCOPE_FRAME_POINTS, s_frame);
        s_frame->frameno++;

        /* 触发窗口已消费 */
        bool was_trig = (s_trig_wr >= 0);
        s_trig_wr = -1;

        /* SINGLE：触发一次后停止采集（任务自删，UI 显示冻结） */
        if (was_trig && s_cfg.trig_mode == SCOPE_TRIG_SINGLE) {
            s_run = false;
            s_frame->running = false;   /* 冻结帧标记停止 */
        }
    }

    /* 任务退出：持锁清句柄（start/stop 靠它判断任务存活），再自删 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

/* ── Public API ── */

/* 停采集并等任务自删（须持 s_lock 调用）：s_run=false → 任务最迟
 * read_parse 超时（100ms）后退出；轮询等待，异常则强删兜底。 */
static void scope_teardown_locked(void)
{
    s_run = false;
    if (s_task) {
        for (int i = 0; i < 30 && s_task; i++) {
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        if (s_task) {   /* 任务未及时退出（异常）：强删 */
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
    if (s_handle) adc_continuous_stop(s_handle);
}

esp_err_t drv_scope_init(void)
{
    if (s_lock) return ESP_OK;   /* 幂等 */

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    for (int i = 0; i < 10; i++) s_ch_map[i] = -1;

    /* 环形缓冲 + 帧快照（PSRAM） */
    for (int c = 0; c < SCOPE_CH_MAX; c++) {
        s_ring[c] = heap_caps_malloc(SCOPE_RING_N * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring[c]) return ESP_ERR_NO_MEM;
    }
    s_frame = heap_caps_malloc(sizeof(scope_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) return ESP_ERR_NO_MEM;
    memset(s_frame, 0, sizeof(scope_frame_t));

    /* 拉帧解析缓冲（PSRAM；~8KB 不进任务栈） */
    s_parse_buf = heap_caps_malloc(SCOPE_FRAME_BYTES * sizeof(adc_continuous_data_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_parse_buf) return ESP_ERR_NO_MEM;

    /* adc_continuous 句柄（驱动池+DMA 缓冲官方内部 RAM 分配） */
    adc_continuous_handle_cfg_t hdl = {
        .max_store_buf_size = 16 * 1024,
        .conv_frame_size = SCOPE_FRAME_BYTES * SOC_ADC_DIGI_DATA_BYTES_PER_CONV,
    };
    esp_err_t ret = adc_continuous_new_handle(&hdl, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(S_TAG, "new_handle failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 电压校准（S3：curve fitting；v5.5.3 API = create_scheme + check_scheme） */
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = SCOPE_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali, &s_cali);
    if (ret == ESP_OK) {
        adc_cali_scheme_ver_t mask = 0;
        ret = adc_cali_check_scheme(&mask);
        if (ret != ESP_OK) {
            ESP_LOGW(S_TAG, "cali check failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(S_TAG, "cali scheme failed: %s, fallback raw*3100/4095", esp_err_to_name(ret));
        s_cali = NULL;
    }

    ESP_LOGI(S_TAG, "init ok (ring %d pts, frame %d pts)", SCOPE_RING_N, SCOPE_FRAME_POINTS);
    return ESP_OK;
}

esp_err_t drv_scope_start(const scope_cfg_t *cfg)
{
    /* 惰性初始化：启动期不占内部 RAM，首次进 Scope 才 init */
    if (!s_lock) {
        esp_err_t ir = drv_scope_init();
        if (ir != ESP_OK) return ir;
    }
    if (!s_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* 停旧启新（若正在运行） */
    scope_teardown_locked();

    s_cfg = *cfg;

    /* GPIO -> ADC1 channel 映射 */
    int nch = (cfg->io[1] >= 0) ? 2 : 1;
    adc_digi_pattern_config_t pat[SCOPE_CH_MAX];
    for (int i = 0; i < 10; i++) s_ch_map[i] = -1;
    for (int i = 0; i < nch; i++) {
        adc_unit_t unit;
        adc_channel_t chan;
        esp_err_t ret = adc_continuous_io_to_channel(cfg->io[i], &unit, &chan);
        if (ret != ESP_OK || unit != ADC_UNIT_1) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(S_TAG, "io %d not ADC1: %s", cfg->io[i], esp_err_to_name(ret));
            return ESP_ERR_INVALID_ARG;
        }
        s_ch_map[chan] = i;
        pat[i] = (adc_digi_pattern_config_t){
            .atten = SCOPE_ATTEN,
            .channel = chan,
            .unit = ADC_UNIT_1,
            .bit_width = ADC_BITWIDTH_12,
        };
    }

    adc_continuous_config_t cont = {
        .sample_freq_hz = cfg->sample_rate_hz,
        .pattern_num = nch,
        .adc_pattern = pat,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    esp_err_t ret = adc_continuous_config(s_handle, &cont);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(S_TAG, "config failed (%d Hz x%d ch): %s", cfg->sample_rate_hz, nch,
                 esp_err_to_name(ret));
        return ret;
    }
    ret = adc_continuous_start(s_handle);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(S_TAG, "start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_wr = 0;
    s_written = 0;
    s_trig_wr = -1;
    s_frame->frameno = 0;
    s_frame->running = true;
    s_run = true;

    xTaskCreateWithCaps(scope_task, "scope", 4096, NULL, 6, &s_task, MALLOC_CAP_SPIRAM);
    ESP_LOGI(S_TAG, "start: %d Hz, %d ch (%s), trig=%s/%d mode=%d", cfg->sample_rate_hz,
             nch, cfg->io[1] >= 0 ? "dual" : "single",
             cfg->edge == SCOPE_EDGE_RISING ? "rise" : "fall",
             cfg->trigger_level, cfg->trig_mode);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t drv_scope_stop(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scope_teardown_locked();
    if (s_frame) s_frame->running = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(S_TAG, "stopped");
    return ESP_OK;
}

/* 释放全部资源（Scope APP 退出时调用）：停采集 → 删驱动/校准 → 释放缓冲 */
esp_err_t drv_scope_deinit(void)
{
    if (!s_lock) return ESP_OK;   /* 未初始化过，无需释放 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scope_teardown_locked();
    if (s_cali) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_handle) {
        adc_continuous_deinit(s_handle);
        s_handle = NULL;
    }
    for (int c = 0; c < SCOPE_CH_MAX; c++) {
        if (s_ring[c]) heap_caps_free(s_ring[c]);
        s_ring[c] = NULL;
    }
    if (s_parse_buf) heap_caps_free(s_parse_buf);
    s_parse_buf = NULL;
    if (s_frame) heap_caps_free(s_frame);
    s_frame = NULL;
    xSemaphoreGive(s_lock);
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    ESP_LOGI(S_TAG, "deinit: resources released");
    return ESP_OK;
}

esp_err_t drv_scope_get_frame(scope_frame_t *out)
{
    if (!s_lock || !s_frame) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = *s_frame;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
