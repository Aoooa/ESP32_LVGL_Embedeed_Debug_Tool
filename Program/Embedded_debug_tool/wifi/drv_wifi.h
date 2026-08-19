#ifndef DRV_WIFI_H
#define DRV_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

#define DRV_WIFI_SSID    "Embedded-debug-tool"
#define DRV_WIFI_CHANNEL 6
#define DRV_WIFI_MAX_STA 5

/* 初始化并启动 SoftAP（首次调用）；之后重复调用 = 重启（已停止时恢复）。
 * 内部 RAM 不足时 esp_wifi_init 可能返回 ESP_ERR_NO_MEM，调用方需处理（不崩溃） */
esp_err_t drv_wifi_init_softap(void);

/* 停止 SoftAP（esp_wifi_stop，可再次 init_softap 重启） */
esp_err_t drv_wifi_stop_softap(void);

/* 完全释放 WiFi（esp_wifi_init 的静态缓冲）。须先 stop；释放后仍可再次 init_softap。
 * 供退出 SerialIP 时归还内部 RAM 给显示缓冲 */
esp_err_t drv_wifi_deinit(void);

/* 查询 SoftAP 是否运行中（net_console 状态灯用） */
bool drv_wifi_ap_is_up(void);

#endif
