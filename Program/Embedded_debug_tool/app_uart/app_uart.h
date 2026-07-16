#ifndef APP_UART_H
#define APP_UART_H

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#define APP_UART_MAX_TCP_CLIENTS 3
#define APP_UART_BCAST_QUEUE_LEN 32

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int tcp_port;
    const char *name;
    volatile int paused;
    int tcp_fds[APP_UART_MAX_TCP_CLIENTS];
    SemaphoreHandle_t tcp_mutex;
    volatile int send_newline;
    volatile int send_hex;
    volatile int timer_on;
    volatile int timer_ms;
    TimerHandle_t send_timer;
    uint8_t send_raw[256];
    int send_raw_len;
    volatile uint32_t tx_bytes;
} uart_bridge_t;

typedef struct {
    uint8_t data[1024];
    int len;
} bcast_item_t;

extern uart_bridge_t *g_bridges[2];
extern QueueHandle_t g_bcast_queue;
extern QueueHandle_t g_tcp_bcast_queue;

void app_uart_init(uart_bridge_t *br);
void app_uart_fwd_task(void *arg);
void app_uart_send(uart_bridge_t *br, const char *data, int len);
int app_hex_to_bytes(const char *hex, uint8_t *out, int max_len);

#endif
