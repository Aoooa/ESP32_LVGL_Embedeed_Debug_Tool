#include "app_wifi.h"
#include "drv_wifi.h"
#include "esp_log.h"

static const char *TAG = "app_wifi";

esp_err_t app_wifi_start(void)
{
    esp_err_t ret = drv_wifi_init_softap();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t app_wifi_stop(void)
{
    return drv_wifi_stop_softap();
}

esp_err_t app_wifi_deinit(void)
{
    esp_err_t ret = drv_wifi_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "deinit failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool app_wifi_is_up(void)
{
    return drv_wifi_ap_is_up();
}
