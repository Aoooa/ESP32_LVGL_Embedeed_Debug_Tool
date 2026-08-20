#ifndef USBIP_SERVER_H
#define USBIP_SERVER_H

/* usbip_server：无线 DAP 通道（USB/IP server，TCP 872）。
 *
 * PC 端用 usbip-win（usbip.exe --tcp-port 872 attach -r <设备IP> -b 1-1）
 * 把本设备挂载为虚拟 USB 设备（CMSIS-DAP，SWD1），Keil/pyOCD 无感使用。
 * 参考 windowsair/wireless-esp8266-dap（MIT）。
 *
 * ── 备选无线方案：OpenOCD remote_bitbang（免驱动，纯 TCP） ──
 * 说明：若不想装 usbip-win 驱动（Secure Boot 限制），OpenOCD 原生支持
 * remote_bitbang 接口（TCP 转发 SWD 位操作，无需 USB/驱动）。
 * 未来实现：监听 TCP 873 端口，实现 remote_bitbang ASCII 协议：
 *   r/R=读 SWDIO、s/S=写 SWDIO、t/T=时钟、q=查询、b=总线复位，
 * 每个位操作映射到 dap_ports[0]（SWD1）的 GPIO。
 * PC 端 openocd.cfg：
 *   interface remote_bitbang
 *   remote_bitbang_port 873
 *   remote_bitbang_host 192.168.4.1
 *   transport select swd
 *   source [find target/n32g455.cfg]
 * 使用：openocd -f wireless_dap.cfg（或 VS Code Cortex-Debug 配 OpenOCD 后端）
 * ─────────────────────────────────────────────
 *
 * 线程：TCP 任务；与 USB 通道通过 dap_port_locks[]（app_dap.c）串行执行
 * 同一端口的 DAP 内核。
 */

#include "esp_err.h"
#include <stdbool.h>

esp_err_t usbip_server_start(void);
esp_err_t usbip_server_stop(void);
bool usbip_server_is_running(void);

#endif /* USBIP_SERVER_H */
