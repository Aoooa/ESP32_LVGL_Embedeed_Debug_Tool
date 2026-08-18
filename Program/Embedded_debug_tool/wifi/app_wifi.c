#include "app_wifi.h"
#include "drv_wifi.h"

void app_wifi_start(void)
{
    drv_wifi_init_softap();
}

void app_wifi_stop(void)
{
    drv_wifi_stop_softap();
}

bool app_wifi_is_up(void)
{
    return drv_wifi_ap_is_up();
}
