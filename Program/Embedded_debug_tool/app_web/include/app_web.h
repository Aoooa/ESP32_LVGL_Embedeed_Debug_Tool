#ifndef APP_WEB_H
#define APP_WEB_H

#include "app_bridge.h"
#include "esp_http_server.h"

/** 全局 HTTP 服务器句柄 */
extern httpd_handle_t g_httpd;

/**
 * @brief 启动 HTTP/WebSocket 服务器（端口 80）
 */
void app_web_start(void);

/**
 * @brief WebSocket 广播任务入口
 * @param arg 未使用
 */
void app_ws_broadcast_task(void *arg);

#endif /* APP_WEB_H */
