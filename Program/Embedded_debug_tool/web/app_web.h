#ifndef APP_WEB_H
#define APP_WEB_H

#include "esp_http_server.h"

extern httpd_handle_t g_httpd;

void app_web_start(void);
void app_web_stop(void);   /* 停止 Web 服务（net_console 开关） */
void app_ws_broadcast_task(void *arg);

#endif
