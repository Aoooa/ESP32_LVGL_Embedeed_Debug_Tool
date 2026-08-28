/* app_usbdisp.c —— USB 电脑副屏服务实现
 *
 * 数据流：PC IDD 驱动 → USB Vendor bulk OUT EP → tud_vendor_n_read →
 *        协议帧解析 → 帧 ready 标志 → decode_task (esp_new_jpeg) →
 *        共享 RGB565 缓冲 → lv_timer (LVGL 线程) → lv_image_set_src →
 *        LVGL flush → esp_lcd_panel_draw_bitmap → ST7789
 */
#include "app_usbdisp.h"
#include "drv_display.h"
#include "app_cardreader.h"
#include "app_dap.h"
#include "app_usb2ttl.h"
#include "esp_lv_adapter.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "esp_jpeg_dec.h"

#include <string.h>
#include <stdatomic.h>

static const char *TAG = "app_usbdisp";

#define USDISP_VID          0x303A
#define USDISP_PID          0x4010
/* xfz1986 协议要求的 Product 字符串格式：
 *  esp32udisp0_R<W>x<H>_Ejpeg<Q>_Fps<F>_Bl<B>
 * driver 用 _ 分词, sscanf 解析每个 token, 缺 Fps 或顺序错会导致 monitor 创建失败 */
#define USDISP_PRODUCT      "ESP32_LCD_Display"
#define USDISP_MANUFACTURER "Espressif"
/* Vendor Interface 字符串（与 product 同内容，driver 优先读 interface string）
 * 必须分配独立字符串索引，driver 用 GetDescriptor(String) 读取 */
#define USDISP_VENDOR_STR_IDX  4

#define USDISP_TYPE_JPG     3

typedef struct __attribute__((packed)) {
    uint16_t crc16;
    uint8_t  type;
    uint8_t  cmd;
    uint16_t x_lo;
    uint16_t y_hi;
    uint16_t w_lo;
    uint16_t h_hi;
    uint32_t id10_len22;
} usdisp_frame_header_t;
#define USDISP_HDR_SIZE 16

#define USDISP_EP_OUT   0x01
#define USDISP_EP_IN    0x81
#define USDISP_BUF_SIZE 64
#define USDISP_FRAME_MAX_SIZE   (300 * 1024)

static atomic_int s_state = 0;
static atomic_uint s_frame_count = 0;
static atomic_uint s_error_count = 0;
static atomic_int s_fps_x10 = 0;
static int64_t s_fps_window_start_us = 0;
static volatile bool s_pc_connected = false;
static bool s_usb_installed = false;

static uint8_t  *s_frame_buf = NULL;
static uint16_t *s_rgb_buf[2] = {NULL, NULL};
static volatile int s_rgb_front = 0;
static volatile int s_rgb_back_new = -1;
static volatile uint32_t s_rgb_back_w = 0, s_rgb_back_h = 0;

static volatile uint32_t s_frame_len = 0;
static volatile bool s_frame_ready = false;

typedef void (*frame_cb_t)(void *ctx, const uint16_t *rgb, uint32_t w, uint32_t h);
static frame_cb_t s_frame_cb = NULL;
static void *s_frame_cb_ctx = NULL;

void app_usbdisp_register_frame_cb(frame_cb_t cb, void *ctx) {
    s_frame_cb = cb;
    s_frame_cb_ctx = ctx;
}

const char *app_usbdisp_state_str(usdisp_state_t st) {
    switch (st) {
    case USDISP_OFF:    return "off";
    case USDISP_ACTIVE: return "active";
    case USDISP_ERROR:  return "error";
    } return "?";
}
usdisp_state_t app_usbdisp_get_state(void) { return (usdisp_state_t)atomic_load(&s_state); }
uint32_t app_usbdisp_get_frame_count(void) { return atomic_load(&s_frame_count); }
uint32_t app_usbdisp_get_error_count(void) { return atomic_load(&s_error_count); }
float app_usbdisp_get_fps(void) { return atomic_load(&s_fps_x10) / 10.0f; }
bool app_usbdisp_pc_connected(void) { return s_pc_connected; }

static lv_timer_t *s_poll_timer = NULL;
static void lv_poll_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_frame_cb) return;
    int back = s_rgb_back_new;
    if (back < 0) return;
    s_rgb_front = back;
    s_rgb_back_new = -1;
    s_frame_cb(s_frame_cb_ctx, s_rgb_buf[s_rgb_front], s_rgb_back_w, s_rgb_back_h);
}

static bool decode_one_frame(const uint8_t *jpg, size_t len) {
    if (len < 10 || len > USDISP_FRAME_MAX_SIZE) {
        atomic_fetch_add(&s_error_count, 1);
        return false;
    }
    int back = s_rgb_front ^ 1;

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.rotate = JPEG_ROTATE_0D;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK || !dec) {
        atomic_fetch_add(&s_error_count, 1);
        return false;
    }
    jpeg_dec_io_t *io = heap_caps_calloc(1, sizeof(*io), MALLOC_CAP_INTERNAL);
    jpeg_dec_header_info_t *hi = heap_caps_calloc(1, sizeof(*hi), MALLOC_CAP_INTERNAL);
    if (!io || !hi) {
        free(io); free(hi);
        jpeg_dec_close(dec);
        return false;
    }
    io->inbuf = (unsigned char *)jpg;
    io->inbuf_len = (int)len;
    if (jpeg_dec_parse_header(dec, io, hi) < 0) {
        free(io); free(hi);
        jpeg_dec_close(dec);
        atomic_fetch_add(&s_error_count, 1);
        return false;
    }
    io->outbuf = (unsigned char *)s_rgb_buf[back];
    int proc_ret = jpeg_dec_process(dec, io);
    jpeg_dec_close(dec);
    uint16_t w = hi->width, h = hi->height;
    free(io); free(hi);
    if (proc_ret < 0) {
        atomic_fetch_add(&s_error_count, 1);
        return false;
    }

    s_rgb_back_w = w;
    s_rgb_back_h = h;
    s_rgb_back_new = back;

    int64_t now = esp_timer_get_time();
    uint32_t fc = atomic_fetch_add(&s_frame_count, 1) + 1;
    if (s_fps_window_start_us == 0) s_fps_window_start_us = now;
    int64_t dt = now - s_fps_window_start_us;
    if (dt >= 1000000) {
        int fps_x10 = (int)((int64_t)fc * 10000000 / dt);
        atomic_store(&s_fps_x10, fps_x10);
        s_fps_window_start_us = now;
        atomic_store(&s_frame_count, 0);
    }
    return true;
}

static void decode_task(void *arg) {
    (void)arg;
    s_poll_timer = lv_timer_create(lv_poll_timer_cb, 16, NULL);
    while (1) {
        while (!s_frame_ready) vTaskDelay(pdMS_TO_TICKS(2));
        uint32_t len = s_frame_len;
        s_frame_ready = false;
        if (len > 0) decode_one_frame(s_frame_buf, len);
    }
}

static struct {
    bool in_frame;
    uint32_t received;
    uint32_t payload_total;
    uint8_t hdr_buf[USDISP_HDR_SIZE];
} s_proto = {0};

/* 处理收到的连续 vendor bulk 数据（USB 任务上下文） */
static void vendor_process_data(const uint8_t *buf, int n) {
    int remain = n, cur = 0;
    while (remain > 0) {
        if (!s_proto.in_frame) {
            int need = USDISP_HDR_SIZE - s_proto.received;
            int take = (remain < need) ? remain : need;
            memcpy(s_proto.hdr_buf + s_proto.received, &buf[cur], take);
            s_proto.received += take;
            cur += take; remain -= take;
            if (s_proto.received == USDISP_HDR_SIZE) {
                usdisp_frame_header_t hdr;
                memcpy(&hdr, s_proto.hdr_buf, sizeof(hdr));
                if (hdr.type != USDISP_TYPE_JPG) {
                    ESP_LOGW(TAG, "unsupported type %u", hdr.type);
                    atomic_fetch_add(&s_error_count, 1);
                    s_proto.received = 0;
                    continue;
                }
                uint32_t total = hdr.id10_len22 & 0x3FFFFF;
                if (total == 0 || total > USDISP_FRAME_MAX_SIZE) {
                    ESP_LOGW(TAG, "bad total %u", (unsigned)total);
                    atomic_fetch_add(&s_error_count, 1);
                    s_proto.received = 0;
                    continue;
                }
                s_proto.payload_total = total;
                s_proto.in_frame = true;
                s_proto.received = 0;
            }
        } else {
            uint32_t want = s_proto.payload_total - s_proto.received;
            int take = (remain < (int)want) ? remain : (int)want;
            if (s_proto.received + take <= USDISP_FRAME_MAX_SIZE) {
                memcpy(s_frame_buf + s_proto.received, &buf[cur], take);
            }
            s_proto.received += take;
            cur += take; remain -= take;
            if (s_proto.received >= s_proto.payload_total) {
                s_frame_len = s_proto.received;
                s_frame_ready = true;
                s_proto.in_frame = false;
                s_proto.received = 0;
            }
        }
    }
}

/* Weak 回调：TinyUSB vendor 数据到达时调用（在 TinyUSB 任务上下文） */
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize) {
    if (idx == 0 && bufsize > 0 && buffer) {
        vendor_process_data(buffer, (int)bufsize);
    }
}

/* 描述符：device + config + strings，static 提供给 tinyusb_config_t */
static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* class=0 表示 class 由接口描述符决定（vendor 接口）
     * 这样 Windows 不会按设备类走 PnP 错误路径 */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USDISP_VID,
    .idProduct = USDISP_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 1,
};

static uint8_t const s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* Vendor 接口：第二个参数是 interface string index（4 = USDISP_VENDOR_STR_IDX）
     * driver 通过 GetDescriptor(String, 4) 读取含分辨率的 vendor 字符串 */
    TUD_VENDOR_DESCRIPTOR(0, USDISP_VENDOR_STR_IDX, USDISP_EP_OUT, USDISP_EP_IN, USDISP_BUF_SIZE),
};

static const char *s_usb_strings[5] = {
    "",                     /* 0: langid 由 esp_tinyusb 内部处理 */
    USDISP_MANUFACTURER,    /* 1: iManufacturer */
    USDISP_PRODUCT,         /* 2: iProduct */
    "0001",                 /* 3: iSerialNumber */
    USDISP_PRODUCT,         /* 4: iInterface (vendor) - driver 用此字符串解析分辨率 */
};

static const tinyusb_desc_config_t s_desc_cfg = {
    .device = &s_dev_desc,
    .string = s_usb_strings,
    .string_count = 4,
    .high_speed_config = NULL,
    .full_speed_config = s_cfg_desc,
};


/* ── USB 事件 ── */
static void tusb_event_cb(tinyusb_event_t *e, void *arg) {
    (void)arg;
    if (e->id == TINYUSB_EVENT_ATTACHED) {
        s_pc_connected = true;
        ESP_LOGI(TAG, "USB attached");
    } else if (e->id == TINYUSB_EVENT_DETACHED) {
        s_pc_connected = false;
        ESP_LOGW(TAG, "USB detached");
    }
}

/* ── enable / disable ── */
/* Microsoft OS 1.0 Descriptor (Extended Compat ID)
 * Windows 主机发起 vendor control request (bRequest=0x01 GET_DESCRIPTOR, wValue=0x03EE)
 * 我们用 tud_control_xfer 返回此 buffer.
 * 声明 interface 0 是 WinUSB 兼容, Windows 自动加载内置 winusb.sys
 * 不需装任何驱动, 不受 Secure Boot / test signing 限制
 */
static const uint8_t s_ms_os_desc[] = {
    /* Header (18 bytes) */
    0x12, 0x00,
    0x00, 0x01,
    0xEE, 0x03,
    0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Extended Compat ID Descriptor (20 bytes) */
    0x14, 0x00,
    0x04,
    0x00,
    0x01,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Override TinyUSB weak default tud_vendor_control_xfer_cb
 * 处理 Microsoft OS 1.0 GET_DESCRIPTOR 请求 (vendor request)
 * 这个 callback 是 WEAK 符号, 不会被 esp_tinyusb 强定义冲突
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                 const tusb_control_request_t *request) {
    if (stage == CONTROL_STAGE_SETUP &&
        request->bRequest == 0x01 &&          /* GET_DESCRIPTOR */
        request->wValue == 0x03EE) {          /* MS OS 1.0 vendor descriptor */
        return tud_control_xfer(rhport, request,
                                (void *)s_ms_os_desc, sizeof(s_ms_os_desc));
    }
    return false;
}

esp_err_t app_usbdisp_enable(void) {
    if ((usdisp_state_t)atomic_load(&s_state) == USDISP_ACTIVE) return ESP_OK;

    if (app_cardreader_get_state() == CARDREADER_EXPOSED ||
        app_cardreader_get_state() == CARDREADER_APP_OWNED) {
        atomic_store(&s_state, USDISP_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    if (app_dap_get_state() == DAP_STATE_READY) {
        atomic_store(&s_state, USDISP_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    if (app_usb2ttl_get_state() == USB2TTL_ON) {
        atomic_store(&s_state, USDISP_ERROR);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_frame_buf) {
        s_frame_buf = heap_caps_malloc(USDISP_FRAME_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frame_buf) { atomic_store(&s_state, USDISP_ERROR); return ESP_ERR_NO_MEM; }
    }
    for (int i = 0; i < 2; i++) {
        if (!s_rgb_buf[i]) {
            s_rgb_buf[i] = heap_caps_aligned_alloc(
                16, DRV_LCD_H_RES * DRV_LCD_V_RES * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_rgb_buf[i]) { atomic_store(&s_state, USDISP_ERROR); return ESP_ERR_NO_MEM; }
            memset(s_rgb_buf[i], 0, DRV_LCD_H_RES * DRV_LCD_V_RES * sizeof(uint16_t));
        }
    }

    tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = { .skip_setup = false, .self_powered = false },
        .task = { .size = 4096, .priority = 5, .xCoreID = 0 },
        .descriptor = s_desc_cfg,
        .event_cb = tusb_event_cb,
    };
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install %s", esp_err_to_name(ret));
        atomic_store(&s_state, USDISP_ERROR);
        return ret;
    }
    s_usb_installed = true;

    xTaskCreatePinnedToCore(decode_task, "udisp_dec", 8192, NULL, 5, NULL, 1);

    s_proto.in_frame = false; s_proto.received = 0;
    atomic_store(&s_frame_count, 0);
    atomic_store(&s_error_count, 0);
    s_fps_window_start_us = 0;
    s_rgb_back_new = -1; s_rgb_front = 0;
    atomic_store(&s_state, USDISP_ACTIVE);
    ESP_LOGI(TAG, "enabled (PID=0x%04X)", USDISP_PID);
    return ESP_OK;
}

esp_err_t app_usbdisp_disable(void) {
    if ((usdisp_state_t)atomic_load(&s_state) == USDISP_OFF) return ESP_OK;
    atomic_store(&s_state, USDISP_OFF);
    if (s_poll_timer) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    if (s_usb_installed) {
        tinyusb_driver_uninstall();
        s_usb_installed = false;
    }
    ESP_LOGI(TAG, "disabled");
    return ESP_OK;
}
