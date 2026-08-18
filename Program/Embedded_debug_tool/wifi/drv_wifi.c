#include "drv_wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"

static bool s_ap_up;   /* SoftAP 运行标志（net_console 状态灯） */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI("drv_wifi", "+ " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI("drv_wifi", "- " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    }
}

void drv_wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {
        .ap = {
            .ssid = DRV_WIFI_SSID, .ssid_len = strlen(DRV_WIFI_SSID),
            .channel = DRV_WIFI_CHANNEL, .max_connection = DRV_WIFI_MAX_STA,
            .authmode = WIFI_AUTH_OPEN, .pmf_cfg = { .required = true },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_up = true;
    ESP_LOGI("drv_wifi", "AP: %s ch%d", DRV_WIFI_SSID, DRV_WIFI_CHANNEL);
}

void drv_wifi_stop_softap(void)
{
    if (esp_wifi_stop() != ESP_OK) {
        ESP_LOGW("drv_wifi", "esp_wifi_stop failed");
        return;
    }
    s_ap_up = false;
    ESP_LOGI("drv_wifi", "AP stopped");
}

bool drv_wifi_ap_is_up(void)
{
    return s_ap_up;
}
