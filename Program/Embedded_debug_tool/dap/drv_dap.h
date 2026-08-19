#ifndef DRV_DAP_H
#define DRV_DAP_H

/* drv_dap：SWD 调试器驱动（GPIO 位敲 + ARM DAPLink 内核）。
 *
 * 线程：任意线程可调用（GPIO 位敲无锁需求，不与 SPI2 总线交互）；
 * 但 SWD 时序敏感，禁止在 ISR 中调用 drv_dap_execute。
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* 初始化调试口 GPIO + DAP 内核（引脚见 DAP_config.h，可改宏） */
esp_err_t drv_dap_init(void);

/* 执行一条 CMSIS-DAP 命令（同步，几 ms 内返回）：
 * req 命令缓冲区（len 字节），rsp 响应缓冲区（≥64B）
 * 返回 DAP 内核 num：低 16 位 = 响应长度（0 = 无响应），高 16 位 = 请求长度 */
uint32_t drv_dap_execute(const uint8_t *req, uint8_t *rsp);

/* 目标连接状态（最近一次 DAP 事务后缓存；未连接=0） */
bool drv_dap_target_connected(void);

#endif /* DRV_DAP_H */
