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
#include "drv_wave.h"
#include "esp_lv_adapter.h"
#include "esp_heap_caps.h"

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

    /* 默认不启动 SoftAP（省电/安全）；由 net_console 页面"开启"按钮控制 */
    /* app_wifi_start(); */

    g_display_queue = xQueueCreate(APP_UART_DISPLAY_QUEUE_LEN, sizeof(disp_item_t));
#if APP_NET_UART_FWD_ENABLED
    g_bcast_queue = xQueueCreate(APP_UART_BCAST_QUEUE_LEN, sizeof(bcast_item_t));
    g_tcp_bcast_queue = xQueueCreate(APP_UART_BCAST_QUEUE_LEN, sizeof(bcast_item_t));
#endif

    app_uart_init(&s_bridge1);
    app_uart_init(&s_bridge2);

    /* 波形输出服务（幂等，启动期一次） */
    drv_wave_init();

#if APP_NET_UART_FWD_ENABLED
    app_tcp_start();
#endif
    app_web_start();

    /* 先建 UI（文件浏览器显示"SD card not ready"），SD 挂载成功后通知刷新。
     * SD 与 LCD 共享 SPI2 总线：挂载/枚举期间持 LVGL 锁，独占总线防并发 */
    app_display_start();

    /* APP 回调测试（交付默认关闭；启用：CMakeLists 取消 app_test.c 注释 + 此处改 #if 1） */
#if 0
    #include "app_test.h"
    lv_timer_t *t = lv_timer_create((lv_timer_cb_t)app_test_run, 2000, NULL);
    lv_timer_set_repeat_count(t, 1);
#endif

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (drv_sdcard_init() == ESP_OK) {
            app_display_notify_sd_ready();
        }
        esp_lv_adapter_unlock();
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

    /* 内存诊断（官方 esp_heap_caps API，启动后快照） */
    printf("MEM internal free=%d largest=%d | DMA free=%d largest=%d | PSRAM free=%d largest=%d\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (int)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    fflush(stdout);
}
