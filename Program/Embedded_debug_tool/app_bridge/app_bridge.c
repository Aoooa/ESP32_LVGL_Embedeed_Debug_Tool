#include "app_bridge.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "drv_uart.h"
#include "esp_log.h"

static const char *TAG = "app_bridge";

uart_bridge_t *g_bridges[2];
QueueHandle_t g_bcast_queue;

/* ──────────────── HEX 工具 ──────────────── */

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

/* ──────────────── 定时器回调 ──────────────── */

static void send_timer_cb(TimerHandle_t xTimer)
{
    uart_bridge_t *br = (uart_bridge_t *)pvTimerGetTimerID(xTimer);
    if (br->send_len > 0) {
        drv_uart_write(br->port, (const uint8_t *)br->send_buf, br->send_len);
    }
}

/* ──────────────── 发送 ──────────────── */

void app_uart_send(uart_bridge_t *br, const char *data, int len)
{
    uint8_t buf[256];
    int n;

    if (br->send_hex) {
        n = app_hex_to_bytes(data, buf, sizeof(buf));
    } else {
        n = (len < (int)sizeof(buf)) ? len : (int)sizeof(buf);
        memcpy(buf, data, n);
    }

    if (br->send_newline && n + 2 <= (int)sizeof(buf)) {
        buf[n++] = '\r';
        buf[n++] = '\n';
    }

    memcpy(br->send_buf, buf, n);
    br->send_len = n;

    drv_uart_write(br->port, buf, n);
    ESP_LOGI(br->name, "TX %d bytes", n);
}

/* ──────────────── 转发任务 ──────────────── */

void app_uart_fwd_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    uint8_t buf[1024];
    ESP_LOGI(br->name, "fwd: TX=IO%d RX=IO%d", br->tx_pin, br->rx_pin);

    while (1) {
        int len = drv_uart_read(br->port, buf, sizeof(buf), 100);
        if (len <= 0) continue;

        /* TCP 广播 */
        xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
        for (int i = 0; i < APP_BRIDGE_MAX_TCP_CLIENTS; i++) {
            if (br->tcp_fds[i] >= 0) {
                if (send(br->tcp_fds[i], buf, len, 0) < 0) {
                    close(br->tcp_fds[i]);
                    br->tcp_fds[i] = -1;
                }
            }
        }
        xSemaphoreGive(br->tcp_mutex);

        /* WebSocket 广播 */
        if (!br->paused && g_bcast_queue) {
            bcast_item_t item;
            memcpy(item.data, buf, len);
            item.len = len;
            xQueueSend(g_bcast_queue, &item, 0);
        }
    }
}

/* ──────────────── 初始化 ──────────────── */

void app_bridge_init(uart_bridge_t *br)
{
    br->paused = 0;
    br->send_newline = 0;
    br->send_hex = 0;
    br->timer_on = 0;
    br->timer_ms = 1000;
    br->send_len = 0;
    br->tcp_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < APP_BRIDGE_MAX_TCP_CLIENTS; i++) {
        br->tcp_fds[i] = -1;
    }

    br->send_timer = xTimerCreate("uart_tx", pdMS_TO_TICKS(1000),
                                   pdTRUE, (void *)br, send_timer_cb);

    drv_uart_init(br->port, br->tx_pin, br->rx_pin);
    ESP_LOGI(br->name, "bridge ready: UART%d TCP=%d", br->port, br->tcp_port);
}
