/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
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
#include "lwip/sockets.h"

/* ──────────────────── Config ──────────────────── */

#define WIFI_SSID       "Embedded-debug-tool"
#define WIFI_CHANNEL    6
#define MAX_STA_CONN    5
#define MAX_CLIENTS     5
#define TCP_BUF_SIZE    1024
#define UART_BUF_SIZE   1024
#define UART_BAUD_RATE  115200
#define RING_BUF_SIZE   (16 * 1024)
#define WEB_FETCH_LIMIT (8 * 1024)

/* ──────────────────── Ring Buffer ──────────────────── */

typedef struct {
    uint8_t buf[RING_BUF_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
} ring_buf_t;

static inline void ring_buf_init(ring_buf_t *rb) { rb->head = rb->tail = 0; }

static inline uint32_t ring_buf_count(ring_buf_t *rb)
{
    return rb->head - rb->tail;
}

static void ring_buf_put(ring_buf_t *rb, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        rb->buf[(rb->head + i) & (RING_BUF_SIZE - 1)] = data[i];
    }
    rb->head += len;
    /* If overflowed, advance tail */
    if (rb->head - rb->tail > RING_BUF_SIZE) {
        rb->tail = rb->head - RING_BUF_SIZE;
    }
}

static uint32_t ring_buf_get(ring_buf_t *rb, uint8_t *out, uint32_t max_len, uint32_t since)
{
    uint32_t count = rb->head - since;
    if (count == 0) return 0;
    if (count > RING_BUF_SIZE) {
        since = rb->head - RING_BUF_SIZE;
        count = RING_BUF_SIZE;
    }
    if (count > max_len) count = max_len;
    uint32_t offset = since & (RING_BUF_SIZE - 1);
    uint32_t first = RING_BUF_SIZE - offset;
    if (first > count) first = count;
    memcpy(out, rb->buf + offset, first);
    if (first < count) {
        memcpy(out + first, rb->buf, count - first);
    }
    return count;
}

/* ──────────────────── Bridge Instance ──────────────────── */

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int tcp_port;
    const char *name;
    int client_fds[MAX_CLIENTS];
    SemaphoreHandle_t mutex;
    ring_buf_t ring;
    volatile int paused;
} uart_bridge_t;

/* ──────────────────── WiFi AP ──────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        printf("[WIFI] station " MACSTR " joined, AID=%d\n", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        printf("[WIFI] station " MACSTR " left, AID=%d\n", MAC2STR(event->mac), event->aid);
    }
}

static void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
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
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("[WIFI] AP started: SSID=%s\n", WIFI_SSID);
}

/* ──────────────────── TCP Server ──────────────────── */

static void add_client(uart_bridge_t *br, int fd)
{
    xSemaphoreTake(br->mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (br->client_fds[i] == -1) {
            br->client_fds[i] = fd;
            printf("[%s] TCP client connected fd=%d\n", br->name, fd);
            xSemaphoreGive(br->mutex);
            return;
        }
    }
    close(fd);
    xSemaphoreGive(br->mutex);
    printf("[%s] TCP client rejected (full)\n", br->name);
}

static void broadcast_to_clients(uart_bridge_t *br, const uint8_t *data, int len)
{
    xSemaphoreTake(br->mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (br->client_fds[i] != -1) {
            if (send(br->client_fds[i], data, len, 0) < 0) {
                close(br->client_fds[i]);
                br->client_fds[i] = -1;
            }
        }
    }
    xSemaphoreGive(br->mutex);
}

static void tcp_server_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { vTaskDelete(NULL); return; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(br->tcp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server_fd); vTaskDelete(NULL); return;
    }
    listen(server_fd, MAX_CLIENTS);
    printf("[%s] TCP server on port %d\n", br->name, br->tcp_port);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        int maxfd = server_fd;

        xSemaphoreTake(br->mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (br->client_fds[i] != -1) {
                FD_SET(br->client_fds[i], &rfds);
                if (br->client_fds[i] > maxfd) maxfd = br->client_fds[i];
            }
        }
        xSemaphoreGive(br->mutex);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) < 0) continue;

        if (FD_ISSET(server_fd, &rfds)) {
            struct sockaddr_in ca; socklen_t cl = sizeof(ca);
            int fd = accept(server_fd, (struct sockaddr *)&ca, &cl);
            if (fd >= 0) add_client(br, fd);
        }

        xSemaphoreTake(br->mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = br->client_fds[i];
            if (fd != -1 && FD_ISSET(fd, &rfds)) {
                uint8_t buf[TCP_BUF_SIZE];
                int n = recv(fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    uart_write_bytes(br->port, buf, n);
                } else {
                    close(fd); br->client_fds[i] = -1;
                }
            }
        }
        xSemaphoreGive(br->mutex);
    }
}

/* ──────────────────── UART Forward ──────────────────── */

static void uart_forward_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    uint8_t buf[UART_BUF_SIZE];
    int log_cnt = 0;

    printf("[%s] forward task running\n", br->name);

    while (1) {
        int len = uart_read_bytes(br->port, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        printf("[%s] RX %d bytes, paused=%d\n", br->name, len, br->paused);

        /* Ring buffer for web */
        if (!br->paused) {
            ring_buf_put(&br->ring, buf, len);
        }

        /* TCP broadcast */
        broadcast_to_clients(br, buf, len);
    }
}

/* ──────────────────── Web Server ──────────────────── */

static uart_bridge_t *g_bridges[2];
static httpd_handle_t g_server;

static void send_html_page(httpd_req_t *req, int uart_idx, uart_bridge_t *br)
{
    /* Prevent browser caching */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    char page[2048];
    int off = 0;

    off += snprintf(page + off, sizeof(page) - off,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>UART%d</title><style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font:13px/1.3 monospace;background:#1a1a1a;color:#ccc;padding:8px}"
        "h3{color:#4fc3f7;margin-bottom:6px}"
        "textarea{width:100%%;height:60vh;background:#111;color:#0f0;border:1px solid #333;"
        "font:12px/1.4 monospace;resize:vertical;padding:4px}"
        ".bar{display:flex;gap:6px;margin:6px 0;align-items:center;flex-wrap:wrap}"
        "button{padding:4px 12px;border:1px solid #555;background:#222;color:#ccc;"
        "cursor:pointer;font:12px monospace;border-radius:3px}"
        "button:hover{background:#333}"
        "button.on{border-color:#4caf50;color:#4caf50}"
        "</style></head><body>"
        "<h3>UART%d - %s</h3>"
        "<div class=\"bar\">"
        "<button id=\"btnP\">暂停</button>"
        "<button id=\"btnC\">清空</button>"
        "<span id=\"inf\" style=\"color:#666;margin-left:auto\"></span>"
        "</div>"
        "<textarea id=\"ta\" readonly></textarea>"
        "<input type=\"hidden\" id=\"uid\" value=\"%d\">",
        br->tcp_port, uart_idx, br->name, uart_idx);

    off += snprintf(page + off, sizeof(page) - off,
        "<script>"
        "var uid=document.getElementById('uid').value,"
        "paused=0,ofs=0,tid;"
        "document.getElementById('btnC').onclick=function(){"
        "document.getElementById('ta').value='';ofs=0};"
        "document.getElementById('btnP').onclick=function(){"
        "var b=document.getElementById('btnP');"
        "paused=!paused;"
        "if(paused){b.textContent='继续';b.className='on';"
        "fetch('/ctrl?uart='+uid+'&pause=1')}else{"
        "b.textContent='暂停';b.className='off';"
        "fetch('/ctrl?uart='+uid+'&pause=0')}"
        "};"
        "function poll(){if(!paused){"
        "fetch('/data?uart='+uid+'&since='+ofs)"
        ".then(function(r){if(r.status===204)return null;return r.text()})"
        ".then(function(t){if(t&&t.length>0){"
        "var a=document.getElementById('ta');"
        "var bot=a.scrollTop>=a.scrollHeight-a.clientHeight-16;"
        "a.value+=t;ofs+=t.length;"
        "if(bot)a.scrollTop=a.scrollHeight;"
        "document.getElementById('inf').textContent=ofs+' bytes';"
        "}})"
        ".catch(function(){});}"
        "tid=setTimeout(poll,200);}"
        "tid=setTimeout(poll,200);"
        "</script></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, off);
}

/* GET /page?uart=0 or /page?uart=1 */
static esp_err_t page_handler(httpd_req_t *req)
{
    char qbuf[16];
    int uart_idx = 0;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[4];
        if (httpd_query_key_value(qbuf, "uart", val, sizeof(val)) == ESP_OK) {
            uart_idx = atoi(val);
        }
    }
    if (uart_idx < 0 || uart_idx > 1) uart_idx = 0;
    send_html_page(req, uart_idx, g_bridges[uart_idx]);
    return ESP_OK;
}

/* GET /data?uart=0&since=N */
static esp_err_t data_handler(httpd_req_t *req)
{
    char qbuf[32];
    int uart_idx = 0;
    uint32_t since = 0;

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qbuf, "uart", val, sizeof(val)) == ESP_OK) {
            uart_idx = atoi(val);
        }
        if (httpd_query_key_value(qbuf, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }
    if (uart_idx < 0 || uart_idx > 1) uart_idx = 0;

    uart_bridge_t *br = g_bridges[uart_idx];
    uint8_t tmp[WEB_FETCH_LIMIT];
    uint32_t n = ring_buf_get(&br->ring, tmp, sizeof(tmp), since);

    if (n == 0) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    printf("[WEB] data uart=%d since=%lu → %lu bytes\n", uart_idx, (unsigned long)since, (unsigned long)n);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, (const char *)tmp, n);
    return ESP_OK;
}

/* GET /ctrl?uart=0&pause=1 */
static esp_err_t ctrl_handler(httpd_req_t *req)
{
    char qbuf[32];
    int uart_idx = 0;
    int pause = -1;

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[4];
        if (httpd_query_key_value(qbuf, "uart", val, sizeof(val)) == ESP_OK) {
            uart_idx = atoi(val);
        }
        if (httpd_query_key_value(qbuf, "pause", val, sizeof(val)) == ESP_OK) {
            pause = atoi(val);
        }
    }
    if (uart_idx < 0 || uart_idx > 1) uart_idx = 0;

    if (pause >= 0) {
        g_bridges[uart_idx]->paused = pause;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

/* GET / (index page) */
static esp_err_t root_handler(httpd_req_t *req)
{
    printf("[WEB] GET / from client\n");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    const char *html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>Embedded Debug Tool</title>"
        "<style>*{margin:0;padding:0}body{font:14px/1.5 monospace;background:#1a1a1a;"
        "color:#ccc;padding:20px}a{color:#4fc3f7;text-decoration:none}"
        "a:hover{text-decoration:underline}h2{color:#4fc3f7;margin-bottom:12px}"
        "li{margin:6px 0}</style></head><body>"
        "<h2>Embedded Debug Tool</h2><ul>"
        "<li><a href=\"/page?uart=0\">UART1 (IO2/IO4) - TCP 8080</a></li>"
        "<li><a href=\"/page?uart=1\">UART2 (IO16/IO17) - TCP 8081</a></li>"
        "</ul></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;
    config.backlog_conn = 7;

    printf("[WEB] starting server on port %d...\n", config.server_port);
    esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        printf("[WEB] server start FAILED: %s\n", esp_err_to_name(err));
        return;
    }
    printf("[WEB] server handle: %p\n", g_server);

    httpd_uri_t uris[] = {
        { .uri = "/",      .method = HTTP_GET, .handler = root_handler },
        { .uri = "/page",  .method = HTTP_GET, .handler = page_handler },
        { .uri = "/data",  .method = HTTP_GET, .handler = data_handler },
        { .uri = "/ctrl",  .method = HTTP_GET, .handler = ctrl_handler },
    };
    for (int i = 0; i < 4; i++) {
        httpd_register_uri_handler(g_server, &uris[i]);
    }
    printf("[WEB] server started on port 80\n");
}

/* ──────────────────── Init ──────────────────── */

static void uart_bridge_init(uart_bridge_t *br)
{
    printf("[%s] init start...\n", br->name);

    ring_buf_init(&br->ring);
    br->mutex = xSemaphoreCreateMutex();
    br->paused = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) br->client_fds[i] = -1;

    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err;
    err = uart_driver_install(br->port, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) { printf("[%s] driver_install FAIL: %s\n", br->name, esp_err_to_name(err)); return; }
    printf("[%s] driver installed\n", br->name);

    err = uart_param_config(br->port, &uart_config);
    if (err != ESP_OK) { printf("[%s] param_config FAIL: %s\n", br->name, esp_err_to_name(err)); return; }
    printf("[%s] param configured\n", br->name);

    err = uart_set_pin(br->port, br->tx_pin, br->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) { printf("[%s] set_pin FAIL: %s\n", br->name, esp_err_to_name(err)); return; }
    printf("[%s] pins set TX=%d RX=%d\n", br->name, br->tx_pin, br->rx_pin);

    xTaskCreate(tcp_server_task, br->name, 4096, br, 5, NULL);
    xTaskCreate(uart_forward_task, br->name, 4096, br, 5, NULL);
    printf("[%s] init done → TCP port %d\n", br->name, br->tcp_port);
}

/* ──────────────────── App Main ──────────────────── */

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

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Networking */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* WiFi AP */
    printf("[INIT] starting WiFi AP...\n");
    wifi_init_softap();

    /* UART bridges */
    printf("[INIT] starting UART bridges...\n");
    uart_bridge_init(&bridge1);
    uart_bridge_init(&bridge2);

    /* Web server */
    printf("[INIT] starting web server...\n");
    start_web_server();

    printf("\n========================================\n");
    printf(" WiFi: %s (no password)\n", WIFI_SSID);
    printf(" Web:  http://192.168.4.1/\n");
    printf(" UART1: http://192.168.4.1/page?uart=0  TCP 192.168.4.1:8080\n");
    printf(" UART2: http://192.168.4.1/page?uart=1  TCP 192.168.4.1:8081\n");
    printf("========================================\n\n");
    fflush(stdout);
}
