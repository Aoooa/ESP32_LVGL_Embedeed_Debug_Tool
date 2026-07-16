#ifndef APP_TCP_H
#define APP_TCP_H

#include "app_bridge.h"

/**
 * @brief TCP 服务器任务入口
 * @param arg uart_bridge_t* 指针
 */
void app_tcp_server_task(void *arg);

#endif /* APP_TCP_H */
