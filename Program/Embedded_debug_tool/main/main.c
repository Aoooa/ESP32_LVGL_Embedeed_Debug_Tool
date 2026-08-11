/*
 * Embedded Debug Tool — 主入口
 */

#include <stdio.h>
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "app_uart.h"
#include "app_wifi.h"
#include "app_tcp.h"
#include "app_web.h"
#include "app_display.h"
#include "drv_sdcard.h"
#include "app_sdcard.h"

static uart_bridge_t s_bridge1 = {
    .port = UART_NUM_1, .tx_pin = 2, .rx_pin = 4,
    .tcp_port = 8080, .name = "UART1",
};

static uart_bridge_t s_bridge2 = {
    .port = UART_NUM_2, .tx_pin = 16, .rx_pin = 17,
    .tcp_port = 8081, .name = "UART2",
};

void app_main(void)
{
    g_bridges[0] = &s_bridge1;
    g_bridges[1] = &s_bridge2;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_wifi_start();

    g_display_queue = xQueueCreate(APP_UART_DISPLAY_QUEUE_LEN, sizeof(disp_item_t));
#if APP_NET_UART_FWD_ENABLED
    g_bcast_queue = xQueueCreate(APP_UART_BCAST_QUEUE_LEN, sizeof(bcast_item_t));
    g_tcp_bcast_queue = xQueueCreate(APP_UART_BCAST_QUEUE_LEN, sizeof(bcast_item_t));
#endif

    app_uart_init(&s_bridge1);
    app_uart_init(&s_bridge2);

#if APP_NET_UART_FWD_ENABLED
    app_tcp_start();
#endif
    app_web_start();
    app_display_start();

    /* SD 卡：挂载并自检（串口日志） */
    if (drv_sdcard_init() == ESP_OK) {
        app_sdcard_self_test();
    }

    /* 内部 RAM 紧张（显示双缓冲占 ~51KB），任务栈放 PSRAM */
#if APP_NET_UART_FWD_ENABLED
    xTaskCreateWithCaps(app_ws_broadcast_task, "ws_bcast", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM);
#endif
    xTaskCreateWithCaps(app_uart_fwd_task, "fwd1", 4096, &s_bridge1, 5, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(app_uart_fwd_task, "fwd2", 4096, &s_bridge2, 5, NULL, MALLOC_CAP_SPIRAM);

    printf("\n=== Ready ===\n");
    printf(" WiFi: Embedded-debug-tool\n");
    printf(" Web:  http://192.168.4.1/\n");
    printf(" UART1: http://192.168.4.1/page?uart=0  TCP :8080\n");
    printf(" UART2: http://192.168.4.1/page?uart=1  TCP :8081\n\n");
    fflush(stdout);
}
