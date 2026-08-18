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

/* 右滑返回（launcher 分发）：无内部分级，直接返回 true（请求关闭回桌面） */
bool net_console_swipe_back(net_console_t *nc);

/* 调试事件（测试模块用）：打印内部状态供验证 */
void net_console_debug_event(net_console_t *nc, int evt);

#endif /* NET_CONSOLE_H */
