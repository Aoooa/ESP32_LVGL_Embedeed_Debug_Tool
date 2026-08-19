#ifndef APP_DAP_H
#define APP_DAP_H

/* app_dap：DAP Link（CMSIS-DAP）服务层。
 *
 * 功能：开关状态机 + TinyUSB HID 对接（CMSIS-DAP v1，PC 免驱动识别为
 * "CMSIS-DAP" 设备，pyOCD/OpenOCD/Keil 可直接烧录/调试）。
 *
 * 线程：enable/disable 须在 LVGL 线程或持 esp_lv_adapter 锁调用；
 * USB 枚举/命令处理在 TinyUSB 任务内（tud_hid 回调）。
 *
 * 互斥：与 USB 读卡器（app_cardreader）共用 USB PHY，不可同时开启；
 * 本模块在 enable 时检测读卡器状态并拒绝。
 */

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DAP_STATE_OFF = 0,   /* 关闭（USB 未接管） */
    DAP_STATE_READY,     /* 开启：USB 已接管，等待/已连接 PC */
    DAP_STATE_ERROR,     /* 开启失败（原因见日志/UI 提示） */
} dap_state_t;

dap_state_t app_dap_get_state(void);
const char *app_dap_state_str(dap_state_t state);

/* 开启 DAP：初始化 SWD 引脚 + 安装 TinyUSB HID 设备。
 * 返回 ESP_ERR_INVALID_STATE = 读卡器正在使用 USB（先关闭读卡器） */
esp_err_t app_dap_enable(void);

/* 关闭 DAP：卸载 TinyUSB + USB PHY 归还 USB-Serial/JTAG（恢复 COM 控制台） */
esp_err_t app_dap_disable(void);

#endif /* APP_DAP_H */
