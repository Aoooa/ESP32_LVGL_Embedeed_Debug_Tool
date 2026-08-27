/* drv_meter.c —— 多路电压表采集驱动：官方 adc_continuous DMA，固定 100kHz。
 * 数据流：采集任务拉帧 → 每通道环形缓冲（PSRAM，4s 窗口）→
 * 滚动 min/max（自 start 起）→ UI 经互斥锁取快照 / 波形列。 */

#include "drv_meter.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

#define M_TAG "drv_meter"

#define METER_ATTEN        ADC_ATTEN_DB_12

/* conv_frame_size：1024（256 样本/帧）。原 2048 时 rx_dma_buf=5×2048=10KB
 * 连续内部 DMA RAM，与 Scope 同类可致 new_handle ESP_ERR_NO_MEM；1024 → 5KB */
#define METER_FRAME_BYTES  1024
#define METER_RAW_BUF_BYTES 1024
#define METER_READ_TIMEOUT 100

/* 驱动状态 */
static adc_continuous_handle_t s_handle;
static adc_cali_handle_t s_cali;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_run;
static int s_n;                 /* 激活通道数 */
static int s_ch_map[10];        /* ADC1 channel(0-9) -> 通道索引，-1=不用 */

typedef struct {
    int io;
    int rate_hz;          /* 实际采样率 = 100k/N */
    uint32_t ring_n;      /* ring 容量 = rate×4s */
    uint16_t *ring;       /* PSRAM */
    uint32_t wr;          /* 写指针 */
    uint32_t written;     /* 总写入数（不回绕） */
    uint16_t min_raw;     /* 自 start 起 min（raw） */
    uint16_t max_raw;
    uint16_t last_raw;
} meter_ch_t;

static meter_ch_t s_ch[METER_CH_MAX];
static uint32_t *s_raw_buf;   /* read 4B DMA 缓冲（PSRAM） */

/* raw -> mV（校准） */
static int meter_raw_to_mv(uint16_t raw)
{
    int mv = 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    return (int)((uint32_t)raw * 3100 / 4095);
}

static void meter_raw_free_all(void)
{
    for (int i = 0; i < METER_CH_MAX; i++) {
        if (s_ch[i].ring) {
            heap_caps_free(s_ch[i].ring);
            s_ch[i].ring = NULL;
        }
        s_ch[i].wr = 0;
        s_ch[i].written = 0;
    }
}

/* ── 采集任务：拉帧 → 环形缓冲 + 滚动 min/max ── */

static void meter_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();

    while (s_run) {
        esp_task_wdt_reset();

        uint32_t out_len = 0;
        esp_err_t ret = adc_continuous_read(s_handle, (uint8_t *)s_raw_buf,
                                            METER_RAW_BUF_BYTES, &out_len,
                                            METER_READ_TIMEOUT);
        if (ret != ESP_OK || out_len == 0) continue;
        uint32_t n = out_len / 4;

        const uint32_t *p = (const uint32_t *)s_raw_buf;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t raw = p[i];
            if ((raw >> 17) & 1) continue;            /* unit=1 → ADC2 跳过 */
            int chn = (int)((raw >> 13) & 0xF);
            int idx = (chn < 10) ? s_ch_map[chn] : -1;
            if (idx < 0) continue;

            uint16_t v = (uint16_t)(raw & 0xFFF);
            meter_ch_t *c = &s_ch[idx];
            c->ring[c->wr] = v;
            c->wr = (c->wr + 1) % c->ring_n;
            c->written++;
            if (v < c->min_raw) c->min_raw = v;
            if (v > c->max_raw) c->max_raw = v;
            c->last_raw = v;
        }
        taskYIELD();   /* 防止饿死 IDLE0（TWDT 5s 超时的根因，同 drv_scope） */
    }

    esp_task_wdt_delete(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

/* ── 停采集（须持锁） ── */

static void meter_teardown_locked(void)
{
    s_run = false;
    if (s_task) {
        for (int i = 0; i < 30 && s_task; i++) {
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        if (s_task) {
            esp_task_wdt_delete(s_task);
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
    if (s_handle) adc_continuous_stop(s_handle);
}

/* ── Public API ── */

esp_err_t drv_meter_init(void)
{
    if (s_lock) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(M_TAG, "init: mutex create failed");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 10; i++) s_ch_map[i] = -1;
    memset(s_ch, 0, sizeof(s_ch));

    s_raw_buf = heap_caps_malloc(METER_RAW_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_raw_buf) {
        ESP_LOGE(M_TAG, "init: raw buf alloc failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    adc_continuous_handle_cfg_t hdl = {
        .max_store_buf_size = 32 * 1024,
        .conv_frame_size = METER_FRAME_BYTES,
    };
    esp_err_t ret = adc_continuous_new_handle(&hdl, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(M_TAG, "init: new_handle FAILED: %s", esp_err_to_name(ret));
        heap_caps_free(s_raw_buf);
        s_raw_buf = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ret;
    }

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = METER_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali, &s_cali);
    if (ret != ESP_OK) {
        ESP_LOGW(M_TAG, "init: cali scheme failed: %s, fallback raw*3100/4095", esp_err_to_name(ret));
        s_cali = NULL;
    }

    ESP_LOGI(M_TAG, "init ok (100kHz, 4s window/ch)");
    return ESP_OK;
}

esp_err_t drv_meter_start(const int io[METER_CH_MAX])
{
    if (!io) return ESP_ERR_INVALID_ARG;

    if (!s_lock) {
        esp_err_t ir = drv_meter_init();
        if (ir != ESP_OK) return ir;
    }
    if (!s_handle) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    meter_teardown_locked();
    meter_raw_free_all();
    s_n = 0;

    for (int i = 0; i < 10; i++) s_ch_map[i] = -1;

    /* 统计激活通道数 + 参数校验 */
    int n = 0;
    for (int i = 0; i < METER_CH_MAX; i++) {
        if (io[i] < 0) continue;
        if (io[i] < 1 || io[i] > 10) {   /* ADC2 不接受 */
            xSemaphoreGive(s_lock);
            ESP_LOGE(M_TAG, "start: io%d not ADC1", io[i]);
            return ESP_ERR_INVALID_ARG;
        }
        n++;
    }
    if (n == 0) {
        xSemaphoreGive(s_lock);
        ESP_LOGI(M_TAG, "start: no active channel (stop)");
        return ESP_OK;
    }
    s_n = n;

    /* 分配 ring（槽位索引 = 通道索引）+ pattern（busy 校验）。
     * pattern 必须紧凑填充（驱动按 pattern_num 顺序读 pat[0..n-1]，
     * 不能用槽位索引——激活槽位不连续时会出现未初始化项） */
    adc_digi_pattern_config_t pat[METER_CH_MAX];
    int pcnt = 0;
    for (int i = 0; i < METER_CH_MAX; i++) {
        if (io[i] < 0) continue;

        adc_unit_t unit;
        adc_channel_t chan;
        esp_err_t ret = adc_continuous_io_to_channel(io[i], &unit, &chan);
        if (ret != ESP_OK || unit != ADC_UNIT_1) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(M_TAG, "start: io%d not ADC1", io[i]);
            return ESP_ERR_INVALID_ARG;
        }
        /* 同 GPIO 被其它槽占用 → 拒绝 */
        for (int j = 0; j < METER_CH_MAX; j++) {
            if (j != i && io[j] == io[i]) {
                xSemaphoreGive(s_lock);
                ESP_LOGE(M_TAG, "start: io%d in two slots", io[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
        s_ch_map[chan] = i;
        s_ch[i].io = io[i];
        s_ch[i].rate_hz = METER_RATE_HZ / n;
        s_ch[i].ring_n = (uint32_t)s_ch[i].rate_hz * METER_WINDOW_S;
        s_ch[i].ring = heap_caps_malloc(s_ch[i].ring_n * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ch[i].ring) {
            /* 回滚：释放已分配 ring，保持无残留 */
            meter_raw_free_all();
            xSemaphoreGive(s_lock);
            ESP_LOGE(M_TAG, "start: ring %d alloc failed (%ukB)", i,
                     (unsigned)(s_ch[i].ring_n * 2 / 1024));
            return ESP_ERR_NO_MEM;
        }
        s_ch[i].wr = 0;
        s_ch[i].written = 0;
        s_ch[i].min_raw = 4095;
        s_ch[i].max_raw = 0;
        s_ch[i].last_raw = 0;

        pat[pcnt++] = (adc_digi_pattern_config_t){
            .atten = METER_ATTEN,
            .channel = chan,
            .unit = ADC_UNIT_1,
            .bit_width = ADC_BITWIDTH_12,
        };
    }

    adc_continuous_config_t cont = {
        .sample_freq_hz = METER_RATE_HZ,
        .pattern_num = n,
        .adc_pattern = pat,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    esp_err_t ret = adc_continuous_config(s_handle, &cont);
    if (ret != ESP_OK) {
        meter_raw_free_all();   /* 锁内释放（与其他失败路径一致） */
        xSemaphoreGive(s_lock);
        ESP_LOGE(M_TAG, "start: config FAILED (%dHz x%d): %s", METER_RATE_HZ, n,
                 esp_err_to_name(ret));
        return ret;
    }
    ret = adc_continuous_start(s_handle);
    if (ret != ESP_OK) {
        meter_raw_free_all();   /* 锁内释放 */
        xSemaphoreGive(s_lock);
        ESP_LOGE(M_TAG, "start: adc_continuous_start FAILED: %s", esp_err_to_name(ret));
        return ret;
    }

    s_run = true;
    ret = xTaskCreateWithCaps(meter_task, "meter", 4096, NULL, 6, &s_task, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        s_run = false;
        adc_continuous_stop(s_handle);
        xSemaphoreGive(s_lock);
        ESP_LOGE(M_TAG, "start: task create FAILED");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(M_TAG, "start ok (100k x%d ch)", n);
    return ESP_OK;
}

esp_err_t drv_meter_stop(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    meter_teardown_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t drv_meter_deinit(void)
{
    if (!s_lock) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    meter_teardown_locked();
    meter_raw_free_all();
    if (s_cali) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_handle) {
        adc_continuous_deinit(s_handle);
        s_handle = NULL;
    }
    if (s_raw_buf) {
        heap_caps_free(s_raw_buf);
        s_raw_buf = NULL;
    }
    xSemaphoreGive(s_lock);
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return ESP_OK;
}

esp_err_t drv_meter_get_snap(int ch, meter_ch_snap_t *out)
{
    if (!out || ch < 0 || ch >= METER_CH_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const meter_ch_t *c = &s_ch[ch];
    if (!c->ring) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    out->io = c->io;
    out->rate_hz = c->rate_hz;
    out->running = s_run;
    out->points = c->ring_n;
    out->v_now = meter_raw_to_mv(c->last_raw) / 1000.0f;
    out->v_min = meter_raw_to_mv(c->min_raw) / 1000.0f;
    out->v_max = meter_raw_to_mv(c->max_raw) / 1000.0f;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t drv_meter_get_wave(int ch, int ncols,
                             float *col_min, float *col_max,
                             float *win_min, float *win_max)
{
    if (!col_min || !col_max || !win_min || !win_max) return ESP_ERR_INVALID_ARG;
    if (ch < 0 || ch >= METER_CH_MAX || ncols < 1) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const meter_ch_t *c = &s_ch[ch];
    if (!c->ring || c->written == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* ring 内有效样本数（未满 = 从头开始的 written 个；满 = 全部 ring_n） */
    uint32_t avail = c->written < c->ring_n ? c->written : c->ring_n;
    uint32_t start = (c->wr - avail + c->ring_n) % c->ring_n;   /* 最老样本位置 */

    uint32_t gmin = 4096, gmax = 0;
    for (int col = 0; col < ncols; col++) {
        uint32_t i0 = (uint32_t)col * avail / ncols;
        uint32_t i1 = (uint32_t)(col + 1) * avail / ncols;
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > avail) i1 = avail;
        uint32_t mn = 4096, mx = 0;
        for (uint32_t k = i0; k < i1; k++) {
            uint16_t v = c->ring[(start + k) % c->ring_n];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        col_min[col] = meter_raw_to_mv((uint16_t)mn) / 1000.0f;
        col_max[col] = meter_raw_to_mv((uint16_t)mx) / 1000.0f;
        if (mn < gmin) gmin = mn;
        if (mx > gmax) gmax = mx;
    }
    *win_min = meter_raw_to_mv((uint16_t)gmin) / 1000.0f;
    *win_max = meter_raw_to_mv((uint16_t)gmax) / 1000.0f;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}