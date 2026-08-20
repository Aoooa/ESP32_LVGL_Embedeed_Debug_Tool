#ifndef APP_DAP_H
#define APP_DAP_H

/* app_dap：DAP Link（CMSIS-DAP）服务层。
 *
 * 双传输通道（USB 直连 + USBIP 无线）共用同一组 SWD 端口（dap_ports[]）：
 *   - USB 通道：TinyUSB HID（tud_hid_set_report_cb，TinyUSB 任务）
 *   - 无线通道：USBIP server（TCP 872，usbip_server 任务）
 * 同一端口的 DAP 内核执行经 dap_port_locks[] 串行（跨任务互斥）。
 *
 * 线程：enable/disable 须在 LVGL 线程或持 esp_lv_adapter 锁调用；
 * 端口锁在任意任务可用。
 *
 * 互斥：与 USB 读卡器（app_cardreader）共用 USB PHY，不可同时开启；
 * 本模块在 enable 时检测读卡器状态并拒绝。
 */

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "drv_dap.h"

typedef enum {
    DAP_STATE_OFF = 0,   /* 关闭（USB 未接管） */
    DAP_STATE_READY,     /* 开启：USB 已接管，等待/已连接 PC */
    DAP_STATE_ERROR,     /* 开启失败（原因见日志/UI 提示） */
} dap_state_t;

/* 每端口 DAP 内核互斥锁（USB 通道与无线通道共用，串行执行）。
 * 懒创建 + 永久保留（无线模式不经过 app_dap_enable）；取锁请用
 * dap_port_lock_get()（自动创建，保证非 NULL）。 */
extern SemaphoreHandle_t dap_port_locks[DAP_PORT_COUNT];
SemaphoreHandle_t dap_port_lock_get(int port);

dap_state_t app_dap_get_state(void);
const char *app_dap_state_str(dap_state_t state);

/* 开启 USB DAP：初始化 SWD 引脚 + 安装 TinyUSB HID 设备。
 * 返回 ESP_ERR_INVALID_STATE = 读卡器正在使用 USB（先关闭读卡器） */
esp_err_t app_dap_enable(void);

/* 关闭 USB DAP：卸载 TinyUSB + USB PHY 归还 USB-Serial/JTAG（恢复 COM 控制台） */
esp_err_t app_dap_disable(void);

/* ── 无线 DAP（USBIP，TCP 872） ── */

typedef enum {
    DAP_WIFI_OFF = 0,   /* 无线关闭 */
    DAP_WIFI_ON,        /* WiFi AP + USBIP server 运行中 */
    DAP_WIFI_ERROR,     /* 启动失败 */
} dap_wifi_state_t;

/* 开启无线：启动 WiFi SoftAP + USBIP server（默认关闭，单独开关）。
 * PC 端 usbip-win: usbip.exe attach_ude -r <设备IP> -b 1-1 */
esp_err_t app_dap_wifi_enable(void);

/* 关闭无线：停止 USBIP server + WiFi AP */
esp_err_t app_dap_wifi_disable(void);

dap_wifi_state_t app_dap_wifi_get_state(void);
const char *app_dap_wifi_state_str(dap_wifi_state_t state);

/* USB 描述符（USB 直连与 USBIP 共用；usbip_server 经访问函数读取） */
const void *dap_usb_device_desc(void);
int dap_usb_config_desc(uint8_t *buf, int maxlen);
int dap_usb_hid_report_desc(uint8_t *buf, int maxlen);
const char *dap_usb_string(int idx);

#endif /* APP_DAP_H */
