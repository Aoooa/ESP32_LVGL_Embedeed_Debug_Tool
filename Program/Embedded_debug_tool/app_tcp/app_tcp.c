#include "app_tcp.h"
#include "app_uart.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "drv_uart.h"
#include "esp_log.h"

static void tcp_broadcast_to_clients(uart_bridge_t *br, const uint8_t *data, int len)
{
    xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
    for (int i = 0; i < APP_UART_MAX_TCP_CLIENTS; i++) {
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
    listen(srv, APP_UART_MAX_TCP_CLIENTS);
    ESP_LOGI("app_tcp", "[%s] server on port %d", br->name, br->tcp_port);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        int maxfd = srv;

        xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
        for (int i = 0; i < APP_UART_MAX_TCP_CLIENTS; i++) {
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
                for (int i = 0; i < APP_UART_MAX_TCP_CLIENTS; i++) {
                    if (br->tcp_fds[i] < 0) {
                        br->tcp_fds[i] = fd;
                        ESP_LOGI("app_tcp", "[%s] + fd=%d", br->name, fd);
                        break;
                    }
                }
                xSemaphoreGive(br->tcp_mutex);
                close(fd);
            }
        }

        xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
        for (int i = 0; i < APP_UART_MAX_TCP_CLIENTS; i++) {
            int fd = br->tcp_fds[i];
            if (fd >= 0 && FD_ISSET(fd, &rfds)) {
                uint8_t tmp[512];
                int n = recv(fd, tmp, sizeof(tmp), 0);
                if (n > 0) {
                    drv_uart_write(br->port, tmp, n);
                } else {
                    close(fd); br->tcp_fds[i] = -1;
                }
            }
        }
        xSemaphoreGive(br->tcp_mutex);
    }
}

static void tcp_bcast_task(void *arg)
{
    bcast_item_t item;
    while (1) {
        if (xQueueReceive(g_tcp_bcast_queue, &item, portMAX_DELAY) == pdTRUE) {
            for (int u = 0; u < 2; u++) {
                tcp_broadcast_to_clients(g_bridges[u], item.data, item.len);
            }
        }
    }
}

void app_tcp_start(void)
{
    for (int i = 0; i < 2; i++) {
        xTaskCreate(tcp_server_task, g_bridges[i]->name, 8192, g_bridges[i], 5, NULL);
    }
    xTaskCreate(tcp_bcast_task, "tcp_bcast", 4096, NULL, 5, NULL);
    ESP_LOGI("app_tcp", "ready");
}
