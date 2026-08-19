#ifndef DRV_DAP_H
#define DRV_DAP_H

/* drv_dap：SWD 调试端口抽象（多实例）。
 *
 * 每个 SWD 口 = 一个 dap_port_t（独立组件实例，内核符号前缀隔离）：
 *   components/dap_port0/  SWD1：SWDIO=11 SWCLK=12 nRESET=13
 *   components/dap_port1/  SWD2：SWDIO=14 SWCLK=15 nRESET=18
 * 新增端口 = 复制 dap_port0 组件，改引脚宏与前缀（见组件 CMakeLists 注释）。
 *
 * 线程：任意线程可调用（GPIO 位敲无锁需求，不与 SPI2 总线交互）；
 * 但 SWD 时序敏感，禁止在 ISR 中调用 execute。
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *name;                          /* 端口名（UI/日志） */
    void (*init)(void);                        /* 引脚 + 内核初始化 */
    uint32_t (*execute)(const uint8_t *req, uint8_t *rsp);  /* 执行 DAP 命令，
        返回 num：低 16 位=响应长度，高 16 位=请求长度 */
} dap_port_t;

#define DAP_PORT_COUNT 2   /* 端口数量（与 drv_dap.c 表一致，有编译期检查） */

/* 端口表（按索引访问）。DAP_PORT_COUNT 须与 drv_dap.c 的 dap_ports 表条目
 * 数一致（drv_dap.c 内有编译期检查兜底） */
extern const dap_port_t *const dap_ports[DAP_PORT_COUNT];

/* 端口数与 TinyUSB HID 接口数联动检查（CONFIG_TINYUSB_HID_COUNT 须 ≥ 端口数，
 * 否则多余的 HID 接口没有对应端口） */
#if DAP_PORT_COUNT > CONFIG_TINYUSB_HID_COUNT
#error "DAP_PORT_COUNT exceeds CONFIG_TINYUSB_HID_COUNT, increase TINYUSB_HID_COUNT"
#endif

#endif /* DRV_DAP_H */
