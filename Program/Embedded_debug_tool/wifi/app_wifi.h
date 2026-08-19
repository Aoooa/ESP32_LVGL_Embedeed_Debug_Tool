#ifndef APP_WIFI_H
#define APP_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t app_wifi_start(void);
esp_err_t app_wifi_stop(void);
esp_err_t app_wifi_deinit(void);
bool app_wifi_is_up(void);

#endif
