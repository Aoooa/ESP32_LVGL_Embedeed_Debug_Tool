#ifndef DRV_UART_H
#define DRV_UART_H

#include "driver/uart.h"

#define DRV_UART_BUF_SIZE  1024
#define DRV_UART_BAUD_RATE 115200

/**
 * @brief 初始化 UART 并设置引脚
 * @param port UART 端口号
 * @param tx_pin TX 引脚号
 * @param rx_pin RX 引脚号
 */
void drv_uart_init(uart_port_t port, int tx_pin, int rx_pin);

/**
 * @brief 从 UART 读取数据
 * @param port UART 端口号
 * @param buf  缓冲区
 * @param size 缓冲区大小
 * @param timeout_ms 超时时间(毫秒)
 * @return 实际读取的字节数
 */
int drv_uart_read(uart_port_t port, uint8_t *buf, int size, int timeout_ms);

/**
 * @brief 向 UART 写入数据
 * @param port UART 端口号
 * @param data 数据指针
 * @param len  数据长度
 * @return 实际写入的字节数
 */
int drv_uart_write(uart_port_t port, const uint8_t *data, int len);

#endif /* DRV_UART_H */
