/* drv_wave.c —— 波形输出驱动：LEDC PWM/方波 / 正弦调制 / 按钮脉冲。
 * 多通道（WAVE_CH_COUNT=7），PWM 类模式独占 LEDC 定时器池（4 个），
 * PULSE 用 GPIO 直驱（不占定时器，完成后自动停止并回调通知）。 */

#include "drv_wave.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_timer.h"
#include "esp_rom_gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define W_TAG "drv_wave"

#define WAVE_SINE_TICK_US   100      /* 正弦占空比更新周期（10kHz） */
#define WAVE_SINE_PHASE_BITS 16      /* 相位定点位数（16.16） */
#define WAVE_SINE_TABLE_LEN 256

/* 硬件约束：S3 LEDC 分频整数最大 1023 + res 上限 14 →
 * 最低频率 = 80MHz/(2^14×1023) ≈ 4.77Hz，故 PWM/SQUARE 下限 5Hz */
#define WAVE_SQUARE_FREQ_MIN    5
#define WAVE_SQUARE_FREQ_MAX    1000000
#define WAVE_SINE_FREQ_MAX      200
#define WAVE_PULSE_MS_MIN       10   /* 系统 tick 10ms，低于此无法精确 */
#define WAVE_PULSE_MS_MAX       5000

/* ── 正弦查表（256 点，int16） ── */
static const int16_t s_sine_table[WAVE_SINE_TABLE_LEN] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
    9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
    25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
    32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268,
    28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151,
    15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410,
    -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
    -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
    -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
    -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
    -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011,
    -3212, -2410, -1608, -804,
};
_Static_assert(sizeof(s_sine_table) / sizeof(s_sine_table[0]) == WAVE_SINE_TABLE_LEN,
               "sine table must have exactly 256 entries");

typedef struct {
    int io;
    wave_type_t type;
    int freq_hz;
    int duty_pct;
    int pulse_ms;
    bool running;          /* 有生效波形（非 OFF） */
    int timer_id;          /* 占用的 LEDC 定时器（PWM 类；-1=未占用） */
    int ledc_res;          /* 当前 LEDC 占空比分辨率（bit） */
    esp_timer_handle_t sine_timer;
    TaskHandle_t pulse_task;
    volatile int pulse_go; /* 通知脉冲任务执行（0=空闲） */
    /* 正弦调制状态（esp_timer 任务回调更新） */
    uint32_t sine_phase;
    uint32_t sine_inc;
    int sine_amp;
} wave_ch_t;

static wave_ch_t s_ch[WAVE_CH_COUNT];
static SemaphoreHandle_t s_wave_mutex;   /* apply/stop/get 互斥 */
static bool s_wave_inited;
static int s_timer_owner[WAVE_TIMER_COUNT];   /* LEDC 定时器归属通道（-1=空闲） */

static const char *const s_type_names[] = {
    "OFF", "PWM", "SQUARE", "SINE", "PULSE",
};

const char *drv_wave_type_str(wave_type_t t)
{
    if (t < WAVE_OFF || t > WAVE_PULSE) return "?";
    return s_type_names[t];
}

bool drv_wave_type_uses_timer(wave_type_t t)
{
    return (t == WAVE_PWM || t == WAVE_SQUARE || t == WAVE_SINE);
}

/* ── GPIO 辅助 ── */

static void wave_gpio_out(int io)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << io,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    /* 强制 GPIO matrix 输出路由回到纯 GPIO（清掉残留 LEDC 信号） */
    esp_rom_gpio_connect_out_signal(io, SIG_GPIO_OUT_IDX, false, false);
}

static void wave_gpio_release(int io)
{
    if (io < 0) return;
    for (int i = 0; i < WAVE_CH_COUNT; i++) {
        if (s_ch[i].io == io && s_ch[i].running) return;
    }
    gpio_reset_pin(io);
}

/* ── LEDC 定时器池 ── */

static int wave_timer_alloc(int ch)
{
    for (int i = 0; i < WAVE_TIMER_COUNT; i++) {
        if (s_timer_owner[i] < 0) {
            s_timer_owner[i] = ch;
            return i;
        }
    }
    return -1;
}

static void wave_timer_free(int ch)
{
    for (int i = 0; i < WAVE_TIMER_COUNT; i++) {
        if (s_timer_owner[i] == ch) s_timer_owner[i] = -1;
    }
}

/* ── LEDC：动态分辨率（高频→低分辨率，保证分频 >=1 且 <=1023） ── */

static int wave_pick_resolution(uint32_t freq_hz)
{
    int r = 14;   /* S3 LEDC 最大 14bit */
    while (r > 4) {
        if (((uint64_t)1 << r) * freq_hz <= 80000000ULL) break;
        r--;
    }
    return r;
}

static esp_err_t wave_ledc_timer_set(wave_ch_t *c, uint32_t freq_hz)
{
    int res = wave_pick_resolution(freq_hz);
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = (ledc_timer_t)c->timer_id,
        .duty_resolution = (ledc_timer_bit_t)res,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    esp_err_t err = ledc_timer_config(&tc);
    if (err == ESP_OK) c->ledc_res = res;
    return err;
}

static esp_err_t wave_ledc_duty(wave_ch_t *c, int duty_pct)
{
    int res = c->ledc_res;
    if (res <= 0) res = 10;
    uint32_t max_duty = (1U << res) - 1;
    uint32_t duty = (uint32_t)(((uint64_t)max_duty * duty_pct + 50) / 100);
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(c - s_ch), duty);
    if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(c - s_ch));
    return err;
}

static esp_err_t wave_ledc_channel(wave_ch_t *c, int io)
{
    ledc_channel_config_t cc = {
        .gpio_num = io,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)(c - s_ch),
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)c->timer_id,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&cc);
}

/* ── 正弦调制（esp_timer 任务回调，10kHz 更新占空比） ── */

static void wave_sine_tick(void *arg)
{
    wave_ch_t *c = arg;
    c->sine_phase += c->sine_inc;
    uint8_t idx = (uint8_t)(c->sine_phase >> (32 - 8));
    int32_t sinv = s_sine_table[idx];
    /* duty(0.1%) = 500 + amp(%) * 10 * sin / 32768（16.15 定点） */
    int permille = 500 + (int32_t)(((int64_t)c->sine_amp * 10 * sinv) >> 15);
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    int res = c->ledc_res;
    if (res <= 0) return;
    uint32_t max_duty = (1U << res) - 1;
    uint32_t duty = (uint32_t)((uint64_t)max_duty * permille / 1000);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(c - s_ch), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(c - s_ch));
}

/* ── 按钮脉冲常驻任务（模拟按键：默认高，脉冲期拉低，完成后自动停止） ── */

static void wave_pulse_task(void *arg)
{
    wave_ch_t *c = arg;
    for (;;) {
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
        if (!c->pulse_go) continue;
        c->pulse_go = 0;
        int io = c->io, ms = c->pulse_ms;
        if (io < 0 || ms <= 0) continue;
        gpio_set_level(io, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));   /* 初始高保持（按钮未按下） */
        gpio_set_level(io, 0);           /* 脉冲期拉低 */
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(io, 1);           /* 结束回高 */

        /* 脉冲完成：自动停止。仅当无新脉冲排队（pulse_go=0）且仍是 PULSE
         * 模式时清理——否则保留状态让新脉冲继续（防重触发竞态）。
         * 回调已移除：UI 用 100ms 轮询 drv_wave_ch_active 同步指示灯。 */
        if (s_wave_mutex) xSemaphoreTake(s_wave_mutex, portMAX_DELAY);
        if (c->type == WAVE_PULSE && c->running && !c->pulse_go) {
            c->running = false;
            wave_gpio_release(c->io);
            c->io = -1;
        }
        if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
    }
}

/* ── 停止当前输出（按类型） ── */

static void wave_ch_halt(wave_ch_t *c)
{
    int ch = (int)(c - s_ch);
    if (c->sine_timer) {
        esp_timer_stop(c->sine_timer);
        esp_timer_delete(c->sine_timer);
        c->sine_timer = NULL;
    }
    if (drv_wave_type_uses_timer(c->type)) {
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
        /* LEDC 停止后 GPIO matrix 仍路由 LEDC 信号；靠后续 wave_gpio_out 的
         * SIG_GPIO_OUT_IDX 强制切回纯 GPIO */
    }
    if (c->timer_id >= 0) {
        wave_timer_free(ch);
        c->timer_id = -1;
    }
    c->running = false;   /* 先清运行标志，再释放 GPIO（wave_gpio_release 查 running） */
    if (c->io >= 0 && c->type != WAVE_OFF) {
        wave_gpio_release(c->io);
    }
}

/* ── Public API ── */

esp_err_t drv_wave_init(void)
{
    if (s_wave_inited) return ESP_OK;   /* 幂等 */
    memset(s_ch, 0, sizeof(s_ch));
    s_wave_mutex = xSemaphoreCreateMutex();
    if (!s_wave_mutex) {
        ESP_LOGE(W_TAG, "mutex create failed");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < WAVE_CH_COUNT; i++) {
        s_ch[i].io = -1;
        s_ch[i].type = WAVE_OFF;
        s_ch[i].timer_id = -1;
        BaseType_t ok = xTaskCreateWithCaps(wave_pulse_task, "wave_pulse", 2048,
                                            &s_ch[i], 5, &s_ch[i].pulse_task,
                                            MALLOC_CAP_SPIRAM);   /* 内部 RAM 紧张 */
        if (ok != pdPASS) {
            ESP_LOGE(W_TAG, "ch%d pulse task create failed", i);
        }
    }
    for (int i = 0; i < WAVE_TIMER_COUNT; i++) s_timer_owner[i] = -1;
    s_wave_inited = true;
    ESP_LOGI(W_TAG, "init ok (%d channels, %d timers)", WAVE_CH_COUNT, WAVE_TIMER_COUNT);
    return ESP_OK;
}

esp_err_t drv_wave_ch_apply(int ch, const wave_ch_cfg_t *cfg)
{
    if (ch < 0 || ch >= WAVE_CH_COUNT) return ESP_ERR_INVALID_ARG;
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* 参数校验（返回前不破坏当前状态） */
    if (cfg->io < 0) return ESP_ERR_INVALID_ARG;
    if (cfg->type == WAVE_PWM || cfg->type == WAVE_SQUARE) {
        if (cfg->freq_hz < WAVE_SQUARE_FREQ_MIN || cfg->freq_hz > WAVE_SQUARE_FREQ_MAX) return ESP_ERR_INVALID_ARG;
        if (cfg->duty_pct < 1 || cfg->duty_pct > 99) return ESP_ERR_INVALID_ARG;
    } else if (cfg->type == WAVE_SINE) {
        if (cfg->freq_hz < 1 || cfg->freq_hz > WAVE_SINE_FREQ_MAX) return ESP_ERR_INVALID_ARG;
        if (cfg->duty_pct < 0 || cfg->duty_pct > 50) return ESP_ERR_INVALID_ARG;
    } else if (cfg->type == WAVE_PULSE) {
        if (cfg->pulse_ms < WAVE_PULSE_MS_MIN || cfg->pulse_ms > WAVE_PULSE_MS_MAX) return ESP_ERR_INVALID_ARG;
    }

    if (s_wave_mutex) xSemaphoreTake(s_wave_mutex, portMAX_DELAY);

    /* 同 GPIO 冲突检测（其他通道正在占用） */
    for (int i = 0; i < WAVE_CH_COUNT; i++) {
        if (i != ch && s_ch[i].running && s_ch[i].io == cfg->io) {
            if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
            ESP_LOGW(W_TAG, "ch%d io=%d conflict with ch%d", ch, cfg->io, i);
            return ESP_ERR_INVALID_ARG;
        }
    }

    wave_ch_t *c = &s_ch[ch];
    wave_ch_halt(c);

    if (cfg->type == WAVE_OFF) {
        c->io = -1;
        c->type = WAVE_OFF;
        if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
        return ESP_OK;
    }

    /* PWM 类模式：分配定时器（池满拒绝） */
    if (drv_wave_type_uses_timer(cfg->type)) {
        int tid = wave_timer_alloc(ch);
        if (tid < 0) {
            c->io = -1;
            c->type = WAVE_OFF;
            if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
            ESP_LOGW(W_TAG, "ch%d no free LEDC timer", ch);
            return ESP_ERR_INVALID_STATE;
        }
        c->timer_id = tid;
    }

    /* GPIO：总是重新配置为输出 */
    if (cfg->io != c->io && c->io >= 0) wave_gpio_release(c->io);
    wave_gpio_out(cfg->io);
    c->io = cfg->io;
    c->type = cfg->type;
    c->freq_hz = cfg->freq_hz;
    c->duty_pct = cfg->duty_pct;
    c->pulse_ms = cfg->pulse_ms;

    esp_err_t err = ESP_OK;
    switch (c->type) {
    case WAVE_PWM:
    case WAVE_SQUARE:
        err = wave_ledc_timer_set(c, c->freq_hz);
        if (err == ESP_OK) err = wave_ledc_channel(c, c->io);
        if (err == ESP_OK) err = wave_ledc_duty(c, c->duty_pct);
        break;
    case WAVE_SINE: {
        c->sine_amp = c->duty_pct;
        c->sine_phase = 0;
        c->sine_inc = (uint32_t)(((uint64_t)c->freq_hz * (1ULL << WAVE_SINE_PHASE_BITS)
                                  * WAVE_SINE_TICK_US + 500000ULL) / 1000000ULL);
        esp_timer_create_args_t ta = {
            .callback = wave_sine_tick,
            .arg = c,
            .name = "wave_sine",
        };
        err = esp_timer_create(&ta, &c->sine_timer);
        if (err == ESP_OK) err = wave_ledc_timer_set(c, 9766);   /* 载波 ~9.77kHz */
        if (err == ESP_OK) err = wave_ledc_channel(c, c->io);
        if (err == ESP_OK) err = wave_ledc_duty(c, 50);
        if (err == ESP_OK) {
            err = esp_timer_start_periodic(c->sine_timer, WAVE_SINE_TICK_US);
        }
        if (err != ESP_OK && c->sine_timer) {
            esp_timer_delete(c->sine_timer);
            c->sine_timer = NULL;
        }
        break;
    }
    case WAVE_PULSE:
        gpio_set_level(c->io, 1);
        c->pulse_go = 1;
        if (c->pulse_task) xTaskNotify(c->pulse_task, 0, eNoAction);
        break;
    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    if (err != ESP_OK) {
        if (drv_wave_type_uses_timer(c->type)) {
            ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
        }
        if (c->timer_id >= 0) {
            wave_timer_free(ch);
            c->timer_id = -1;
        }
        wave_gpio_release(c->io);
        c->io = -1;
        c->type = WAVE_OFF;
        c->running = false;
        if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
        ESP_LOGE(W_TAG, "ch%d apply failed %d, rolled back", ch, err);
        return err;
    }

    c->running = true;
    if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
    ESP_LOGI(W_TAG, "ch%d %s io=%d freq=%d duty=%d%% ms=%d",
             ch, drv_wave_type_str(c->type), c->io, c->freq_hz, c->duty_pct, c->pulse_ms);
    return ESP_OK;
}

esp_err_t drv_wave_ch_stop(int ch)
{
    if (ch < 0 || ch >= WAVE_CH_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_wave_mutex) xSemaphoreTake(s_wave_mutex, portMAX_DELAY);
    wave_ch_halt(&s_ch[ch]);
    s_ch[ch].io = -1;
    s_ch[ch].type = WAVE_OFF;
    if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
    return ESP_OK;
}

bool drv_wave_ch_active(int ch)
{
    if (ch < 0 || ch >= WAVE_CH_COUNT) return false;
    return s_ch[ch].running;
}

void drv_wave_ch_get(int ch, wave_ch_cfg_t *out)
{
    if (!out) return;
    if (ch < 0 || ch >= WAVE_CH_COUNT) {
        memset(out, 0, sizeof(*out));
        out->io = -1;
        return;
    }
    if (s_wave_mutex) xSemaphoreTake(s_wave_mutex, portMAX_DELAY);
    const wave_ch_t *c = &s_ch[ch];
    out->io = c->io;
    out->type = c->type;
    out->freq_hz = c->freq_hz;
    out->duty_pct = c->duty_pct;
    out->pulse_ms = c->pulse_ms;
    if (s_wave_mutex) xSemaphoreGive(s_wave_mutex);
}

int drv_wave_timers_in_use(void)
{
    int n = 0;
    for (int i = 0; i < WAVE_TIMER_COUNT; i++) {
        if (s_timer_owner[i] >= 0) n++;
    }
    return n;
}
