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
    /* Send config */
    volatile int send_newline;
    volatile int send_hex;
    volatile int timer_on;
    volatile int timer_ms;
    TimerHandle_t send_timer;
    char send_buf[256];
    int send_len;
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
                /* Silently ignore send errors (client may have disconnected) */
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

/* ──────────────────── Send Helpers ──────────────────── */

static int hex_char_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int max_len)
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
    if (br->send_len > 0) {
        uart_write_bytes(br->port, br->send_buf, br->send_len);
    }
}

static void do_send(uart_bridge_t *br, const char *data, int len)
{
    uint8_t buf[256];
    int n;

    if (br->send_hex) {
        n = hex_to_bytes(data, buf, sizeof(buf));
    } else {
        n = (len < (int)sizeof(buf)) ? len : (int)sizeof(buf);
        memcpy(buf, data, n);
    }

    if (br->send_newline && n + 2 <= (int)sizeof(buf)) {
        buf[n++] = '\r';
        buf[n++] = '\n';
    }

    /* Store for timer re-send */
    memcpy(br->send_buf, buf, n);
    br->send_len = n;

    uart_write_bytes(br->port, buf, n);
    printf("[%s] TX %d bytes\n", br->name, n);
}

/* ──────────────────── WebSocket Handler ──────────────────── */

static uart_bridge_t *g_bridges[2];

static void ws_send_status(httpd_handle_t hd, int fd, uart_bridge_t *br)
{
    char status[64];
    int sn = snprintf(status, sizeof(status), "S:%d,%d,%d",
                      br->timer_on, br->timer_ms, br->paused);
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)status,
        .len = sn, .final = true
    };
    httpd_ws_send_frame_async(hd, fd, &frame);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int idx = (req->uri[4] == '1') ? 1 : 0;
        printf("[WS] connected fd=%d uart%d\n", httpd_req_to_sockfd(req), idx + 1);
        ws_send_status(g_httpd, httpd_req_to_sockfd(req), g_bridges[idx]);
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
        int fd = httpd_req_to_sockfd(req);
        char *msg = (char *)buf;

        if (strncmp(msg, "pause:", 6) == 0) {
            br->paused = atoi(msg + 6);
        } else if (strcmp(msg, "clear") == 0) {
            /* No-op */
        } else if (strncmp(msg, "send:", 5) == 0) {
            do_send(br, msg + 5, frame.len - 5);
        } else if (strncmp(msg, "sendh:", 6) == 0) {
            uint8_t bin[256];
            int n = hex_to_bytes(msg + 6, bin, sizeof(bin));
            if (br->send_newline && n + 2 <= (int)sizeof(bin)) {
                bin[n++] = '\r'; bin[n++] = '\n';
            }
            memcpy(br->send_buf, bin, n);
            br->send_len = n;
            uart_write_bytes(br->port, bin, n);
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
        /* Confirm state back to client */
        ws_send_status(g_httpd, fd, br);
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
        "<style>body{font-family:-apple-system,system-ui,sans-serif;background:#fff;"
        "color:#111;padding:40px 24px}h1{font-size:18px;font-weight:600;margin-bottom:24px}"
        "a{display:block;padding:14px 16px;border:1px solid #e5e5e5;border-radius:8px;"
        "text-decoration:none;color:#111;margin-bottom:10px;font-size:14px}"
        "a:hover{background:#f9fafb;border-color:#9ca3af}"
        ".sub{color:#9ca3af;font-size:12px;margin-top:4px}</style></head><body>"
        "<h1>Embedded Debug Tool</h1>"
        "<a href=\"/page?uart=0\">UART1<span class=\"sub\">IO2 / IO4 &middot; TCP :8080</span></a>"
        "<a href=\"/page?uart=1\">UART2<span class=\"sub\">IO16 / IO17 &middot; TCP :8081</span></a>"
        "</body></html>";
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

    char *page = malloc(5120);
    if (!page) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    int n = snprintf(page, 5120,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>%s</title><style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:-apple-system,system-ui,sans-serif;background:#fff;color:#111;"
        "display:flex;flex-direction:column;height:100vh;padding:12px 16px}"
        "header{display:flex;align-items:center;gap:12px;padding-bottom:10px;"
        "border-bottom:1px solid #e5e5e5}"
        "h1{font-size:15px;font-weight:600;flex:1}"
        ".dot{width:7px;height:7px;border-radius:50%%;background:#ddd}"
        ".dot.on{background:#22c55e}"
        "button{padding:5px 14px;border:1px solid #d1d5d5;background:#fff;color:#374151;"
        "border-radius:6px;font-size:12px;cursor:pointer;font-family:inherit}"
        "button:hover{background:#f9fafb;border-color:#9ca3af}"
        "button.active{background:#f0fdf4;border-color:#22c55e;color:#166534}"
        "#log{flex:1;width:100%%;margin-top:10px;border:1px solid #e5e5e5;border-radius:8px;"
        "padding:10px;font-family:'SF Mono',Menlo,Consolas,monospace;font-size:12px;"
        "line-height:1.5;resize:none;outline:none;background:#fafafa;color:#111}"
        "#log:focus{border-color:#9ca3af}"
        ".info{font-size:11px;color:#9ca3af;text-align:right;padding-top:8px}"
        "#snd{display:flex;gap:6px;align-items:center;padding-top:8px;border-top:1px solid #e5e5e5;margin-top:4px}"
        "#si{flex:1;padding:6px 10px;border:1px solid #d1d5d5;border-radius:6px;font-size:13px;outline:none;font-family:inherit}"
        "#si:focus{border-color:#9ca3af}"
        "#snd label{font-size:12px;color:#374151;display:flex;align-items:center;gap:3px;white-space:nowrap}"
        "#ti{width:65px;padding:5px 6px;border:1px solid #d1d5d5;border-radius:6px;font-size:12px;outline:none;text-align:center}"
        "#sb{padding:6px 16px;border:none;background:#111;color:#fff;border-radius:6px;font-size:12px;cursor:pointer;font-family:inherit}"
        "#sb:hover{background:#333}"
        "</style></head><body>"
        "<header>"
        "<span class=\"dot\" id=\"st\"></span>"
        "<h1>%s</h1>"
        "<button id=\"bp\">暂停</button>"
        "<button id=\"bc\">清空</button>"
        "</header>"
        "<textarea id=\"log\" readonly spellcheck=\"false\"></textarea>"
        "<div class=\"info\" id=\"bi\">RX: 0 &nbsp; TX: 0</div>"
        "<div id=\"snd\">"
        "<input type=\"text\" id=\"si\" placeholder=\"输入数据...\" autocomplete=\"off\">"
        "<label><input type=\"checkbox\" id=\"nl\"> 回车</label>"
        "<label><input type=\"checkbox\" id=\"hx\"> HEX</label>"
        "<input type=\"number\" id=\"ti\" value=\"1000\" min=\"100\" style=\"width:70px\">"
        "<label><input type=\"checkbox\" id=\"te\"> 定时</label>"
        "<button id=\"sb\">发送</button>"
        "</div>"
        "<script>"
        "var U=%d,ws,paused=0,RX=0,TX=0;"
        "function wsSend(m){if(ws&&ws.readyState===1)ws.send(m)}"
        "function updInfo(){document.getElementById('bi').textContent='RX: '+RX+'  TX: '+TX}"
        "function connect(){"
        "ws=new WebSocket('ws://'+location.host+'/ws'+U);"
        "ws.onopen=function(){document.getElementById('st').className='dot on'};"
        "ws.onclose=function(){document.getElementById('st').className='dot';"
        "setTimeout(connect,1000)};"
        "ws.onmessage=function(e){"
        "var m=e.data;"
        "if(m.charAt(0)==='S'){"
        "var p=m.substring(2).split(',');"
        "var te=document.getElementById('te'),ti=document.getElementById('ti'),bp=document.getElementById('bp');"
        "te.checked=parseInt(p[0]);ti.value=p[1];"
        "paused=parseInt(p[2]);"
        "bp.textContent=paused?'继续':'暂停';bp.className=paused?'active':'';"
        "return}"
        "if(paused)return;"
        "var t=document.getElementById('log'),"
        "at=t.scrollTop>=t.scrollHeight-t.clientHeight-10;"
        "t.value+=m;RX+=m.length;"
        "if(at)t.scrollTop=t.scrollHeight;"
        "updInfo()}};"
        "function doSend(){"
        "var d=document.getElementById('si').value;"
        "if(!d)return;"
        "var hx=document.getElementById('hx').checked;"
        "wsSend(hx?'sendh:'+d:'send:'+d);"
        "TX+=hx?Math.ceil(d.length/2):d.length;"
        "updInfo()}"
        "document.getElementById('sb').onclick=doSend;"
        "document.getElementById('si').onkeydown=function(e){if(e.key==='Enter')doSend()};"
        "document.getElementById('nl').onchange=function(){"
        "wsSend('cfg:'+(this.checked?1:0)+','+(document.getElementById('hx').checked?1:0))};"
        "document.getElementById('hx').onchange=function(){"
        "wsSend('cfg:'+(document.getElementById('nl').checked?1:0)+','+(this.checked?1:0))};"
        "document.getElementById('ti').onchange=function(){"
        "wsSend('tconf:'+this.value)};"
        "document.getElementById('te').onchange=function(){"
        "wsSend('timer:'+(this.checked?1:0));"
        "wsSend('tconf:'+document.getElementById('ti').value)};"
        "document.getElementById('bc').onclick=function(){"
        "document.getElementById('log').value='';RX=0;TX=0;"
        "updInfo();wsSend('clear')};"
        "document.getElementById('bp').onclick=function(){"
        "paused=!paused;var b=document.getElementById('bp');"
        "if(paused){b.textContent='继续';b.className='active';"
        "wsSend('pause:1')}else{"
        "b.textContent='暂停';b.className='';"
        "wsSend('pause:0')}};"
        "connect();"
        "</script></body></html>",
        br->name, br->name, idx);

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
    br->send_newline = 0;
    br->send_hex = 0;
    br->timer_on = 0;
    br->timer_ms = 1000;
    br->send_len = 0;
    br->tcp_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) br->tcp_fds[i] = -1;

    /* Periodic send timer */
    br->send_timer = xTimerCreate("uart_tx", pdMS_TO_TICKS(1000),
                                   pdTRUE, (void *)br, send_timer_cb);

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
