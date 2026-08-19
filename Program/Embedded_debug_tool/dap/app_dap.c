#include "app_dap.h"
#include "drv_dap.h"
#include "app_cardreader.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "soc/soc_caps.h"
#include "hal/usb_serial_jtag_ll.h"
#include <string.h>

static const char *TAG = "app_dap";

/* ── 状态 ── */
static dap_state_t s_state = DAP_STATE_OFF;

/* 每端口 DAP 命令/响应缓冲（CMSIS-DAP v1 包 = 64B） */
static uint8_t s_cmd[DAP_PORT_COUNT][64];
static uint8_t s_rsp[DAP_PORT_COUNT][64];

/* ── USB 描述符（CMSIS-DAP v1：每 SWD 口一个 HID 接口，OUT 走 SET_REPORT） ── */

/* HID 报表：vendor-defined 64B 输入/输出，**无 Report ID**（CMSIS-DAP v1 标准，
 * Keil/pyOCD 按 64B 无 ID 协议通信；带 ID 会导致命令错位） */
static const uint8_t s_hid_report_desc[] = {
    0x06, 0x00, 0xFF,   /* Usage Page (Vendor Defined) */
    0x09, 0x01,         /* Usage (0x01) */
    0xA1, 0x01,         /* Collection (Application) */
    0x15, 0x00,         /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,   /*   Logical Maximum (255) */
    0x75, 0x08,         /*   Report Size (8) */
    0x95, 0x40,         /*   Report Count (64) */
    0x09, 0x01,         /*   Usage (0x01) */
    0x81, 0x02,         /*   Input (Data,Var,Abs) */
    0x09, 0x01,         /*   Usage (0x01) */
    0x91, 0x02,         /*   Output (Data,Var,Abs) */
    0xC0,               /* End Collection */
};

/* 配置描述符：1 个配置 + N 个 HID 接口（每 SWD 口一个，EP IN 0x81 起，64B 1ms） */
#define DAP_USB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + DAP_PORT_COUNT * TUD_HID_DESC_LEN)
static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, DAP_PORT_COUNT, 0, DAP_USB_DESC_TOTAL_LEN,
                          0x80 | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
#if DAP_PORT_COUNT >= 1
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_desc), 0x81, 64, 1),
#endif
#if DAP_PORT_COUNT >= 2
    TUD_HID_DESCRIPTOR(1, 5, false, sizeof(s_hid_report_desc), 0x82, 64, 1),
#endif
};

/* 序列号：芯片 MAC（设备级唯一；同一设备的两个 HID 接口共享，接口靠
 * iInterface 字符串 "SWD1"/"SWD2" 区分） */
static char s_serial_str[13];

static const char *s_string_desc[] = {
    (char[]){0x09, 0x04},         /* 0: English (0x0409) */
    "Embedded Debug Tool",        /* 1: Manufacturer */
    "CMSIS-DAP",                  /* 2: Product（PC 设备管理器显示） */
    s_serial_str,                 /* 3: Serial（MAC，enable 时填充） */
    "CMSIS-DAP SWD1 (GPIO11/12/13)",  /* 4: 接口 0 字符串（HID 集合 MI_00；
                                          保留 CMSIS-DAP 供 pyOCD/Keil 识别过滤） */
    "CMSIS-DAP SWD2 (GPIO14/15/18)",  /* 5: 接口 1 字符串（HID 集合 MI_01） */
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,           /* Espressif */
    .idProduct = 0x4008,          /* CMSIS-DAP */
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/* ── TinyUSB HID 回调（CMSIS-DAP 协议：一问一答） ── */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id; (void)report_type;

    if (instance >= DAP_PORT_COUNT) {
        ESP_LOGW(TAG, "invalid HID instance %u", instance);
        return;
    }
    if (bufsize == 0 || bufsize > sizeof(s_cmd[0])) {
        ESP_LOGW(TAG, "invalid DAP packet size %u", bufsize);
        return;
    }
    memcpy(s_cmd[instance], buffer, bufsize);

    /* 同步执行（几 ms）。返回 num：低 16 位=响应长度，高 16 位=请求长度 */
    uint32_t num = dap_ports[instance]->execute(s_cmd[instance], s_rsp[instance]);
    uint16_t rsp_len = (uint16_t)(num & 0xFFFFU);
    if (rsp_len > 0) {
        /* 响应填充到整包 64B：DWC2 DMA 模式要求传输长度 4 字节对齐
         * （2/3 字节短响应会挂起 IN 传输），且 Windows HID 按 64B 报表读取 */
        memset(s_rsp[instance] + rsp_len, 0, sizeof(s_rsp[0]) - rsp_len);
        /* 注意：用 tud_hid_n_report（多接口版）——tud_hid_report 是单接口
         * 宏（硬编码 instance=0，会把 instance 当 report_id 导致发错接口） */
        tud_hid_n_report(instance, 0, s_rsp[instance], sizeof(s_rsp[0]));
    } else {
        ESP_LOGW(TAG, "%s: cmd=0x%02X len=%u -> no response",
                 dap_ports[instance]->name, s_cmd[instance][0], bufsize);
    }
}

/* ── USB 事件：PC 枚举完成 / 断开（仅日志） ── */

static void tusb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "PC attached: CMSIS-DAP enumerated");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "PC detached (USB unplugged)");
        break;
    default:
        break;
    }
}

/* ── 开关 ── */

static void set_state(dap_state_t st)
{
    s_state = st;
    ESP_LOGI(TAG, "state -> %s", app_dap_state_str(st));
}

esp_err_t app_dap_enable(void)
{
    if (s_state != DAP_STATE_OFF) {
        ESP_LOGW(TAG, "enable ignored: already %s", app_dap_state_str(s_state));
        return ESP_OK;
    }

    /* 与 USB 读卡器互斥（共用 USB PHY/描述符） */
    if (app_cardreader_get_state() != CARDREADER_IDLE &&
        app_cardreader_get_state() != CARDREADER_ERROR) {
        ESP_LOGE(TAG, "card reader is using USB (%s), disable it first",
                 app_cardreader_state_str(app_cardreader_get_state()));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    /* 序列号 = 芯片 MAC（后 6 字节，大写十六进制） */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial_str, sizeof(s_serial_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "USB serial: %s", s_serial_str);

    /* 1. 初始化全部 SWD 端口（引脚 + 内核） */
    for (int i = 0; i < DAP_PORT_COUNT; i++) {
        dap_ports[i]->init();
        ESP_LOGI(TAG, "port %d ready: %s", i, dap_ports[i]->name);
    }

    /* 2. 安装 TinyUSB HID 设备：接管 USB PHY → USJ 控制台静默 */
    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_FULL_SPEED(tusb_event_cb, NULL);
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_config_desc;
    tusb_cfg.descriptor.string = (const char **)s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb driver install failed: %s", esp_err_to_name(ret));
        set_state(DAP_STATE_ERROR);
        return ret;
    }

    set_state(DAP_STATE_READY);
    ESP_LOGI(TAG, "DAP enabled: %d CMSIS-DAP interface(s)", DAP_PORT_COUNT);
    return ESP_OK;
}

esp_err_t app_dap_disable(void)
{
    if (s_state == DAP_STATE_OFF) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "disabling DAP (%s)", app_dap_state_str(s_state));

    /* 1. 卸载 TinyUSB：USB 设备消失 */
    esp_err_t ret = tinyusb_driver_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb uninstall failed: %s", esp_err_to_name(ret));
    }

    /* 2. 把内部 USB PHY 路由交还 USB-Serial/JTAG（恢复 COM 控制台枚举） */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_ll_phy_enable_external(false);
#endif

    set_state(DAP_STATE_OFF);
    ESP_LOGI(TAG, "DAP disabled, USJ console restored");
    return ret;
}

const char *app_dap_state_str(dap_state_t state)
{
    switch (state) {
    case DAP_STATE_OFF:    return "OFF";
    case DAP_STATE_READY:  return "READY";
    case DAP_STATE_ERROR:  return "ERROR";
    default:               return "?";
    }
}

dap_state_t app_dap_get_state(void)
{
    return s_state;
}
