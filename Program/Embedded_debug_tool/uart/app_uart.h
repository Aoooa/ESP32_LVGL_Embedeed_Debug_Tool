#ifndef APP_UART_H
#define APP_UART_H

#include "drv_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#define APP_UART_MAX_TCP_CLIENTS 3
#define APP_UART_BCAST_QUEUE_LEN   32
#define APP_UART_DISPLAY_QUEUE_LEN 64

/* TCP/WebSocket 串口数据转发总开关：0=禁用（省 RAM），1=启用。
 * 禁用时相关队列/任务/推送全部跳过，代码保留 */
#define APP_NET_UART_FWD_ENABLED 1

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int tcp_port;
    const char *name;
    volatile int active;        /* UART 驱动已装载（惰性占用：APP 打开期间=1） */
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

typedef struct {
    uint8_t data[256];
    int len;
    int uart_idx;
} disp_item_t;

extern uart_bridge_t *g_bridges[2];
extern QueueHandle_t g_bcast_queue;
extern QueueHandle_t g_tcp_bcast_queue;
extern QueueHandle_t g_display_queue;

void app_uart_init(uart_bridge_t *br);
void app_uart_fwd_task(void *arg);
void app_uart_send(uart_bridge_t *br, const char *data, int len);
int app_hex_to_bytes(const char *hex, uint8_t *out, int max_len);

/* ── UART 惰性占用（上电不占 IO，谁用谁申领）： ──
 * 终端 APP 打开时调 app_uart_start()（装载两个桥接 UART 驱动 + io_picker
 * 记账占用引脚），关闭时调 app_uart_stop()（卸载驱动 + 归还引脚，供其他
 * APP 选择）。幂等：start 已激活则跳过；stop 未激活则无操作。 */

/* 装载 UART1+UART2 驱动并置 active + 记账占用（未装过才装） */
esp_err_t app_uart_start(void);

/* 卸载 UART1+UART2 驱动并清 active + 归还引脚 */
esp_err_t app_uart_stop(void);

#endif
