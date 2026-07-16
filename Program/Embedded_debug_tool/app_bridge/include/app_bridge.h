#ifndef APP_BRIDGE_H
#define APP_BRIDGE_H

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#define APP_BRIDGE_MAX_TCP_CLIENTS 3
#define APP_BRIDGE_BCAST_QUEUE_LEN 32

/**
 * @brief UART 桥接实例，TCP 和 Web 模块共享
 */
typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int tcp_port;
    const char *name;
    volatile int paused;
    int tcp_fds[APP_BRIDGE_MAX_TCP_CLIENTS];
    SemaphoreHandle_t tcp_mutex;
    /* 发送配置 */
    volatile int send_newline;
    volatile int send_hex;
    volatile int timer_on;
    volatile int timer_ms;
    TimerHandle_t send_timer;
    uint8_t send_raw[256];
    int send_raw_len;
} uart_bridge_t;

/**
 * @brief 广播队列项
 */
typedef struct {
    uint8_t data[1024];
    int len;
} bcast_item_t;

/** 全局桥接实例数组 */
extern uart_bridge_t *g_bridges[2];

/** WebSocket 广播队列 */
extern QueueHandle_t g_bcast_queue;

/**
 * @brief 初始化桥接实例（UART 驱动 + 定时器 + TCP 客户端数组）
 */
void app_bridge_init(uart_bridge_t *br);

/**
 * @brief UART 转发任务入口（读取 UART → TCP 广播 + WS 队列）
 */
void app_uart_fwd_task(void *arg);

/**
 * @brief 向 UART 发送数据（处理 HEX/换行/定时器缓冲）
 */
void app_uart_send(uart_bridge_t *br, const char *data, int len);

/**
 * @brief HEX 字符串转字节数组
 * @return 转换后的字节数
 */
int app_hex_to_bytes(const char *hex, uint8_t *out, int max_len);

#endif /* APP_BRIDGE_H */
