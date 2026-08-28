/* app_usbdisp.h —— USB 电脑副屏服务（USB Vendor bulk 接收 MJPEG 帧）
 *
 * 协议：xfz1986 协议（win10_idd_xfz1986_usb_graphic_driver_display）。
 *   VID=0x303A / PID=0x2987（单功能显示）/ product=esp32udisp0_R240x320_Ejpg6_Ergb16_Bl20
 *   帧头 16B：crc16(2) + type(1:0=RGB565/3=JPG) + cmd(1) + x(2) + y(2) + w(2) + h(2) + id10b|len22b(4)
 *   帧结束：short packet (< EP_SIZE) 或 payload_total 已收齐
 *   解码：esp_new_jpeg（image_viewer 复用），输出 RGB565 → lv_image_dsc 注入 LVGL
 *
 * 互斥：与 app_cardreader / app_dap / app_usb2ttl 互斥使用 USB OTG PHY
 *   enable() 开头检查三服务状态，否则 ESP_ERR_INVALID_STATE
 *
 * 线程：enable/disable 须在 LVGL 线程或持 esp_lv_adapter 锁调用
 *   - vendor_rx_cb（USB 任务）：解析协议 → 帧完成入队 ready_frame
 *   - decode_task（独立任务）：队取帧 → esp_new_jpeg → 写共享 RGB565 → lvgl_post
 *   - lv_timer（LVGL 线程）：检测共享帧新 → lv_image_set_src + lv_obj_invalidate
 *
 * 自动关闭：USB 拔出（TINYUSB_EVENT_DETACHED）后强制 disable，恢复给其它 APP
 */
#ifndef APP_USBDISP_H
#define APP_USBDISP_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    USDISP_OFF = 0,    /* 未启用：USB 归还其它 APP */
    USDISP_ACTIVE,     /* 启用：USB Vendor bulk + 帧解码中 */
    USDISP_ERROR,      /* 上次操作失败 */
} usdisp_state_t;

usdisp_state_t app_usbdisp_get_state(void);
const char *app_usbdisp_state_str(usdisp_state_t);

/* 开启副屏。检查互斥后启动 USB + 解码任务。
 * 失败返回 ESP_ERR_INVALID_STATE（peer USB 服务占用）。 */
esp_err_t app_usbdisp_enable(void);

/* 关闭副屏（任意状态幂等），恢复 USB 给其它 APP。 */
esp_err_t app_usbdisp_disable(void);

/* 诊断计数（用于 UI 显示 / 日志）。线程安全（原子读）。 */
uint32_t app_usbdisp_get_frame_count(void);
uint32_t app_usbdisp_get_error_count(void);
float    app_usbdisp_get_fps(void);
bool     app_usbdisp_pc_connected(void);  /* tud_ready() && tud_vendor_mounted() */

/* 帧回调注册（APP 实现，LVGL 线程内被调用）。
 * 在收到新解码帧时通知 APP 切换显示缓冲。 */
typedef void (*app_usbdisp_frame_cb_t)(void *ctx,
                                       const uint16_t *rgb565,
                                       uint32_t w, uint32_t h);
void app_usbdisp_register_frame_cb(app_usbdisp_frame_cb_t cb, void *ctx);

#endif /* APP_USBDISP_H */
