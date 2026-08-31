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
#include "app_usbdisp.h"
#include "drv_sdcard.h"
#include "drv_wave.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* USB 副屏自测模式：编译时打开，会自动周期性启停副屏用于 Windows 端验证 */
#define USDISP_SELFTEST_ENABLE     1   /* start auto-enable, no APP needed */
#if USDISP_SELFTEST_ENABLE
static void usdisp_selftest_task(void *arg) {
    (void)arg;
    ESP_LOGI("usdisp_selftest", "started, alternating 3s on / 10s off cycle");
    vTaskDelay(pdMS_TO_TICKS(5000));
    bool on = false;
    while (1) {
        if (!on) {
            esp_err_t r = app_usbdisp_enable();
            ESP_LOGI("usdisp_selftest", "ENABLE -> %s (st=%s)", esp_err_to_name(r), app_usbdisp_state_str(app_usbdisp_get_state()));
            vTaskDelay(pdMS_TO_TICKS(3000));   /* vendor 3s - PC test window */
        } else {
            app_usbdisp_disable();
            ESP_LOGI("usdisp_selftest", "DISABLE (COM back, flash window)");
            vTaskDelay(pdMS_TO_TICKS(10000));  /* USJ 10s - flash window */
        }
        on = !on;
    }
}
#endif

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

    /* 先初始化显示平台（保持黑屏，暂不建 UI），SD 挂载完成后一次刷新 */
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

    /* 全部初始化完成 → 一次构建桌面并刷新（上电黑屏 → 桌面，无中间闪烁） */
    app_display_build_ui();

#if USDISP_SELFTEST_ENABLE
    xTaskCreatePinnedToCore(usdisp_selftest_task, "udisp_test", 4096, NULL, 3, NULL, 0);
#endif

    printf("\n=== Ready ===\n");
    printf(" WiFi: Embedded-debug-tool\n");
    printf(" Web:  http://192.168.4.1/\n");
    printf(" UART1: http://192.168.4.1/page?uart=0  TCP :8080\n");
    printf(" UART2: http://192.168.4.1/page?uart=1  TCP :8081\n\n");
    fflush(stdout);
}
