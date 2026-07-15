/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * UART-to-HTTP bridge: UART1(UART2) data viewed via web browser.
 * Architecture: HTTP server only (no separate TCP server) to avoid
 * lwIP netconn/socket API conflict crash.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#define UART_BUF_SIZE   1024
#define UART_BAUD_RATE  115200
#define RING_BUF_SIZE   (16 * 1024)
#define WEB_FETCH_LIMIT (8 * 1024)

/* ──────────────────── Ring Buffer ──────────────────── */

typedef struct {
    uint8_t buf[RING_BUF_SIZE];
    volatile uint32_t head;
} ring_buf_t;

static void ring_buf_init(ring_buf_t *rb) { rb->head = 0; }

static void ring_buf_put(ring_buf_t *rb, const uint8_t *data, uint32_t len)
{
    uint32_t h = rb->head;
    for (uint32_t i = 0; i < len; i++) {
        rb->buf[(h + i) & (RING_BUF_SIZE - 1)] = data[i];
    }
    rb->head = h + len;
}

/* Returns bytes written to out. since=0 means "from oldest available". */
static uint32_t ring_buf_get(ring_buf_t *rb, uint8_t *out, uint32_t max_len, uint32_t since)
{
    uint32_t h = rb->head;
    if (h == since) return 0;

    /* If client is too far behind, give from oldest */
    uint32_t available = h - since;
    if (available > RING_BUF_SIZE) {
        since = h - RING_BUF_SIZE;
        available = RING_BUF_SIZE;
    }

    uint32_t len = (available > max_len) ? max_len : available;
    uint32_t offset = since & (RING_BUF_SIZE - 1);
    uint32_t first = RING_BUF_SIZE - offset;
    if (first > len) first = len;

    memcpy(out, rb->buf + offset, first);
    if (first < len) {
        memcpy(out, rb->buf, len - first);
    }
    return len;
}

/* ──────────────────── UART Bridge ──────────────────── */

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    const char *name;
    ring_buf_t ring;
    volatile int paused;
} uart_bridge_t;

static void uart_forward_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    uint8_t buf[UART_BUF_SIZE];

    printf("[%s] forward task running: TX=IO%d RX=IO%d\n", br->name, br->tx_pin, br->rx_pin);

    while (1) {
        int len = uart_read_bytes(br->port, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        if (!br->paused) {
            ring_buf_put(&br->ring, buf, len);
        }
    }
}

static void uart_bridge_init(uart_bridge_t *br)
{
    ring_buf_init(&br->ring);
    br->paused = 0;

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

    xTaskCreate(uart_forward_task, br->name, 4096, br, 5, NULL);
    printf("[%s] UART%d ready: TX=IO%d RX=IO%d\n", br->name, br->port, br->tx_pin, br->rx_pin);
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
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = { .required = true },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("[WIFI] AP: %s ch%d\n", WIFI_SSID, WIFI_CHANNEL);
}

/* ──────────────────── HTTP Handlers ──────────────────── */

static uart_bridge_t *g_bridges[2];
static httpd_handle_t g_server;

static void set_no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
}

/* GET / */
static esp_err_t root_handler(httpd_req_t *req)
{
    set_no_cache(req);
    const char *html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>Embedded Debug Tool</title>"
        "<style>body{font:14px monospace;background:#1a1a1a;color:#ccc;padding:20px}"
        "a{color:#4fc3f7}li{margin:8px 0}</style></head><body>"
        "<h2 style=\"color:#4fc3f7\">Embedded Debug Tool</h2><ul>"
        "<li><a href=\"/page?uart=0\">UART1 (IO2/IO4)</a></li>"
        "<li><a href=\"/page?uart=1\">UART2 (IO16/IO17)</a></li>"
        "</ul></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

/* GET /page?uart=N */
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

    char page[2048];
    int n = snprintf(page, sizeof(page),
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
        "</style></head><body>"
        "<h3>UART%d - %s</h3>"
        "<div class=\"b\">"
        "<button id=\"bp\">暂停</button>"
        "<button id=\"bc\">清空</button>"
        "<span id=\"bi\"style=\"color:#666;margin-left:auto\"></span>"
        "</div>"
        "<textarea id=\"ta\"readonly></textarea>"
        "<input type=\"hidden\"id=\"uid\"value=\"%d\">"
        "<script>"
        "var U=document.getElementById('uid').value,"
        "P=0,O=0,T;"
        "document.getElementById('bc').onclick=function(){"
        "document.getElementById('ta').value='';O=0};"
        "document.getElementById('bp').onclick=function(){"
        "P=!P;var b=document.getElementById('bp');"
        "if(P){b.textContent='继续';b.className='on';"
        "fetch('/ctrl?uart='+U+'&pause=1')}else{"
        "b.textContent='暂停';b.className='';"
        "fetch('/ctrl?uart='+U+'&pause=0')}};"
        "function poll(){if(!P){"
        "fetch('/data?uart='+U+'&since='+O)"
        ".then(function(r){if(r.status===204)return null;return r.text()})"
        ".then(function(t){if(t&&t.length>0){"
        "var a=document.getElementById('ta'),"
        "at=a.scrollTop>=a.scrollHeight-a.clientHeight-16;"
        "a.value+=t;O+=t.length;"
        "if(at)a.scrollTop=a.scrollHeight;"
        "document.getElementById('bi').textContent=O+' bytes'}})"
        ".catch(function(){});}"
        "T=setTimeout(poll,200)}T=setTimeout(poll,200);"
        "</script></body></html>",
        idx, idx, br->name, idx);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, n);
}

/* GET /data?uart=N&since=M */
static esp_err_t data_handler(httpd_req_t *req)
{
    char qbuf[32] = {0};
    int idx = 0;
    uint32_t since = 0;

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(qbuf, "uart", v, sizeof(v)) == ESP_OK) idx = atoi(v);
        if (httpd_query_key_value(qbuf, "since", v, sizeof(v)) == ESP_OK) since = strtoul(v, NULL, 10);
    }
    if (idx < 0 || idx > 1) idx = 0;

    uart_bridge_t *br = g_bridges[idx];
    uint8_t tmp[WEB_FETCH_LIMIT];
    uint32_t n = ring_buf_get(&br->ring, tmp, sizeof(tmp), since);

    if (n == 0) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, (const char *)tmp, n);
}

/* GET /ctrl?uart=N&pause=M */
static esp_err_t ctrl_handler(httpd_req_t *req)
{
    char qbuf[32] = {0};
    int idx = 0;
    int pause = -1;

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char v[4];
        if (httpd_query_key_value(qbuf, "uart", v, sizeof(v)) == ESP_OK) idx = atoi(v);
        if (httpd_query_key_value(qbuf, "pause", v, sizeof(v)) == ESP_OK) pause = atoi(v);
    }
    if (idx < 0 || idx > 1) idx = 0;
    if (pause >= 0) g_bridges[idx]->paused = pause;

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", 2);
}

/* ──────────────────── HTTP Server ──────────────────── */

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    printf("[WEB] starting...\n");
    ESP_ERROR_CHECK(httpd_start(&g_server, &config));

    static const httpd_uri_t uris[] = {
        { .uri = "/",     .method = HTTP_GET, .handler = root_handler },
        { .uri = "/page", .method = HTTP_GET, .handler = page_handler },
        { .uri = "/data", .method = HTTP_GET, .handler = data_handler },
        { .uri = "/ctrl", .method = HTTP_GET, .handler = ctrl_handler },
    };
    for (int i = 0; i < 4; i++) {
        httpd_register_uri_handler(g_server, &uris[i]);
    }
    printf("[WEB] ready on port 80\n");
}

/* ──────────────────── App Main ──────────────────── */

void app_main(void)
{
    static uart_bridge_t bridge1 = {
        .port = UART_NUM_1, .tx_pin = 2, .rx_pin = 4, .name = "UART1",
    };
    static uart_bridge_t bridge2 = {
        .port = UART_NUM_2, .tx_pin = 16, .rx_pin = 17, .name = "UART2",
    };
    g_bridges[0] = &bridge1;
    g_bridges[1] = &bridge2;

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Network */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* WiFi */
    wifi_init_softap();

    /* UARTs */
    uart_bridge_init(&bridge1);
    uart_bridge_init(&bridge2);

    /* HTTP */
    start_web_server();

    printf("\n=== Ready ===\n");
    printf(" WiFi: %s\n", WIFI_SSID);
    printf(" Web:  http://192.168.4.1/\n");
    printf(" UART1: http://192.168.4.1/page?uart=0\n");
    printf(" UART2: http://192.168.4.1/page?uart=1\n\n");
    fflush(stdout);
}
