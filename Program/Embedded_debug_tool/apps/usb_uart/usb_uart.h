#ifndef USB_UART_H
#define USB_UART_H

/* usb_uart：USB 虚拟串口 + ISP 下载 APP（LVGL 9）
 *
 * 界面：状态行（USB 状态 / PC 连接 / 波特率 / 校验 / BOOT0/RST 引脚 /
 * 提示）+ 底部按钮（开启/关闭桥接、进入 ISP 模式）。
 *
 * 互斥提示：读卡器（MSD）/DAP（SWD）占用 USB PHY 时桥接不可开启；
 * 桥接开启期间 UART1 被独占，TCP/网页/终端转发暂停。
 */

#include "lvgl.h"

typedef void (*usb_uart_back_cb_t)(void *ctx);

typedef struct usb_uart_app usb_uart_app_t;

usb_uart_app_t *usb_uart_create(lv_obj_t *parent, usb_uart_back_cb_t back_cb, void *ctx);
void usb_uart_destroy(usb_uart_app_t *app);
bool usb_uart_swipe_back(usb_uart_app_t *app);

#endif /* USB_UART_H */
