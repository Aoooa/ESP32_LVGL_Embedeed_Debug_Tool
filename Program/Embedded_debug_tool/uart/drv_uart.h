#ifndef DRV_UART_H
#define DRV_UART_H

#include "driver/uart.h"

#define DRV_UART_BUF_SIZE  1024
#define DRV_UART_BAUD_RATE 115200

/* 初始化（幂等）：驱动已安装则跳过。若引脚未占用则占用 tx/rx */
void drv_uart_init(uart_port_t port, int tx_pin, int rx_pin);
/* 反初始化：卸载驱动并复位 tx/rx 引脚为 GPIO（未安装则无操作）。
 * 供 APP 退出时释放 IO（UART 惰性占用：只在终端/桥接 APP 打开期间占引脚） */
void drv_uart_deinit(uart_port_t port, int tx_pin, int rx_pin);
int drv_uart_read(uart_port_t port, uint8_t *buf, int size, int timeout_ms);
int drv_uart_write(uart_port_t port, const uint8_t *data, int len);

#endif
