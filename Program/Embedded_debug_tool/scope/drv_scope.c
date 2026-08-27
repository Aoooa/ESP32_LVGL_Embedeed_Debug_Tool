/* drv_scope.c —— 示波器采集驱动：官方 adc_continuous DMA + 软件触发 + 测量。
 * 数据流：adc_continuous_read_parse 拉帧 → 应用层环形缓冲（PSRAM）→
 * 触发检测（边沿+电平）→ 窗口快照（预触发 25%）→ 测量 → 互斥锁帧。 */

#include "drv_scope.h"
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

#define S_TAG "drv_scope"

#define SCOPE_ATTEN        ADC_ATTEN_DB_12   /* 满量程 ~3.1V（0..3.1V 单极性） */
#define SCOPE_FRAME_BYTES  1024              /* conv_frame_size（每 DMA 帧 256 样本）。
                                              * 原 2048 时 rx_dma_buf=5×2048=10KB 连续内部
                                              * DMA RAM，行程 RAM 余量不足导致 new_handle
                                              * ESP_ERR_NO_MEM；1024 → 5KB，吞吐不变 */
#define SCOPE_RAW_BUF_BYTES 1024             /* read 缓冲（1 个 DMA 帧；4B/样本） */
#define SCOPE_READ_TIMEOUT 100               /* 拉帧阻塞超时 ms */
#define SCOPE_PRE_TRIG_RATIO 4    /* 预触发 = 窗口的 1/4（25%），动态窗口下按比例 */

/* 驱动状态（s_lock 保护；采集任务内 s_ring/s_wr 单写者无需锁） */
static adc_continuous_handle_t s_handle;
static adc_cali_handle_t s_cali;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_run;
static scope_cfg_t s_cfg;

static int s_ch_map[10];           /* ADC1 channel(0-9) -> 应用索引(0/1)，-1=不用 */
static uint16_t *s_ring[2];        /* 环形缓冲（PSRAM） */
static uint32_t s_wr[2];           /* 每通道独立写指针（触发窗口必须按 CH1 样本数计） */
static uint32_t s_written[2];      /* 每通道总写入数（不回绕，判断缓冲是否填满） */
static int s_trig_wr;              /* 触发点环形位置（CH1 索引，-1=无） */
static scope_frame_t *s_frame;     /* 最新帧快照（PSRAM，s_lock 保护） */
static uint32_t *s_raw_buf;        /* read 原始 4B DMA 数据缓冲（PSRAM） */

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
    /* 过零法频率：上升沿过中线计数（每周期 1 次）→ f = cross × rate / n
     * rate 用 f->sample_rate_hz（Dual 下已减半，见帧更新） */
    uint32_t cross = 0;
    for (i = 1; i < (uint32_t)n; i++) {
        if (w[i - 1] < mid && w[i] >= mid) cross++;
    }
    f->freq_hz = (float)cross * f->sample_rate_hz / (float)n;

    /* 占空比 + 脉宽（中线以上） */
    uint32_t hi = 0, runs = 0, cur = 0;
    for (i = 0; i < (uint32_t)n; i++) {
        if (w[i] >= mid) { hi++; cur++; }
        else if (cur) { runs++; cur = 0; }
    }
    if (cur) runs++;
    f->duty_pct = 100.0f * hi / n;
    f->pw_ms = runs ? (float)hi / runs * 1000.0f / f->sample_rate_hz : 0.0f;
}

/* ── 采集任务：拉帧 → 环形缓冲 → 触发 → 窗口快照 ── */
static void scope_task(void *arg)
{
    (void)arg;

    /* 采集任务优先级 6，1MHz 下长时间占用 CPU0 → 饿死 IDLE0 触发任务看门狗
     * （TWDT 5s 超时打断任务，表现 = 运行 ~7s 后帧停摆）。注册 TWDT + 每批
     * 喂狗 + taskYIELD 让出，IDLE0 得以喂狗；read 阻塞等待时任务本就让出 */
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();

    while (s_run) {
        esp_task_wdt_reset();   /* 每批喂狗 */

        /* 直接 read 原始 4B DMA 数据（绕开 read_parse 的 12B 结构体展开——
         * 1MHz 下 parse 的 PSRAM 读写是吞吐瓶颈）。一次 read 拿 1 个 DMA 帧
         * （conv_frame_size=2048B=512 样本），手写位域解析。 */
        uint32_t out_len = 0;
        esp_err_t ret = adc_continuous_read(s_handle, (uint8_t *)s_raw_buf, SCOPE_RAW_BUF_BYTES, &out_len,
                                            SCOPE_READ_TIMEOUT);
        if (ret != ESP_OK || out_len == 0) continue;
        uint32_t n = out_len / 4;   /* 每样本 4B（type2） */

        /* S3 type2 原始格式（32 位字，官方 adc_types.h:213）：
         * bit0-11 = data(12bit)，bit12 = reserved，bit13-16 = channel(4bit)，
         * bit17 = unit(0=ADC1/1=ADC2)。直接位域解析，免 12B 结构体 */
        const uint32_t *p = (const uint32_t *)s_raw_buf;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t raw = p[i];
            if ((raw >> 17) & 1) continue;            /* unit=1 → ADC2，跳过（S3 只用 ADC1） */
            int ch = (int)((raw >> 13) & 0xF);
            int idx = (ch < 10) ? s_ch_map[ch] : -1;
            if (idx < 0) continue;

            uint16_t v = (uint16_t)(raw & 0xFFF);
            s_ring[idx][s_wr[idx]] = v;

            /* 主通道（idx==0）触发检测：边沿跨过阈值（按 CH1 自身写指针） */
            if (idx == 0 && s_trig_wr < 0) {
                uint32_t prev = (s_wr[0] + SCOPE_RING_N - 1) % SCOPE_RING_N;
                uint16_t pv = s_ring[0][prev];
                bool hit = (s_cfg.edge == SCOPE_EDGE_RISING)
                               ? (pv < (uint16_t)s_cfg.trigger_level && v >= (uint16_t)s_cfg.trigger_level)
                               : (pv > (uint16_t)s_cfg.trigger_level && v <= (uint16_t)s_cfg.trigger_level);
                if (hit) s_trig_wr = (int)s_wr[0];
            }
            s_wr[idx] = (s_wr[idx] + 1) % SCOPE_RING_N;
            s_written[idx]++;
        }

        /* 帧更新：挂起触发等补满（预触发 25%），无触发 AUTO 滚动。
         * 窗口长度（wp = s_cfg.window_points）按 CH1 样本数计算——
         * Dual 时两通道样本交替写入各自缓冲，共用写指针会导致窗口
         * 计数翻倍、取到错乱位置（波形竖线 bug 根因） */
        int wp = s_cfg.window_points;
        if (wp < 64) wp = 64;
        if (wp > SCOPE_FRAME_MAX) wp = SCOPE_FRAME_MAX;
        int pre = wp / SCOPE_PRE_TRIG_RATIO;      /* 25% 预触发 */
        int post = wp - pre;                      /* 75% 触发后 */
        int start = -1;
        bool trig_frame = false;
        if (s_trig_wr >= 0) {
            uint32_t after = (s_wr[0] - 1 - (uint32_t)s_trig_wr + SCOPE_RING_N) % SCOPE_RING_N;
            if (after >= (uint32_t)post) {
                start = (s_trig_wr - pre + SCOPE_RING_N) % SCOPE_RING_N;
                trig_frame = true;
            } else {
                continue;   /* 触发后未补满：等下一批（AUTO 也等，触发优先于滚动） */
            }
        } else if (s_cfg.trig_mode == SCOPE_TRIG_AUTO) {
            if (s_written[0] < (uint32_t)wp) continue;
            start = (int)((s_wr[0] - (uint32_t)wp + SCOPE_RING_N) % SCOPE_RING_N);
        } else {
            continue;   /* NORM 无触发不更新 */
        }

        /* 快照窗口 + 测量（写 s_frame 全程持锁，与 get_frame 锁内 memcpy 同步） */
        int nch = (s_cfg.io[1] >= 0) ? 2 : 1;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int c = 0; c < nch; c++) {
            for (int k = 0; k < wp; k++) {
                s_frame->ch[c][k] = s_ring[c][(start + k) % SCOPE_RING_N];
            }
        }
        s_frame->points = wp;
        s_frame->channels = nch;
        /* 每通道采样率：Dual 下 MUX 分时减半（测量换算依据，否则偏大 2 倍） */
        s_frame->sample_rate_hz = s_cfg.sample_rate_hz / nch;
        s_frame->running = s_run;
        scope_measure(s_frame->ch[0], wp, s_frame);
        s_frame->frameno++;
        xSemaphoreGive(s_lock);

        /* 触发窗口已消费 */
        s_trig_wr = -1;

        /* SINGLE：触发一次后停止采集（任务自删，UI 显示冻结） */
        if (trig_frame && s_cfg.trig_mode == SCOPE_TRIG_SINGLE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_run = false;
            s_frame->running = false;   /* 冻结帧标记停止 */
            xSemaphoreGive(s_lock);
        }

        /* 让出 CPU（不延迟）：1MHz 下 read 大部分时间立即返回（不阻塞），任务
         * 会连续占用 CPU0 饿死 IDLE0 → TWDT 5s 超时触发（实测 ~5.7s 帧停）。
         * taskYIELD 让 IDLE0 得以喂狗；对吞吐影响 ~0（read 阻塞等待时本就让出，
         * 实测 100 万/s 稳定 ovf=0）。不用 vTaskDelay(1)：会真实让出 1ms，
         * 1MHz 下每批 512 样本仅 0.5ms 数据，1ms 让出会砍半吞吐。 */
        taskYIELD();
    }

    /* 任务退出：持锁清句柄（start/stop 靠它判断任务存活），再自删 */
    esp_task_wdt_delete(NULL);
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
        if (s_task) {   /* 任务未及时退出（异常）：强删（先注销其 TWDT 订阅） */
            esp_task_wdt_delete(s_task);
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
    if (s_handle) adc_continuous_stop(s_handle);
}

esp_err_t drv_scope_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(S_TAG, "init: mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < 10; i++) s_ch_map[i] = -1;

    /* 环形缓冲 + 帧快照（PSRAM） */
    for (int c = 0; c < SCOPE_CH_MAX; c++) {
        s_ring[c] = heap_caps_malloc(SCOPE_RING_N * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring[c]) {
            ESP_LOGE(S_TAG, "init: ring[%d] alloc failed (%dKB PSRAM)", c,
                     (int)(SCOPE_RING_N * sizeof(uint16_t) / 1024));
            goto err;
        }
    }
    s_frame = heap_caps_malloc(sizeof(scope_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) goto err;
    memset(s_frame, 0, sizeof(scope_frame_t));

    /* read 原始 4B DMA 数据缓冲（PSRAM；1 个 DMA 帧 = 2048B） */
    s_raw_buf = heap_caps_malloc(SCOPE_RAW_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_raw_buf) goto err;

    /* adc_continuous 句柄。1MHz 方案（vendored）：
     *  - rx_dma_buf 保持内部 DMA RAM（10KB = 5×conv 2048）：PSRAM 版实测
     *    ~6s 后 DMA 数据流崩溃（ISR esp_cache_msync 对 PSRAM 非对齐地址周期性
     *    失效，cache 回写覆盖 DMA 数据），内部 RAM 余量（largest~15KB）足够。
     *  - ringbuf_storage（store 32KB）在 PSRAM：纯 CPU 读写（ISR send/任务
     *    receive），无 DMA 直访，安全；1MHz 4MB/s 下 8ms 防丢缓冲。
     *  - 采集任务已加 TWDT 喂狗 + 每批让出（饿死 IDLE0 触发 WDT 的 5s 停摆
     *    根因，已修复）。 */
    adc_continuous_handle_cfg_t hdl = {
        .max_store_buf_size = 32 * 1024,
        .conv_frame_size = 2048,
    };
    esp_err_t ret = adc_continuous_new_handle(&hdl, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(S_TAG, "init: adc_continuous_new_handle FAILED: %s (internal RAM?)",
                 esp_err_to_name(ret));
        goto err;
    }
    ESP_LOGI(S_TAG, "init: handle ok (store=%d conv=%d)", hdl.max_store_buf_size, hdl.conv_frame_size);

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
            ESP_LOGW(S_TAG, "init: cali check failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(S_TAG, "init: cali scheme failed: %s, fallback raw*3100/4095", esp_err_to_name(ret));
        s_cali = NULL;   /* 校准失败可继续（近似换算） */
    }

    ESP_LOGI(S_TAG, "init ok (ring %d pts, frame max %d pts)", SCOPE_RING_N, SCOPE_FRAME_MAX);
    return ESP_OK;

err:
    /* 部分分配失败：释放已分配资源 + 删锁，保证 s_lock=NULL 可重试 */
    for (int c = 0; c < SCOPE_CH_MAX; c++) {
        if (s_ring[c]) { heap_caps_free(s_ring[c]); s_ring[c] = NULL; }
    }
    if (s_raw_buf) { heap_caps_free(s_raw_buf); s_raw_buf = NULL; }
    if (s_frame) { heap_caps_free(s_frame); s_frame = NULL; }
    if (s_handle) { adc_continuous_deinit(s_handle); s_handle = NULL; }
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    ESP_LOGE(S_TAG, "init FAILED (internal free=%u largest=%u, psram free=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_ERR_NO_MEM;
}

esp_err_t drv_scope_start(const scope_cfg_t *cfg)
{
    /* 惰性初始化：启动期不占内部 RAM，首次进 Scope 才 init */
    if (!s_lock) {
        esp_err_t ir = drv_scope_init();
        if (ir != ESP_OK) {
            ESP_LOGE(S_TAG, "start: lazy init failed: %s", esp_err_to_name(ir));
            return ir;
        }
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
            ESP_LOGE(S_TAG, "start: io %d not ADC1: %s", cfg->io[i], esp_err_to_name(ret));
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
        ESP_LOGE(S_TAG, "start: config FAILED (%d Hz x%d ch): %s", cfg->sample_rate_hz, nch,
                 esp_err_to_name(ret));
        return ret;
    }
    ret = adc_continuous_start(s_handle);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(S_TAG, "start: adc_continuous_start FAILED: %s", esp_err_to_name(ret));
        return ret;
    }

    s_wr[0] = s_wr[1] = 0;
    s_written[0] = s_written[1] = 0;
    s_trig_wr = -1;
    s_frame->frameno = 0;
    s_frame->running = true;
    s_run = true;

    /* 优先 6（高于 LVGL 5/网络 5）：1MHz 下任务需抢占 CPU 及时消费 DMA，
     * 否则 store ringbuf 溢出丢样；不高过 10 避免极端饿死系统任务 */
    ret = xTaskCreateWithCaps(scope_task, "scope", 4096, NULL, 6, &s_task, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        s_run = false;
        adc_continuous_stop(s_handle);
        xSemaphoreGive(s_lock);
        ESP_LOGE(S_TAG, "start: task create FAILED");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(S_TAG, "start ok (%dHz, %d ch)", cfg->sample_rate_hz, nch);
    return ESP_OK;
}

esp_err_t drv_scope_stop(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scope_teardown_locked();
    if (s_frame) s_frame->running = false;
    xSemaphoreGive(s_lock);
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
    if (s_raw_buf) heap_caps_free(s_raw_buf);
    s_raw_buf = NULL;
    if (s_frame) heap_caps_free(s_frame);
    s_frame = NULL;
    xSemaphoreGive(s_lock);
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
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
