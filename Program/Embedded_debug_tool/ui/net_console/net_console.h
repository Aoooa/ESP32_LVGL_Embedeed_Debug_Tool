#ifndef NET_CONSOLE_H
#define NET_CONSOLE_H

/* net_console：网络服务信息 APP（全屏）。
 *
 * 显示当前服务状态（WiFi AP / IP / Web 地址 / UART TCP 端口），
 * 供快速查看设备网络信息。左上角 ← 返回桌面。
 *
 * 线程：创建/销毁在 LVGL 线程。
 */

#include "lvgl.h"

typedef struct net_console net_console_t;

/* 返回回调（左上角 ←，回桌面） */
typedef void (*net_console_back_cb_t)(void *ctx);

/* 创建网络信息 APP（parent 通常为当前 screen） */
net_console_t *net_console_create(lv_obj_t *parent, net_console_back_cb_t back_cb, void *ctx);

/* 销毁网络信息 APP */
void net_console_destroy(net_console_t *nc);

#endif /* NET_CONSOLE_H */
