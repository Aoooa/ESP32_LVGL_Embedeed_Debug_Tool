#include "app_web.h"
#include "app_uart.h"
#include "app_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drv_uart.h"
#include "esp_log.h"

static void ws_broadcast_status(uart_bridge_t *br)
{
    char status[64];
    int sn = snprintf(status, sizeof(status), "S:%d,%d,%d,%lu",
                      br->timer_on, br->timer_ms, br->paused,
                      (unsigned long)br->tx_bytes);
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)status,
        .len = sn, .final = true
    };
    size_t count = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(g_httpd, &count, fds) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            if (httpd_ws_get_fd_info(g_httpd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(g_httpd, fds[i], &frame);
            }
        }
    }
}

esp_err_t app_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int idx = (req->uri[4] == '1') ? 1 : 0;
        ESP_LOGI("app_ws", "connected fd=%d uart%d", httpd_req_to_sockfd(req), idx + 1);
        ws_broadcast_status(g_bridges[idx]);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    uint8_t *buf = malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        buf[frame.len] = 0;
        int idx = (req->uri[4] == '1') ? 1 : 0;
        uart_bridge_t *br = g_bridges[idx];
        char *msg = (char *)buf;

        if (strncmp(msg, "pause:", 6) == 0) {
            br->paused = atoi(msg + 6);
        } else if (strcmp(msg, "clear") == 0) {
            /* 无操作 */
        } else if (strncmp(msg, "send:", 5) == 0) {
            app_uart_send(br, msg + 5, frame.len - 5);
        } else if (strncmp(msg, "sendh:", 6) == 0) {
            uint8_t bin[256];
            int n = app_hex_to_bytes(msg + 6, bin, sizeof(bin));
            if (br->send_newline && n + 2 <= (int)sizeof(bin)) {
                bin[n++] = '\r'; bin[n++] = '\n';
            }
            int raw_len = frame.len - 6;
            if (raw_len > (int)sizeof(br->send_raw)) raw_len = sizeof(br->send_raw);
            memcpy(br->send_raw, msg + 6, raw_len);
            br->send_raw_len = raw_len;
            drv_uart_write(br->port, bin, n);
            br->tx_bytes += n;
        } else if (strncmp(msg, "cfg:", 4) == 0) {
            br->send_newline = msg[4] - '0';
            br->send_hex = msg[6] - '0';
        } else if (strncmp(msg, "tconf:", 6) == 0) {
            br->timer_ms = atoi(msg + 6);
            if (br->send_timer) xTimerChangePeriod(br->send_timer, pdMS_TO_TICKS(br->timer_ms), 0);
        } else if (strncmp(msg, "timer:", 6) == 0) {
            br->timer_on = atoi(msg + 6);
            if (br->timer_on && br->send_timer && br->send_raw_len > 0) {
                xTimerStart(br->send_timer, 0);
            } else if (br->send_timer) {
                xTimerStop(br->send_timer, 0);
            }
        }
        ws_broadcast_status(br);
        app_display_notify_status();
    }
    free(buf);
    return ret;
}
