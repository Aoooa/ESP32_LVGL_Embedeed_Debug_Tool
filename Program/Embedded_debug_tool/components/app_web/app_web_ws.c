#include "app_web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drv_uart.h"
#include "esp_log.h"

static const char *TAG = "app_ws";

static void ws_send_status(httpd_handle_t hd, int fd, uart_bridge_t *br)
{
    char status[64];
    int sn = snprintf(status, sizeof(status), "S:%d,%d,%d",
                      br->timer_on, br->timer_ms, br->paused);
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)status,
        .len = sn,
        .final = true
    };
    httpd_ws_send_frame_async(hd, fd, &frame);
}

esp_err_t app_ws_handler(httpd_req_t *req)
{
    /* 新客户端连接 — 发送当前状态 */
    if (req->method == HTTP_GET) {
        int idx = (req->uri[4] == '1') ? 1 : 0;
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "connected fd=%d uart%d", fd, idx + 1);
        ws_send_status(g_httpd, fd, g_bridges[idx]);
        return ESP_OK;
    }

    /* 接收 WebSocket 消息 */
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
        int fd = httpd_req_to_sockfd(req);
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
                bin[n++] = '\r';
                bin[n++] = '\n';
            }
            memcpy(br->send_buf, bin, n);
            br->send_len = n;
            drv_uart_write(br->port, bin, n);
        } else if (strncmp(msg, "cfg:", 4) == 0) {
            br->send_newline = msg[4] - '0';
            br->send_hex = msg[6] - '0';
        } else if (strncmp(msg, "tconf:", 6) == 0) {
            br->timer_ms = atoi(msg + 6);
            if (br->send_timer) {
                xTimerChangePeriod(br->send_timer, pdMS_TO_TICKS(br->timer_ms), 0);
            }
        } else if (strncmp(msg, "timer:", 6) == 0) {
            br->timer_on = atoi(msg + 6);
            if (br->timer_on && br->send_timer) {
                xTimerStart(br->send_timer, 0);
            } else if (br->send_timer) {
                xTimerStop(br->send_timer, 0);
            }
        }
        /* 确认状态 */
        ws_send_status(g_httpd, fd, br);
    }
    free(buf);
    return ret;
}
