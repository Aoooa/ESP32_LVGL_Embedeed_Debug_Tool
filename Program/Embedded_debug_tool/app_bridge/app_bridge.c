#include "app_bridge.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "drv_uart.h"
#include "esp_log.h"

uart_bridge_t *g_bridges[2];
QueueHandle_t g_bcast_queue;
QueueHandle_t g_tcp_bcast_queue;

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
    if (br->send_raw_len <= 0 || !br->timer_on) return;

    /* 每次定时触发时，根据当前设置重新构建发送数据 */
    uint8_t final_buf[260];
    int n = 0;

    if (br->send_hex) {
        /* HEX 模式：将原始 hex 字符串解析为字节 */
        n = app_hex_to_bytes((const char *)br->send_raw, final_buf, sizeof(final_buf));
    } else {
        n = br->send_raw_len;
        memcpy(final_buf, br->send_raw, n);
    }

    if (br->send_newline && n + 2 <= (int)sizeof(final_buf)) {
        final_buf[n++] = '\r';
        final_buf[n++] = '\n';
    }

    drv_uart_write(br->port, final_buf, n);
    br->tx_bytes += n;
}

/* ──────────────── 发送 ──────────────── */

void app_uart_send(uart_bridge_t *br, const char *data, int len)
{
    /* 保存原始数据，定时器每次触发时根据当前设置重新构建 */
    int copy_len = (len < (int)sizeof(br->send_raw)) ? len : (int)sizeof(br->send_raw);
    memcpy(br->send_raw, data, copy_len);
    br->send_raw_len = copy_len;

    /* 立即发送（应用当前 HEX/换行设置） */
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

/* ──────────────── 转发任务 ──────────────── */

void app_uart_fwd_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    uint8_t buf[1024];
    ESP_LOGI(br->name, "fwd: TX=IO%d RX=IO%d", br->tx_pin, br->rx_pin);

    while (1) {
        int len = drv_uart_read(br->port, buf, sizeof(buf), 100);
        if (len <= 0) continue;

        /* 写入 TCP 广播队列 */
        if (g_tcp_bcast_queue) {
            bcast_item_t item;
            memcpy(item.data, buf, len);
            item.len = len;
            xQueueSend(g_tcp_bcast_queue, &item, 0);
        }

        /* 写入 WebSocket 广播队列 */
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
    br->send_raw_len = 0;
    br->tcp_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < APP_BRIDGE_MAX_TCP_CLIENTS; i++) {
        br->tcp_fds[i] = -1;
    }

    br->send_timer = xTimerCreate("uart_tx", pdMS_TO_TICKS(1000),
                                   pdTRUE, (void *)br, send_timer_cb);

    drv_uart_init(br->port, br->tx_pin, br->rx_pin);
    ESP_LOGI(br->name, "bridge ready: UART%d TCP=%d", br->port, br->tcp_port);
}
