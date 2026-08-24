#ifndef APP_USB2TTL_H
#define APP_USB2TTL_H

/* app_usb2ttl：USB 虚拟串口（CDC-ACM）↔ UART1 桥接 + ISP 下载控制。
 *
 * 功能：
 *   PC 经 USB 枚举为 COM 口（TinyUSB CDC-ACM 单接口），数据双向转发到
 *   UART1（IO2 TX / IO4 RX，与 TCP 转发共用同一硬件但互斥使用）。
 *   "进入 ISP 模式"：BOOT0（默认 IO5）+ RST（默认 IO18）两个 GPIO 执行
 *   复位序列，把目标 MCU 拉进系统 bootloader（BOOT0=1 → RST 脉冲 →
 *   BOOT0=0），之后 PC 侧烧录工具经本桥接串口下载固件。
 *
 * 互斥（本服务 enforce 双向）：
 *   1. USB PHY：与读卡器（MSC）/DAP（HID）互斥——三者共用内部 USB PHY，
 *      同时只能启用一个；enable 时检查对方状态，对方 enable 时也检查本服务。
 *   2. UART1：桥接开启期间置 g_bridges[0]->paused=1，TCP/网页/终端转发
 *      暂停读取；关闭时恢复 115200/8N1 并解除暂停。
 *
 * 波特率/校验配置：仅 OFF 状态可改（开启后锁定，避免运行期重配 UART
 * 竞态）；ISP 引脚运行期可改（仅 enter_isp 时驱动）。UI 按状态禁用按钮。
 *
 * 线程约定：
 *   enable/disable/enter_isp/配置 须在 LVGL 线程或持 esp_lv_adapter 锁
 *   调用；桥接任务为独立任务；TinyUSB 事件回调运行在 TinyUSB 任务，
 *   只更新连接标志，不做 UART 操作。
 */

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    USB2TTL_OFF = 0,   /* 未启用：UART1 归 TCP/终端转发 */
    USB2TTL_ON,        /* 已启用：CDC 桥接 + UART1 独占 */
    USB2TTL_ERROR,     /* 上次操作失败 */
} usb2ttl_state_t;

const char *app_usb2ttl_state_str(usb2ttl_state_t st);

usb2ttl_state_t app_usb2ttl_get_state(void);

/* PC 串口是否已打开（CDC 线状态 DTR+RTS，TinyUSB 任务更新） */
bool app_usb2ttl_pc_open(void);

/* 开启桥接：暂停 UART1 转发 → 应用波特率/校验 → 装 TinyUSB CDC →
 * 启动桥接任务。与读卡器/DAP 冲突返回 ESP_ERR_INVALID_STATE。 */
esp_err_t app_usb2ttl_enable(void);

/* 关闭桥接（任意状态幂等）：停任务 → 卸载 TinyUSB → 恢复 USJ 控制台 →
 * 恢复 UART1 115200/8N1 + 解除暂停 */
esp_err_t app_usb2ttl_disable(void);

/* ── ISP 控制 ── */

/* BOOT0/RST 引脚默认值（板面空闲脚） */
#define USB2TTL_BOOT0_DEF   5
#define USB2TTL_RST_DEF     18

/* 配置 ISP 引脚（运行期可改，仅 enter_isp 时生效）。boot0/rst 必须
 * ∈ 可用引脚白名单且不相等，否则返回 ESP_ERR_INVALID_ARG。 */
esp_err_t app_usb2ttl_set_isp_pins(int boot0, int rst);
void app_usb2ttl_get_isp_pins(int *boot0, int *rst);

/* 执行进入 ISP 模式复位序列（阻塞约 0.5s，须在 LVGL 线程或持锁调用）：
 *   BOOT0=1 → 稳定 20ms → RST 拉低 100ms → 释放（BOOT0 采样期保持 1）→
 *   等 300ms 目标 bootloader 就绪 → BOOT0=0 → 两脚交还输入。 */
esp_err_t app_usb2ttl_enter_isp(void);

/* ── 自动下载（默认关；勾选后 PC 经 SetCommState 控制 DTR/RTS 触发 ISP） ──
 *
 * 原理等同 USB 转 TTL 自动下载电路：CDC 的 DTR/RTS 状态位直通映射到
 * BOOT0/RST GPIO，PC 烧录工具按自身时序翻转控制线即自动进 bootloader，
 * 平时 DTR/RTS 静止则纯桥接。极性对齐 esptool 约定：
 *   DTR=1 → BOOT0 高（运行）；DTR=0 → BOOT0 低（下载模式）
 *   RTS=1 → RST 低（复位）；RTS=0 → RST 高（释放）
 * 兼容工具：esptool.py（--before 复位序列）、STM32CubeProgrammer
 * （UART 模式勾选 Hardware reset 并配 RTS=Reset / DTR=Boot0）、
 * stm32flash 等支持 DTR/RTS 复位的工具。
 *
 * 运行时修改安全：勾选瞬间按当前 DTR/RTS 立即应用；取消勾选交还 GPIO
 * 输入。桥接关闭时只存标志（无 CDC，GPIO 不驱动）。 */
void app_usb2ttl_set_auto_isp(bool en);
bool app_usb2ttl_get_auto_isp(void);

/* ── 串口参数（仅 OFF 状态可改） ── */

/* 波特率：ISP 常用 115200（多数 STM32 自动波特率）/ 57600 */
esp_err_t app_usb2ttl_set_baud(int baud);
int app_usb2ttl_get_baud(void);

/* 校验：false=8N1（通用/新系列 STM32），true=8E1（经典 STM32F1/F4 等
 * USART bootloader 要求偶校验） */
esp_err_t app_usb2ttl_set_parity_even(bool even);
bool app_usb2ttl_get_parity_even(void);

#endif /* APP_USB2TTL_H */
