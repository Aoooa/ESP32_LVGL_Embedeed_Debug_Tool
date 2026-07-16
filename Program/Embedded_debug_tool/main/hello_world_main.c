/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * UART-to-TCP/WebSocket bridge.
 * - TCP raw socket: port 8080/8081
 * - Web UI: port 80, WebSocket real-time push
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/uart.h"

/* ──────────────────── Config ──────────────────── */

#define WIFI_SSID       "Embedded-debug-tool"
#define WIFI_CHANNEL    6
#define MAX_STA_CONN    5
#define MAX_TCP_CLIENTS 3
#define UART_BUF_SIZE   1024
#define UART_BAUD_RATE  115200
#define BROADCAST_QUEUE_LEN 32

/* ──────────────────── UART Bridge ──────────────────── */

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int tcp_port;
    const char *name;
    volatile int paused;
    int tcp_fds[MAX_TCP_CLIENTS];
    SemaphoreHandle_t tcp_mutex;
} uart_bridge_t;

/* ──────────────────── Broadcast Queue ──────────────────── */

typedef struct {
    uint8_t data[UART_BUF_SIZE];
    int len;
} bcast_item_t;

static QueueHandle_t g_bcast_queue;
static httpd_handle_t g_httpd;

/* ──────────────────── TCP Server ──────────────────── */

static void tcp_broadcast(uart_bridge_t *br, const uint8_t *data, int len)
{
    xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (br->tcp_fds[i] >= 0) {
            if (send(br->tcp_fds[i], data, len, 0) < 0) {
                close(br->tcp_fds[i]);
                br->tcp_fds[i] = -1;
            }
        }
    }
    xSemaphoreGive(br->tcp_mutex);
}

static void tcp_server_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { vTaskDelete(NULL); return; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(br->tcp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv); vTaskDelete(NULL); return;
    }
    listen(srv, MAX_TCP_CLIENTS);
    printf("[%s] TCP on port %d\n", br->name, br->tcp_port);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        int maxfd = srv;

        xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (br->tcp_fds[i] >= 0) {
                FD_SET(br->tcp_fds[i], &rfds);
                if (br->tcp_fds[i] > maxfd) maxfd = br->tcp_fds[i];
            }
        }
        xSemaphoreGive(br->tcp_mutex);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) < 0) continue;

        if (FD_ISSET(srv, &rfds)) {
            struct sockaddr_in ca; socklen_t cl = sizeof(ca);
            int fd = accept(srv, (struct sockaddr *)&ca, &cl);
            if (fd >= 0) {
                xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
                for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
                    if (br->tcp_fds[i] < 0) {
                        br->tcp_fds[i] = fd;
                        printf("[%s] TCP+ fd=%d\n", br->name, fd);
                        break;
                    }
                }
                xSemaphoreGive(br->tcp_mutex);
                close(fd);
            }
        }

        xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            int fd = br->tcp_fds[i];
            if (fd >= 0 && FD_ISSET(fd, &rfds)) {
                uint8_t tmp[512];
                int n = recv(fd, tmp, sizeof(tmp), 0);
                if (n > 0) {
                    uart_write_bytes(br->port, tmp, n);
                } else {
                    close(fd); br->tcp_fds[i] = -1;
                    printf("[%s] TCP- fd=%d\n", br->name, fd);
                }
            }
        }
        xSemaphoreGive(br->tcp_mutex);
    }
}

/* ──────────────────── UART Forward ──────────────────── */

static void uart_forward_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    uint8_t buf[UART_BUF_SIZE];
    printf("[%s] forward: TX=IO%d RX=IO%d\n", br->name, br->tx_pin, br->rx_pin);

    while (1) {
        int len = uart_read_bytes(br->port, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        /* TCP broadcast */
        tcp_broadcast(br, buf, len);

        /* WebSocket broadcast via queue */
        if (!br->paused) {
            bcast_item_t item;
            memcpy(item.data, buf, len);
            item.len = len;
            if (xQueueSend(g_bcast_queue, &item, 0) != pdTRUE) {
                /* Queue full — drop data */
            }
        }
    }
}

/* ──────────────────── WebSocket Broadcast ──────────────────── */

static void ws_broadcast_work(void *arg)
{
    bcast_item_t *item = (bcast_item_t *)arg;
    size_t client_count = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];

    if (httpd_get_client_list(g_httpd, &client_count, fds) == ESP_OK) {
        for (size_t i = 0; i < client_count; i++) {
            if (httpd_ws_get_fd_info(g_httpd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = item->data,
                    .len = item->len,
                    .final = true,
                };
                httpd_ws_send_frame_async(g_httpd, fds[i], &frame);
            }
        }
    }
    free(item);
}

static void ws_broadcast_task(void *arg)
{
    bcast_item_t item;
    while (1) {
        if (xQueueReceive(g_bcast_queue, &item, portMAX_DELAY) == pdTRUE) {
            bcast_item_t *copy = malloc(sizeof(bcast_item_t));
            if (copy) {
                memcpy(copy, &item, sizeof(bcast_item_t));
                httpd_queue_work(g_httpd, ws_broadcast_work, copy);
            }
        }
    }
}

/* ──────────────────── WebSocket Handler ──────────────────── */

static uart_bridge_t *g_bridges[2];

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        printf("[WS] client connected fd=%d\n", httpd_req_to_sockfd(req));
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
        if (strncmp((char *)buf, "pause:", 6) == 0) {
            int val = atoi((char *)buf + 6);
            int idx = (req->uri[4] == '1') ? 1 : 0;
            g_bridges[idx]->paused = val;
            printf("[WS] uart%d paused=%d\n", idx, val);
        } else if (strcmp((char *)buf, "clear") == 0) {
            /* Nothing to clear for WebSocket (no ring buffer) */
        }
    }
    free(buf);
    return ret;
}

/* ──────────────────── WiFi AP ──────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        printf("[WIFI] + " MACSTR " AID=%d\n", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        printf("[WIFI] - " MACSTR " AID=%d\n", MAC2STR(e->mac), e->aid);
    }
}

static void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {
        .ap = {
            .ssid = WIFI_SSID, .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL, .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_OPEN, .pmf_cfg = { .required = true },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("[WIFI] AP: %s ch%d\n", WIFI_SSID, WIFI_CHANNEL);
}

/* ──────────────────── HTTP Handlers ──────────────────── */

static void set_no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
}

static esp_err_t root_handler(httpd_req_t *req)
{
    set_no_cache(req);
    const char *html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>Embedded Debug Tool</title>"
        "<style>body{font:14px monospace;background:#1a1a1a;color:#ccc;padding:20px}"
        "a{color:#4fc3f7}li{margin:8px 0}h2{color:#4fc3f7}</style></head><body>"
        "<h2>Embedded Debug Tool</h2><ul>"
        "<li><a href=\"/page?uart=0\">UART1 Web (IO2/IO4)</a> | TCP 192.168.4.1:8080</li>"
        "<li><a href=\"/page?uart=1\">UART2 Web (IO16/IO17)</a> | TCP 192.168.4.1:8081</li>"
        "</ul></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t page_handler(httpd_req_t *req)
{
    set_no_cache(req);

    char qbuf[16] = {0};
    int idx = 0;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char v[4];
        if (httpd_query_key_value(qbuf, "uart", v, sizeof(v)) == ESP_OK) idx = atoi(v);
    }
    if (idx < 0 || idx > 1) idx = 0;
    uart_bridge_t *br = g_bridges[idx];

    char *page = malloc(2048);
    if (!page) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    int n = snprintf(page, 2048,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>UART%d</title><style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font:13px/1.3 monospace;background:#1a1a1a;color:#ccc;padding:8px}"
        "h3{color:#4fc3f7;margin-bottom:6px}"
        "textarea{width:100%%;height:60vh;background:#111;color:#0f0;border:1px solid #333;"
        "font:12px/1.4 monospace;resize:vertical;padding:4px}"
        ".b{display:flex;gap:6px;margin:6px 0;align-items:center}"
        "button{padding:4px 12px;border:1px solid #555;background:#222;color:#ccc;"
        "cursor:pointer;font:12px monospace;border-radius:3px}"
        "button:hover{background:#333}"
        ".on{border-color:#4caf50;color:#4caf50}"
        "#st{width:8px;height:8px;border-radius:50%%;background:#555;display:inline-block;margin:0 6px}"
        "#st.on{background:#4caf50}"
        "</style></head><body>"
        "<h3>%s</h3>"
        "<div class=\"b\">"
        "<span id=\"st\"></span>"
        "<button id=\"bp\">暂停</button>"
        "<button id=\"bc\">清空</button>"
        "<span id=\"bi\"style=\"color:#666;margin-left:auto\"></span>"
        "</div>"
        "<textarea id=\"ta\"readonly></textarea>"
        "<script>"
        "var U=%d,ws,paused=0,O=0;"
        "function connect(){"
        "ws=new WebSocket('ws://'+location.host+'/ws'+U);"
        "ws.onopen=function(){document.getElementById('st').className='on'};"
        "ws.onclose=function(){document.getElementById('st').className='';"
        "setTimeout(connect,1000)};"
        "ws.onmessage=function(e){"
        "if(paused)return;"
        "var d=e.data,a=document.getElementById('ta'),"
        "at=a.scrollTop>=a.scrollHeight-a.clientHeight-16;"
        "a.value+=d;O+=d.length;"
        "if(at)a.scrollTop=a.scrollHeight;"
        "document.getElementById('bi').textContent=O+' bytes'}};"
        "document.getElementById('bc').onclick=function(){"
        "document.getElementById('ta').value='';O=0;"
        "if(ws&&ws.readyState===1)ws.send('clear')};"
        "document.getElementById('bp').onclick=function(){"
        "paused=!paused;var b=document.getElementById('bp');"
        "if(paused){b.textContent='继续';b.className='on';"
        "if(ws&&ws.readyState===1)ws.send('pause:1')}else{"
        "b.textContent='暂停';b.className='';"
        "if(ws&&ws.readyState===1)ws.send('pause:0')}};"
        "connect();"
        "</script></body></html>",
        idx + 1, br->name, idx);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, n);
    free(page);
    return ESP_OK;
}

/* ──────────────────── HTTP Server ──────────────────── */

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 16384;
    config.lru_purge_enable = true;

    printf("[WEB] starting...\n");
    ESP_ERROR_CHECK(httpd_start(&g_httpd, &config));

    static const httpd_uri_t uris[] = {
        { .uri = "/",     .method = HTTP_GET, .handler = root_handler },
        { .uri = "/page", .method = HTTP_GET, .handler = page_handler },
        { .uri = "/ws0",  .method = HTTP_GET, .handler = ws_handler,
          .is_websocket = true, .handle_ws_control_frames = true },
        { .uri = "/ws1",  .method = HTTP_GET, .handler = ws_handler,
          .is_websocket = true, .handle_ws_control_frames = true },
    };
    for (int i = 0; i < 4; i++) {
        httpd_register_uri_handler(g_httpd, &uris[i]);
    }
    printf("[WEB] ready (HTTP + WebSocket)\n");
}

/* ──────────────────── Init ──────────────────── */

static void uart_bridge_init(uart_bridge_t *br)
{
    br->paused = 0;
    br->tcp_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) br->tcp_fds[i] = -1;

    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(br->port, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(br->port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(br->port, br->tx_pin, br->rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(tcp_server_task, br->name, 8192, br, 5, NULL);
    xTaskCreate(uart_forward_task, br->name, 4096, br, 5, NULL);
    printf("[%s] ready: UART%d TX=IO%d RX=IO%d TCP=%d\n",
           br->name, br->port, br->tx_pin, br->rx_pin, br->tcp_port);
}

void app_main(void)
{
    static uart_bridge_t bridge1 = {
        .port = UART_NUM_1, .tx_pin = 2, .rx_pin = 4,
        .tcp_port = 8080, .name = "UART1",
    };
    static uart_bridge_t bridge2 = {
        .port = UART_NUM_2, .tx_pin = 16, .rx_pin = 17,
        .tcp_port = 8081, .name = "UART2",
    };
    g_bridges[0] = &bridge1;
    g_bridges[1] = &bridge2;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_softap();

    /* Broadcast queue + task */
    g_bcast_queue = xQueueCreate(BROADCAST_QUEUE_LEN, sizeof(bcast_item_t));
    xTaskCreate(ws_broadcast_task, "ws_bcast", 4096, NULL, 5, NULL);

    uart_bridge_init(&bridge1);
    uart_bridge_init(&bridge2);
    start_web_server();

    printf("\n=== Ready ===\n");
    printf(" WiFi: %s\n", WIFI_SSID);
    printf(" Web:  http://192.168.4.1/\n");
    printf(" UART1: http://192.168.4.1/page?uart=0  TCP :8080\n");
    printf(" UART2: http://192.168.4.1/page?uart=1  TCP :8081\n\n");
    fflush(stdout);
}
