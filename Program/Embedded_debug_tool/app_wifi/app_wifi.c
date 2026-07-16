#include "app_wifi.h"
#include "drv_wifi.h"

void app_wifi_start(void)
{
    drv_wifi_init_softap();
}
