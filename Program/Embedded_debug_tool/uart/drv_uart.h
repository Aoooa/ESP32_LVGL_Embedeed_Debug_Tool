#ifndef DRV_UART_H
#define DRV_UART_H

#include "driver/uart.h"

#define DRV_UART_BUF_SIZE  1024
#define DRV_UART_BAUD_RATE 115200

void drv_uart_init(uart_port_t port, int tx_pin, int rx_pin);
int drv_uart_read(uart_port_t port, uint8_t *buf, int size, int timeout_ms);
int drv_uart_write(uart_port_t port, const uint8_t *data, int len);

#endif
