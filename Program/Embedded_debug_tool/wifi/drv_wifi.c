#include "drv_wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "drv_wifi";

static bool s_ap_up;             /* SoftAP 运行标志（net_console 状态灯） */
static bool s_netif_created;     /* 默认 AP netif 只创建一次 */
static bool s_wifi_init_done;    /* esp_wifi_init 只做一次（重复 init 会 INVALID_STATE） */
static esp_event_handler_instance_t s_event_inst;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "+ " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "- " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    }
}

esp_err_t drv_wifi_init_softap(void)
{
    if (!s_netif_created) {
        esp_netif_t *netif = esp_netif_create_default_wifi_ap();
        if (!netif) {
            ESP_LOGE(TAG, "create default wifi ap netif failed");
            return ESP_ERR_NO_MEM;
        }
        s_netif_created = true;
    }

    esp_err_t ret;
    if (!s_wifi_init_done) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s (internal RAM shortage?)", esp_err_to_name(ret));
            return ret;
        }
        ret = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_event_inst);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "wifi event handler register failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_wifi_init_done = true;
    }

    wifi_config_t wc = {
        .ap = {
            .ssid = DRV_WIFI_SSID, .ssid_len = strlen(DRV_WIFI_SSID),
            .channel = DRV_WIFI_CHANNEL, .max_connection = DRV_WIFI_MAX_STA,
            .authmode = WIFI_AUTH_OPEN, .pmf_cfg = { .required = true },
        },
    };
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_ap_up = true;
    ESP_LOGI(TAG, "AP: %s ch%d", DRV_WIFI_SSID, DRV_WIFI_CHANNEL);
    return ESP_OK;
}

esp_err_t drv_wifi_stop_softap(void)
{
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_ap_up = false;
    ESP_LOGI(TAG, "AP stopped");
    return ESP_OK;
}

/* 完全释放 WiFi（esp_wifi_init 分配的静态缓冲）。须先 stop。
 * 释放后可再次 init_softap（netif/事件回调不重建） */
esp_err_t drv_wifi_deinit(void)
{
    if (!s_wifi_init_done) {
        return ESP_OK;   /* 未初始化则无操作 */
    }
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
    }
    ret = esp_wifi_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (s_event_inst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_event_inst);
        s_event_inst = NULL;
    }
    s_wifi_init_done = false;
    s_ap_up = false;
    ESP_LOGI(TAG, "wifi deinit (buffers released)");
    return ESP_OK;
}

bool drv_wifi_ap_is_up(void)
{
    return s_ap_up;
}
