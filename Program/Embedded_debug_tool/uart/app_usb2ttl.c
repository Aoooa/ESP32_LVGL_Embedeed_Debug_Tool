/* app_usb2ttl.c —— USB CDC-ACM 虚拟串口 ↔ UART1 桥接 + ISP 下载控制
 *
 * 数据流：
 *   PC → CDC RX FIFO → 桥接任务 → UART1 TX（IO2）
 *   UART1 RX（IO4）→ 桥接任务 → CDC TX FIFO → PC
 *
 * TinyUSB 采用 esp_tinyusb 官方 CDC-ACM 包装（tinyusb_cdcacm_*）而非裸
 * tud_cdc_* 回调：该组件已定义强符号 tud_cdc_* 回调（tinyusb_cdc_acm.c），
 * 与 DAP（裸 HID 回调）不同，此处复用官方包装注册事件即可，避免重复定义。
 *
 * 描述符为自定义（device/config/string），与 DAP 同一模式：
 *   tinyusb_driver_install(&cfg) → tinyusb_cdcacm_init(&acm_cfg)。
 */

#include "app_usb2ttl.h"
#include "app_uart.h"          /* g_bridges / uart_bridge_t.paused */
#include "app_cardreader.h"
#include "app_dap.h"
#include "app_usbdisp.h"
#include "drv_uart.h"          /* DRV_UART_BAUD_RATE */
#include "io_picker.h"         /* 惰性占用账本：enable 占用/disable 归还 */
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "class/cdc/cdc_device.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "hal/usb_serial_jtag_ll.h"
#endif
#include <string.h>

static const char *TAG = "app_usb2ttl";

#define USB2TTL_CDC_ITF   TINYUSB_CDC_ACM_0   /* esp_tinyusb 接口号 */
#define USB2TTL_PORT      UART_NUM_1          /* 桥接 UART（IO2/IO4） */
#define USB2TTL_TASK_WAIT_MS  50              /* 停任务最大等待 */

/* ── 状态 ── */
static usb2ttl_state_t s_state = USB2TTL_OFF;
static volatile bool s_pc_open;        /* PC 已打开串口（DTR+RTS） */
static volatile bool s_run;            /* 桥接任务运行标志 */
static TaskHandle_t s_task;            /* 桥接任务句柄（NULL=未运行） */
static bool s_uart1_self_installed;    /* 本次 enable 自己装载了 UART1 驱动（disable 时归还） */

/* ── 自动下载（DTR/RTS 直通映射，默认关） ── */
static volatile bool s_auto_isp;           /* PC 经 SetCommState(DTR/RTS) 触发 ISP */
static volatile bool s_auto_isp_gpio_on;   /* auto GPIO 当前已驱动（懒配置） */

/* ── 串口参数（仅 OFF 可改，enable 时应用） ── */
static int s_baud = 115200;
static bool s_even_parity;         /* false=8N1，true=8E1 */

/* ── ISP 引脚（enter_isp 时驱动，运行期可改） ── */
static int s_isp_boot0 = USB2TTL_BOOT0_DEF;
static int s_isp_rst   = USB2TTL_RST_DEF;

/* 可用 ISP 引脚白名单：避开 LCD/触摸/SD/UART/USB 等硬占用脚。
 * IO6-15/IO21 为摄像头接口（未使用），IO37/IO44 空闲；其中 IO9/IO10 是
 * 示波器 ADC 输入、IO6-8/18/37/44 是波形输出可选脚——引脚仅在按下 ISP
 * 时短暂驱动，若与其他 APP 同时接线注意绕开冲突脚 */
static const int s_isp_pin_choices[] = {
    5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 18, 21, 37, 44,
};
#define ISP_PIN_CHOICES_N  (sizeof(s_isp_pin_choices) / sizeof(s_isp_pin_choices[0]))

static bool isp_pin_valid(int pin)
{
    for (int i = 0; i < (int)ISP_PIN_CHOICES_N; i++) {
        if (s_isp_pin_choices[i] == pin) return true;
    }
    return false;
}

/* ── USB 描述符（自定义，与 DAP 同模式） ── */

/* 序列号：芯片 MAC（enable 时填充） */
static char s_serial_str[13];

static const char *s_string_desc[] = {
    (char[]){0x09, 0x04},         /* 0: English (0x0409) */
    "Embedded Debug Tool",        /* 1: Manufacturer */
    "USB2TTL Bridge",            /* 2: Product（PC 设备管理器显示） */
    s_serial_str,                 /* 3: Serial（MAC） */
    "USB2TTL (UART1 IO2/IO4)",   /* 4: 接口字符串（CDC 数据接口） */
};

/* 单 CDC-ACM：控制接口(0) + 数据接口(1)；端点 notify=0x81(IN)，
 * bulk OUT=0x02（PC→设备），bulk IN=0x82（设备→PC），全速 64B */
#define USB2TTL_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, USB2TTL_DESC_TOTAL_LEN,
                          0x80 | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,          /* 单 CDC 用 IAD 复合类 */
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,           /* Espressif */
    .idProduct = 0x4003,          /* USB2TTL Bridge */
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/* ── ISP GPIO 复用（手动序列与自动下载共用） ──
 * BOOT0：推挽输出（低=下载模式，高=运行）
 * RST：开漏 + 上拉（低=复位，高=释放） */
static void usb2ttl_isp_gpio_output(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_isp_boot0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    io.pin_bit_mask = 1ULL << s_isp_rst;
    io.mode = GPIO_MODE_OUTPUT_OD;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);
}

static void usb2ttl_isp_gpio_release(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_isp_boot0) | (1ULL << s_isp_rst),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* 自动下载：把 CDC DTR/RTS 直通映射到 BOOT0/RST GPIO。
 * 极性对齐 esptool 的 DTR/RTS 复位约定（PC 工具按自身时序翻转控制线）：
 *   DTR=1 → BOOT0 高（运行模式）；DTR=0 → BOOT0 低（下载模式）
 *   RTS=1 → RST 低（复位）；RTS=0 → RST 高（释放）
 * 可运行在 TinyUSB 任务（line-state 回调）或 LVGL 线程（勾选瞬间） */
static void usb2ttl_auto_isp_apply(bool dtr, bool rts)
{
    if (!s_auto_isp_gpio_on) {
        usb2ttl_isp_gpio_output();
        s_auto_isp_gpio_on = true;
    }
    gpio_set_level(s_isp_boot0, dtr ? 1 : 0);
    gpio_set_level(s_isp_rst, rts ? 0 : 1);   /* RTS=1 → 拉低复位 */
    ESP_LOGD(TAG, "auto ISP: DTR=%d RTS=%d -> BOOT0=%d RST=%s",
             dtr ? 1 : 0, rts ? 1 : 0, dtr ? 1 : 0, rts ? "LOW" : "HIGH");
}

/* ── TinyUSB 事件（TinyUSB 任务上下文，仅日志） ── */

static void usb2ttl_tusb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "PC attached: CDC enumerated");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "PC detached (USB unplugged)");
        s_pc_open = false;
        break;
    default:
        break;
    }
}

/* CDC 线状态回调（TinyUSB 任务）：PC 打开/关闭串口。仅更新标志；
 * 自动下载开启时把 DTR/RTS 直通映射到 BOOT0/RST GPIO */
static void usb2ttl_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    s_pc_open = dtr && rts;
    ESP_LOGI(TAG, "PC serial port %s (DTR=%d RTS=%d)",
             s_pc_open ? "opened" : "closed", dtr ? 1 : 0, rts ? 1 : 0);
    if (s_auto_isp) {
        usb2ttl_auto_isp_apply(dtr, rts);
    }
}

/* ── 桥接任务：双向转发 ── */

static void usb2ttl_bridge_task(void *arg)
{
    (void)arg;
    uint8_t uart_buf[1024];
    uint8_t cdc_buf[512];

    ESP_LOGI(TAG, "bridge task started");
    while (s_run) {
        /* PC → 目标：CDC RX FIFO → UART1 TX */
        size_t n = 0;
        if (tinyusb_cdcacm_read(USB2TTL_CDC_ITF, cdc_buf, sizeof(cdc_buf), &n) == ESP_OK
            && n > 0) {
            uart_write_bytes(USB2TTL_PORT, cdc_buf, n);
        }

        /* 目标 → PC：UART1 RX → CDC TX FIFO（PC 未打开端口时丢弃） */
        int n2 = drv_uart_read(USB2TTL_PORT, uart_buf, sizeof(uart_buf), 5);
        if (n2 > 0 && s_pc_open) {
            size_t off = 0;
            while (off < (size_t)n2 && s_run) {
                size_t w = tinyusb_cdcacm_write_queue(USB2TTL_CDC_ITF,
                                                      uart_buf + off, n2 - off);
                off += w;
                tinyusb_cdcacm_write_flush(USB2TTL_CDC_ITF, 0);   /* 非阻塞 */
                if (w == 0) vTaskDelay(1);   /* FIFO 满：等 TinyUSB 任务排空 */
            }
        }
    }
    ESP_LOGI(TAG, "bridge task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

static bool usb2ttl_task_start(void)
{
    s_run = true;
    s_task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(usb2ttl_bridge_task, "usb2ttl", 4096,
                                        NULL, 5, &s_task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "bridge task create failed");
        s_run = false;
        return false;
    }
    return true;
}

static void usb2ttl_task_stop(void)
{
    if (!s_task) return;
    s_run = false;
    for (int i = 0; i < 20 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(USB2TTL_TASK_WAIT_MS));
    }
    if (s_task) {
        ESP_LOGW(TAG, "bridge task did not stop, force delete");
        vTaskDelete(s_task);
        s_task = NULL;
    }
}

/* ── UART1 参数应用/恢复 ── */

static void usb2ttl_apply_uart_cfg(int baud, bool even_parity)
{
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = even_parity ? UART_PARITY_EVEN : UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(USB2TTL_PORT, &cfg));
    ESP_LOGI(TAG, "UART1 %d baud, %s",
             baud, even_parity ? "8E1" : "8N1");
}

/* ── 开关 ── */

esp_err_t app_usb2ttl_enable(void)
{
    if (s_state == USB2TTL_ON) return ESP_OK;

    /* USB PHY 互斥：读卡器/DAP 占用时拒绝 */
    cardreader_state_t cr = app_cardreader_get_state();
    if (cr == CARDREADER_EXPOSED || cr == CARDREADER_APP_OWNED) {
        ESP_LOGE(TAG, "card reader owns USB (%s), disable it first",
                 app_cardreader_state_str(cr));
        return ESP_ERR_INVALID_STATE;
    }
    if (app_dap_get_state() == DAP_STATE_READY) {
        ESP_LOGE(TAG, "DAP owns USB, disable it first");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_usbdisp_get_state() == USDISP_ACTIVE) {
        ESP_LOGE(TAG, "USB display owns USB, disable it first");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;

    /* UART1 独占：暂停 TCP/网页/终端转发（fwd 任务读到 paused 即停读） */
    if (g_bridges[0]) g_bridges[0]->paused = 1;

    /* UART 惰性占用：终端未开启（bridge 非 active）时要自己装载 UART1 驱动，
     * 并同步 active/账本（让终端/其它 APP 看到占用）；若终端已占用（active）
     * 则直接复用其驱动。drv_uart_init 内部幂等 */
    s_uart1_self_installed = false;
    if (g_bridges[0] && !g_bridges[0]->active) {
        drv_uart_init(USB2TTL_PORT, g_bridges[0]->tx_pin, g_bridges[0]->rx_pin);
        g_bridges[0]->active = 1;
        io_picker_reserve(g_bridges[0]->tx_pin);
        io_picker_reserve(g_bridges[0]->rx_pin);
        s_uart1_self_installed = true;
    }

    /* 应用波特率/校验 */
    usb2ttl_apply_uart_cfg(s_baud, s_even_parity);

    /* 序列号 = 芯片 MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial_str, sizeof(s_serial_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 安装 TinyUSB CDC 设备：接管 USB PHY → USJ 控制台静默 */
    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_FULL_SPEED(usb2ttl_tusb_event_cb, NULL);
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_config_desc;
    tusb_cfg.descriptor.string = (const char **)s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb driver install failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = USB2TTL_CDC_ITF,
        .callback_rx = NULL,              /* 桥接任务轮询读取，无需回调 */
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = usb2ttl_line_state_cb,
        .callback_line_coding_changed = NULL,
    };
    ret = tinyusb_cdcacm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "cdcacm init failed: %s", esp_err_to_name(ret));
        tinyusb_driver_uninstall();
        goto fail;
    }

    s_pc_open = false;
    if (!usb2ttl_task_start()) {
        tinyusb_cdcacm_deinit(USB2TTL_CDC_ITF);
        tinyusb_driver_uninstall();
        goto fail;
    }

    s_state = USB2TTL_ON;
    ESP_LOGI(TAG, "USB2TTL enabled: baud=%d %s, UART1 fwd paused",
             s_baud, s_even_parity ? "8E1" : "8N1");
    return ESP_OK;

fail:
    if (g_bridges[0]) g_bridges[0]->paused = 0;
    if (s_uart1_self_installed) {   /* enable 失败：归还自装的 UART1 驱动/账本 */
        if (g_bridges[0]) {
            g_bridges[0]->active = 0;
            drv_uart_deinit(USB2TTL_PORT, g_bridges[0]->tx_pin, g_bridges[0]->rx_pin);
            io_picker_release(g_bridges[0]->tx_pin);
            io_picker_release(g_bridges[0]->rx_pin);
        }
        s_uart1_self_installed = false;
    }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    /* 恢复 USJ 控制台（driver 已装成功过，PHY 需要重新映射回 USJ） */
    usb_serial_jtag_ll_phy_enable_external(false);
#endif
    s_state = USB2TTL_ERROR;
    return ret;
}

esp_err_t app_usb2ttl_disable(void)
{
    if (s_state == USB2TTL_OFF) return ESP_OK;

    ESP_LOGI(TAG, "disabling USB2TTL (%s)", app_usb2ttl_state_str(s_state));
    esp_err_t ret = ESP_OK;
    esp_err_t r;

    /* 1. 停桥接任务 */
    usb2ttl_task_stop();

    /* 2. 卸载 CDC-ACM + TinyUSB：USB 设备消失（ERROR 态可能未 init） */
    if (tinyusb_cdcacm_initialized(USB2TTL_CDC_ITF)) {
        r = tinyusb_cdcacm_deinit(USB2TTL_CDC_ITF);
        if (r != ESP_OK) ESP_LOGE(TAG, "cdcacm deinit failed: %s", esp_err_to_name(r));
    }

    r = tinyusb_driver_uninstall();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb uninstall failed: %s", esp_err_to_name(r));
        if (ret == ESP_OK) ret = r;
    }

    /* 3. 内部 USB PHY 路由交还 USB-Serial/JTAG（恢复 COM 控制台枚举） */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_ll_phy_enable_external(false);
#endif

    /* 3.5 自动下载驱动的 ISP GPIO 交还输入（目标解除 BOOT0/RST 控制） */
    if (s_auto_isp_gpio_on) {
        usb2ttl_isp_gpio_release();
        s_auto_isp_gpio_on = false;
    }

    /* 4. 解除暂停（交还 TCP/终端转发）。
     * 若 UART1 驱动为自己 enable 时装载（终端未占用）→ 卸载释放 IO2/4 +
     * 清 active/账本；若终端占用中 → 只恢复参数+解除暂停，驱动/引脚归终端 */
    if (g_bridges[0]) g_bridges[0]->paused = 0;
    if (s_uart1_self_installed) {
        if (g_bridges[0]) {
            g_bridges[0]->active = 0;   /* 先清占用，再释放驱动/账本 */
            drv_uart_deinit(USB2TTL_PORT, g_bridges[0]->tx_pin, g_bridges[0]->rx_pin);
            io_picker_release(g_bridges[0]->tx_pin);
            io_picker_release(g_bridges[0]->rx_pin);
        }
        s_uart1_self_installed = false;
    } else {
        usb2ttl_apply_uart_cfg(DRV_UART_BAUD_RATE, false);   /* 终端占用：只还原参数 */
    }

    s_state = USB2TTL_OFF;
    ESP_LOGI(TAG, "USB2TTL disabled, UART1 restored to TCP/terminal");
    return ret;
}

/* ── 状态查询 ── */

usb2ttl_state_t app_usb2ttl_get_state(void)
{
    return s_state;
}

const char *app_usb2ttl_state_str(usb2ttl_state_t st)
{
    switch (st) {
    case USB2TTL_OFF:    return "off";
    case USB2TTL_ON:     return "on";
    case USB2TTL_ERROR:  return "error";
    default:              return "?";
    }
}

bool app_usb2ttl_pc_open(void)
{
    return s_pc_open;
}

/* ── ISP 引脚 ── */

esp_err_t app_usb2ttl_set_isp_pins(int boot0, int rst)
{
    /* 引脚仅在 enter_isp 时被驱动（LVGL 线程），运行期可改，无竞态 */
    if (!isp_pin_valid(boot0) || !isp_pin_valid(rst) || boot0 == rst) {
        ESP_LOGE(TAG, "invalid ISP pins: BOOT0=%d RST=%d", boot0, rst);
        return ESP_ERR_INVALID_ARG;
    }
    s_isp_boot0 = boot0;
    s_isp_rst = rst;
    ESP_LOGI(TAG, "ISP pins: BOOT0=IO%d RST=IO%d", boot0, rst);
    return ESP_OK;
}

void app_usb2ttl_get_isp_pins(int *boot0, int *rst)
{
    if (boot0) *boot0 = s_isp_boot0;
    if (rst) *rst = s_isp_rst;
}

esp_err_t app_usb2ttl_enter_isp(void)
{
    if (s_isp_boot0 == s_isp_rst) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "ISP: BOOT0=IO%d -> 1, RST=IO%d low pulse",
             s_isp_boot0, s_isp_rst);

    usb2ttl_isp_gpio_output();
    gpio_set_level(s_isp_boot0, 0);   /* 先低（防复位瞬间误采样） */
    gpio_set_level(s_isp_rst, 1);     /* 复位线释放（OD 截止 + 上拉） */

    /* 序列：BOOT0=1 → 稳定 → RST 拉低 → 保持 → 释放（BOOT0 采样）→
     * 等 bootloader → BOOT0=0 */
    gpio_set_level(s_isp_boot0, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_isp_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(s_isp_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(s_isp_boot0, 0);

    /* 交还引脚为输入（高阻）：目标可正常启动/后续复位 */
    usb2ttl_isp_gpio_release();

    ESP_LOGI(TAG, "ISP sequence done, target should be in bootloader");
    return ESP_OK;
}

/* ── 自动下载（DTR/RTS 直通触发 ISP） ── */

void app_usb2ttl_set_auto_isp(bool en)
{
    s_auto_isp = en;
    if (en) {
        /* 勾选瞬间 PC 端口可能已打开（无新 line-state 回调）：按当前
         * DTR/RTS 立即应用。桥接未开启（tinyusb 未装）时不驱动 GPIO，
         * 等开启后 PC 连接自然触发 line-state 回调 */
        if (s_state == USB2TTL_ON) {
            uint8_t ls = tud_cdc_n_get_line_state(USB2TTL_CDC_ITF);
            usb2ttl_auto_isp_apply((ls & 0x01) != 0, (ls & 0x02) != 0);
        }
    } else {
        if (s_auto_isp_gpio_on) {
            usb2ttl_isp_gpio_release();
            s_auto_isp_gpio_on = false;
        }
    }
    ESP_LOGI(TAG, "auto ISP %s", en ? "enabled" : "disabled");
}

bool app_usb2ttl_get_auto_isp(void)
{
    return s_auto_isp;
}

/* ── 串口参数 ── */

esp_err_t app_usb2ttl_set_baud(int baud)
{
    if (s_state != USB2TTL_OFF) {
        ESP_LOGE(TAG, "baud locked while bridge active");
        return ESP_ERR_INVALID_STATE;
    }
    if (baud != 115200 && baud != 57600 && baud != 9600) {
        ESP_LOGE(TAG, "unsupported baud %d", baud);
        return ESP_ERR_INVALID_ARG;
    }
    s_baud = baud;
    ESP_LOGI(TAG, "baud set to %d", baud);
    return ESP_OK;
}

int app_usb2ttl_get_baud(void)
{
    return s_baud;
}

esp_err_t app_usb2ttl_set_parity_even(bool even)
{
    if (s_state != USB2TTL_OFF) {
        ESP_LOGE(TAG, "parity locked while bridge active");
        return ESP_ERR_INVALID_STATE;
    }
    s_even_parity = even;
    ESP_LOGI(TAG, "parity set to %s", even ? "8E1" : "8N1");
    return ESP_OK;
}

bool app_usb2ttl_get_parity_even(void)
{
    return s_even_parity;
}
