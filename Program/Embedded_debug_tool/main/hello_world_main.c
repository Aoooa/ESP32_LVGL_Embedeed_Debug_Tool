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
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "lwip/sockets.h"

/* UART1 config */
#define UART_PORT       UART_NUM_1
#define UART_TX_PIN     2
#define UART_RX_PIN     4
#define UART_BAUD_RATE  115200
#define UART_BUF_SIZE   1024

/* WiFi AP config */
#define WIFI_SSID       "Embedded-debug-tool"
#define WIFI_CHANNEL    6
#define MAX_STA_CONN    5

/* TCP server config */
#define TCP_PORT        8080
#define MAX_CLIENTS     5
#define TCP_BUF_SIZE    1024

/* Client tracking */
static int client_fds[MAX_CLIENTS];
static SemaphoreHandle_t client_mutex;

/* ──────────────────────────── WiFi AP ──────────────────────────── */

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

/* ──────────────────────────── TCP Server ──────────────────────────── */

static void remove_client(int fd)
{
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] == fd) {
            close(fd);
            client_fds[i] = -1;
            printf("[TCP] client disconnected, fd=%d\n", fd);
            break;
        }
    }
    xSemaphoreGive(client_mutex);
}

static void add_client(int fd)
{
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] == -1) {
            client_fds[i] = fd;
            printf("[TCP] client connected, fd=%d\n", fd);
            xSemaphoreGive(client_mutex);
            return;
        }
    }
    /* Full — reject */
    close(fd);
    xSemaphoreGive(client_mutex);
    printf("[TCP] client rejected (full), fd=%d\n", fd);
}

static void broadcast_to_clients(const uint8_t *data, int len)
{
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] != -1) {
            int sent = send(client_fds[i], data, len, 0);
            if (sent < 0) {
                close(client_fds[i]);
                client_fds[i] = -1;
                printf("[TCP] send failed, removed fd=%d\n", client_fds[i]);
            }
        }
    }
    xSemaphoreGive(client_mutex);
}

static void tcp_server_task(void *arg)
{
    int server_fd;
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("[TCP] socket create failed\n");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("[TCP] bind failed\n");
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        printf("[TCP] listen failed\n");
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    printf("[TCP] server listening on port %d\n", TCP_PORT);

    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        xSemaphoreTake(client_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] != -1) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }
        xSemaphoreGive(client_mutex);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            continue;
        }

        /* Check for new connections */
        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (new_fd >= 0) {
                add_client(new_fd);
            }
        }

        /* Check for data from TCP clients → forward to UART1 TX */
        xSemaphoreTake(client_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds[i];
            if (fd != -1 && FD_ISSET(fd, &read_fds)) {
                uint8_t buf[TCP_BUF_SIZE];
                int len = recv(fd, buf, sizeof(buf), 0);
                if (len > 0) {
                    uart_write_bytes(UART_PORT, buf, len);
                } else {
                    close(fd);
                    client_fds[i] = -1;
                    printf("[TCP] client disconnected, fd=%d\n", fd);
                }
            }
        }
        xSemaphoreGive(client_mutex);
    }
}

/* ──────────────────────────── UART1 Forward ──────────────────────────── */

static void uart_forward_task(void *arg)
{
    uint8_t buf[UART_BUF_SIZE];
    printf("[UART1] forward task started, TX=IO%d, RX=IO%d\n", UART_TX_PIN, UART_RX_PIN);

    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            broadcast_to_clients(buf, len);
        }
    }
}

/* ──────────────────────────── App Main ──────────────────────────── */

void app_main(void)
{
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize networking */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Start WiFi AP */
    wifi_init_softap();

    /* Configure UART1 */
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    printf("[UART1] configured: TX=IO%d, RX=IO%d, baud=%d\n",
           UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);

    /* Init client array */
    client_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }

    /* Start tasks */
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    xTaskCreate(uart_forward_task, "uart_fwd", 4096, NULL, 5, NULL);

    printf("[APP] UART1-TCP bridge ready. Connect to WiFi '%s', then TCP 192.168.4.1:%d\n",
           WIFI_SSID, TCP_PORT);
}
