#ifndef DRV_WIFI_H
#define DRV_WIFI_H

#define DRV_WIFI_SSID    "Embedded-debug-tool"
#define DRV_WIFI_CHANNEL 6
#define DRV_WIFI_MAX_STA 5

/**
 * @brief 初始化 WiFi AP 模式
 */
void drv_wifi_init_softap(void);

#endif /* DRV_WIFI_H */
