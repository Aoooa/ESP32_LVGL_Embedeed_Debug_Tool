#include "app_web.h"
#include "app_uart.h"
#include <stdlib.h>
#include <string.h>

static void ws_broadcast_work(void *arg)
{
    bcast_item_t *item = (bcast_item_t *)arg;
    size_t client_count = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];

    if (httpd_get_client_list(g_httpd, &client_count, fds) == ESP_OK) {
        for (size_t i = 0; i < client_count; i++) {
            if (httpd_ws_get_fd_info(g_httpd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT, .payload = item->data,
                    .len = item->len, .final = true,
                };
                httpd_ws_send_frame_async(g_httpd, fds[i], &frame);
            }
        }
    }
    free(item);
}

void app_ws_broadcast_task(void *arg)
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
