#include "app_uart.h"
#include "io_picker.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/task.h"

uart_bridge_t *g_bridges[2];
QueueHandle_t g_bcast_queue;
QueueHandle_t g_tcp_bcast_queue;
QueueHandle_t g_display_queue;

static int hex_char_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int app_hex_to_bytes(const char *hex, uint8_t *out, int max_len)
{
    int len = 0;
    while (hex[0] && hex[1] && len < max_len) {
        int hi = hex_char_val(hex[0]);
        int lo = hex_char_val(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[len++] = (hi << 4) | lo;
        hex += 2;
    }
    return len;
}

static void send_timer_cb(TimerHandle_t xTimer)
{
    uart_bridge_t *br = (uart_bridge_t *)pvTimerGetTimerID(xTimer);
    if (br->send_raw_len <= 0 || !br->timer_on) return;

    uint8_t buf[260];
    int n = 0;
    if (br->send_hex) {
        n = app_hex_to_bytes((const char *)br->send_raw, buf, sizeof(buf));
    } else {
        n = br->send_raw_len;
        memcpy(buf, br->send_raw, n);
    }
    if (br->send_newline && n + 2 <= (int)sizeof(buf)) {
        buf[n++] = '\r';
        buf[n++] = '\n';
    }
    drv_uart_write(br->port, buf, n);
    br->tx_bytes += n;
}

void app_uart_send(uart_bridge_t *br, const char *data, int len)
{
    int copy_len = (len < (int)sizeof(br->send_raw)) ? len : (int)sizeof(br->send_raw);
    memcpy(br->send_raw, data, copy_len);
    br->send_raw_len = copy_len;

    uint8_t buf[260];
    int n = 0;
    if (br->send_hex) {
        n = app_hex_to_bytes(data, buf, sizeof(buf));
    } else {
        n = copy_len;
        memcpy(buf, data, n);
    }
    if (br->send_newline && n + 2 <= (int)sizeof(buf)) {
        buf[n++] = '\r';
        buf[n++] = '\n';
    }
    drv_uart_write(br->port, buf, n);
    br->tx_bytes += n;
    ESP_LOGI(br->name, "TX %d bytes", n);
}

void app_uart_fwd_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    uint8_t buf[1024];

    while (1) {
        /* UART 惰性占用：驱动未装载（APP 未开）或暂停时循环等待，
         * 不读端口（防未装驱动时 uart_read_bytes 空转） */
        if (!br->active || br->paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int len = drv_uart_read(br->port, buf, sizeof(buf), 10);
        if (len <= 0) continue;

#if APP_NET_UART_FWD_ENABLED
        if (g_tcp_bcast_queue) {
            bcast_item_t item;
            memcpy(item.data, buf, len);
            item.len = len;
            xQueueSend(g_tcp_bcast_queue, &item, 0);
        }
        if (!br->paused && g_bcast_queue) {
            bcast_item_t item;
            memcpy(item.data, buf, len);
            item.len = len;
            xQueueSend(g_bcast_queue, &item, 0);
        }
#endif
        if (!br->paused && g_display_queue) {
            disp_item_t di;
            int copy = (len < (int)sizeof(di.data)) ? len : (int)sizeof(di.data);
            memcpy(di.data, buf, copy);
            di.len = copy;
            di.uart_idx = (br->port == UART_NUM_1) ? 0 : 1;
            xQueueSend(g_display_queue, &di, 0);
        }
    }
}

void app_uart_init(uart_bridge_t *br)
{
    br->paused = 0;
    br->active = 0;   /* 惰性：上电不装载驱动；APP 打开时 app_uart_start 再占 IO */
    br->send_newline = 0;
    br->send_hex = 0;
    br->timer_on = 0;
    br->timer_ms = 1000;
    br->send_raw_len = 0;
    br->tx_bytes = 0;
    br->tcp_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < APP_UART_MAX_TCP_CLIENTS; i++) br->tcp_fds[i] = -1;

    br->send_timer = xTimerCreate("uart_tx", pdMS_TO_TICKS(1000),
                                   pdTRUE, (void *)br, send_timer_cb);
}

/* 装载两个桥接 UART 驱动（幂等：已 active 跳过）。终端 APP 打开时调用，
 * 期间占用对应 IO（io_picker 记账，其它 APP 选 IO 时置灰）；未占用时
 * 释放（drv_uart_init 内部幂等） */
esp_err_t app_uart_start(void)
{
    for (int i = 0; i < 2; i++) {
        uart_bridge_t *br = g_bridges[i];
        if (!br) continue;
        if (!br->active) {
            drv_uart_init(br->port, br->tx_pin, br->rx_pin);
            br->active = 1;
            io_picker_reserve(br->tx_pin);   /* 终端占用期间其它 APP 不可选 */
            io_picker_reserve(br->rx_pin);
        }
    }
    return ESP_OK;
}

/* 卸载两个桥接 UART 驱动并释放 IO（幂等：未 active 跳过） */
esp_err_t app_uart_stop(void)
{
    for (int i = 0; i < 2; i++) {
        uart_bridge_t *br = g_bridges[i];
        if (!br) continue;
        if (br->active) {
            br->active = 0;
            drv_uart_deinit(br->port, br->tx_pin, br->rx_pin);
            io_picker_release(br->tx_pin);
            io_picker_release(br->rx_pin);
        }
    }
    return ESP_OK;
}
