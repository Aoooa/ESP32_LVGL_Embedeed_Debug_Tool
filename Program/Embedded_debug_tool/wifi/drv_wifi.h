#ifndef DRV_WIFI_H
#define DRV_WIFI_H

#include <stdbool.h>

#define DRV_WIFI_SSID    "Embedded-debug-tool"
#define DRV_WIFI_CHANNEL 6
#define DRV_WIFI_MAX_STA 5

/* 初始化并启动 SoftAP（首次调用）；之后重复调用 = 重启（已停止时恢复） */
void drv_wifi_init_softap(void);

/* 停止 SoftAP（esp_wifi_stop，可再次 init_softap 重启） */
void drv_wifi_stop_softap(void);

/* 查询 SoftAP 是否运行中（net_console 状态灯用） */
bool drv_wifi_ap_is_up(void);

#endif
