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
#include "nvs_flash.h"
#include "driver/uart.h"
#include "lwip/sockets.h"

/* ──────────────────────── Config ──────────────────────── */

#define WIFI_SSID       "Embedded-debug-tool"
#define WIFI_CHANNEL    6
#define MAX_STA_CONN    5
#define MAX_CLIENTS     5
#define TCP_BUF_SIZE    1024
#define UART_BUF_SIZE   1024
#define UART_BAUD_RATE  115200

/* Per-UART bridge instance */
typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int tcp_port;
    const char *name;
    int client_fds[MAX_CLIENTS];
    SemaphoreHandle_t mutex;
} uart_bridge_t;

/* ──────────────────────── WiFi AP ──────────────────────── */

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

    printf("[WIFI] AP started: SSID=%s, channel=%d\n", WIFI_SSID, WIFI_CHANNEL);
}

/* ──────────────────────── TCP Server ──────────────────────── */

static void add_client(uart_bridge_t *br, int fd)
{
    xSemaphoreTake(br->mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (br->client_fds[i] == -1) {
            br->client_fds[i] = fd;
            printf("[%s] client connected, fd=%d\n", br->name, fd);
            xSemaphoreGive(br->mutex);
            return;
        }
    }
    close(fd);
    xSemaphoreGive(br->mutex);
    printf("[%s] client rejected (full), fd=%d\n", br->name, fd);
}

static void broadcast_to_clients(uart_bridge_t *br, const uint8_t *data, int len)
{
    xSemaphoreTake(br->mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (br->client_fds[i] != -1) {
            int sent = send(br->client_fds[i], data, len, 0);
            if (sent < 0) {
                close(br->client_fds[i]);
                br->client_fds[i] = -1;
                printf("[%s] send failed, removed fd=%d\n", br->name, br->client_fds[i]);
            }
        }
    }
    xSemaphoreGive(br->mutex);
}

static void tcp_server_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;

    int server_fd;
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(br->tcp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("[%s] socket create failed\n", br->name);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("[%s] bind failed on port %d\n", br->name, br->tcp_port);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        printf("[%s] listen failed\n", br->name);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    printf("[%s] TCP server listening on port %d\n", br->name, br->tcp_port);

    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        xSemaphoreTake(br->mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (br->client_fds[i] != -1) {
                FD_SET(br->client_fds[i], &read_fds);
                if (br->client_fds[i] > max_fd) {
                    max_fd = br->client_fds[i];
                }
            }
        }
        xSemaphoreGive(br->mutex);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            continue;
        }

        /* New connection */
        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (new_fd >= 0) {
                add_client(br, new_fd);
            }
        }

        /* TCP client data → UART TX */
        xSemaphoreTake(br->mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = br->client_fds[i];
            if (fd != -1 && FD_ISSET(fd, &read_fds)) {
                uint8_t buf[TCP_BUF_SIZE];
                int len = recv(fd, buf, sizeof(buf), 0);
                if (len > 0) {
                    uart_write_bytes(br->port, buf, len);
                } else {
                    close(fd);
                    br->client_fds[i] = -1;
                    printf("[%s] client disconnected, fd=%d\n", br->name, fd);
                }
            }
        }
        xSemaphoreGive(br->mutex);
    }
}

/* ──────────────────────── UART Forward ──────────────────────── */

static void uart_forward_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    uint8_t buf[UART_BUF_SIZE];

    printf("[%s] forward started: TX=IO%d, RX=IO%d\n", br->name, br->tx_pin, br->rx_pin);

    while (1) {
        int len = uart_read_bytes(br->port, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            broadcast_to_clients(br, buf, len);
        }
    }
}

/* ──────────────────────── Init ──────────────────────── */

static void uart_bridge_init(uart_bridge_t *br)
{
    /* Init clients */
    br->mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        br->client_fds[i] = -1;
    }

    /* Configure UART */
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(br->port, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(br->port, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(br->port, br->tx_pin, br->rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    printf("[%s] UART%d: TX=IO%d, RX=IO%d, baud=%d\n",
           br->name, br->port, br->tx_pin, br->rx_pin, UART_BAUD_RATE);

    /* Start tasks */
    xTaskCreate(tcp_server_task, br->name, 4096, br, 5, NULL);
    xTaskCreate(uart_forward_task, br->name, 4096, br, 5, NULL);
}

/* ──────────────────────── App Main ──────────────────────── */

void app_main(void)
{
    /* UART1: IO2(TX) IO4(RX) → TCP 8080 */
    static uart_bridge_t bridge1 = {
        .port = UART_NUM_1,
        .tx_pin = 2,
        .rx_pin = 4,
        .tcp_port = 8080,
        .name = "UART1",
    };

    /* UART2: IO16(TX) IO17(RX) → TCP 8081 */
    static uart_bridge_t bridge2 = {
        .port = UART_NUM_2,
        .tx_pin = 16,
        .rx_pin = 17,
        .tcp_port = 8081,
        .name = "UART2",
    };

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
    wifi_init_softap();

    /* Init bridges */
    uart_bridge_init(&bridge1);
    uart_bridge_init(&bridge2);

    printf("[APP] Bridge ready. WiFi '%s' → 192.168.4.1\n", WIFI_SSID);
    printf("[APP] UART1 (IO2/IO4) → TCP port %d\n", bridge1.tcp_port);
    printf("[APP] UART2 (IO16/IO17) → TCP port %d\n", bridge2.tcp_port);
}
