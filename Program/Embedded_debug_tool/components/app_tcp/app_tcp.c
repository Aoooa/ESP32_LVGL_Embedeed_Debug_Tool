#include "app_tcp.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "drv_uart.h"
#include "esp_log.h"

static const char *TAG = "app_tcp";

void app_tcp_server_task(void *arg)
{
    uart_bridge_t *br = (uart_bridge_t *)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        ESP_LOGE(TAG, "[%s] socket fail", br->name);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(br->tcp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "[%s] bind fail", br->name);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    listen(srv, APP_BRIDGE_MAX_TCP_CLIENTS);
    ESP_LOGI(TAG, "[%s] TCP on port %d", br->name, br->tcp_port);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        int maxfd = srv;

        xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
        for (int i = 0; i < APP_BRIDGE_MAX_TCP_CLIENTS; i++) {
            if (br->tcp_fds[i] >= 0) {
                FD_SET(br->tcp_fds[i], &rfds);
                if (br->tcp_fds[i] > maxfd) maxfd = br->tcp_fds[i];
            }
        }
        xSemaphoreGive(br->tcp_mutex);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) < 0) continue;

        /* 接受新连接 */
        if (FD_ISSET(srv, &rfds)) {
            struct sockaddr_in ca;
            socklen_t cl = sizeof(ca);
            int fd = accept(srv, (struct sockaddr *)&ca, &cl);
            if (fd >= 0) {
                xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
                for (int i = 0; i < APP_BRIDGE_MAX_TCP_CLIENTS; i++) {
                    if (br->tcp_fds[i] < 0) {
                        br->tcp_fds[i] = fd;
                        ESP_LOGI(TAG, "[%s] + fd=%d", br->name, fd);
                        break;
                    }
                }
                xSemaphoreGive(br->tcp_mutex);
                close(fd);
            }
        }

        /* 接收 TCP 客户端数据 → UART TX */
        xSemaphoreTake(br->tcp_mutex, portMAX_DELAY);
        for (int i = 0; i < APP_BRIDGE_MAX_TCP_CLIENTS; i++) {
            int fd = br->tcp_fds[i];
            if (fd >= 0 && FD_ISSET(fd, &rfds)) {
                uint8_t tmp[512];
                int n = recv(fd, tmp, sizeof(tmp), 0);
                if (n > 0) {
                    drv_uart_write(br->port, tmp, n);
                } else {
                    close(fd);
                    br->tcp_fds[i] = -1;
                    ESP_LOGI(TAG, "[%s] - fd=%d", br->name, fd);
                }
            }
        }
        xSemaphoreGive(br->tcp_mutex);
    }
}
