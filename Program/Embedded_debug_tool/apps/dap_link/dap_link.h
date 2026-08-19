#ifndef DAP_LINK_H
#define DAP_LINK_H

/* dap_link：DAP Link（CMSIS-DAP 烧录器/调试器）APP。
 *
 * 界面：SWD 接口示意 + 设备状态 + 提示 + 底部开关。
 * 开启后 USB 枚举为 CMSIS-DAP（HID），PC 上 pyOCD/OpenOCD/Keil 直接识别，
 * 通过 SWCLK/SWDIO/nRESET 三线烧录/调试目标板（GPIO 位敲，不占 SPI 总线）。
 *
 * 线程：创建/回调在 LVGL 线程；服务事件经 lv_timer 轮询刷新界面。
 */

#include "lvgl.h"

typedef void (*dap_link_back_cb_t)(void *ctx);

typedef struct dap_link dap_link_t;

dap_link_t *dap_link_create(lv_obj_t *parent, dap_link_back_cb_t back_cb, void *ctx);
void dap_link_destroy(dap_link_t *dl);
bool dap_link_swipe_back(dap_link_t *dl);

#endif /* DAP_LINK_H */
