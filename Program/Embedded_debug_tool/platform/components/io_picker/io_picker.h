#ifndef IO_PICKER_H
#define IO_PICKER_H

/* io_picker —— 通用 IO 引脚选择模块（LVGL 9，公共组件）。
 *
 * 用途：APP 需要占用物理引脚时，声明所需功能（能力掩码），弹出两列
 * 引脚选择界面：左侧 IO0..21、右侧 IO26..48，每脚一个彩色按钮——
 *   青   = 空闲 GPIO（可复用 UART/SPI/I2C/PWM，S3 全 IO 矩阵）
 *   黄   = 空闲且带 ADC 能力
 *   灰   = 硬件性不可用（LCD/SD/触摸/USB/IMU/BOOT 等板上焊死或总线独占）
 * 在当前需求掩码下不满足的彩色脚同样置灰不可选。
 * 单击选中 → 立即回调返回引脚号并关闭界面（单选即回）。
 *
 * 占用语义：静态表只标"硬件性不可用"；App 运行时占用走
 * io_picker_reserve/release（App 退出自动释放），不写入静态表。
 *
 * 线程：LVGL 线程（或持 esp_lv_adapter 锁）。
 */

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/* 引脚能力（能力位；ESP32-S3 IO 矩阵使所有空闲数字脚均可复用为
 * UART/SPI/I2C/PWM——故这几类共用"空闲"判定；ADC 单独按通道分组） */
typedef enum {
    IO_CAP_GPIO = 1 << 0,
    IO_CAP_UART = 1 << 1,
    IO_CAP_SPI  = 1 << 2,
    IO_CAP_I2C  = 1 << 3,
    IO_CAP_PWM  = 1 << 4,
    IO_CAP_ADC  = 1 << 5,
    IO_CAP_ADC1 = 1 << 6,   /* 仅 ADC1（S3 连续 DMA 只支持 ADC1，IO1..10） */
} io_cap_t;

/* 选择结果回调：io = 选中引脚号；io < 0 = 用户取消 */
typedef void (*io_pick_done_t)(void *ctx, int io);

/* 弹出 IO 选择界面。caps = 需要的功能掩码（如 IO_CAP_UART|IO_CAP_GPIO，
 * 或 IO_CAP_ANY 定义请用 0 表示"任意空闲"）。返回 false = 已有一界面在显示 */
bool io_picker_show(lv_obj_t *parent, uint32_t caps, io_pick_done_t cb, void *ctx);

/* ▲ 任意空闲脚：caps == 0 */
#define IO_CAPS_ANY 0u

/* ── 账本查询 / 运行时占用（不依赖 UI） ── */

/* 该脚在静态表+动态占用下，是否可用（且能力匹配） */
bool io_picker_is_available(int io, uint32_t caps);

/* 运行时声明占用/释放（App 完成+退出后调用 io_picker_release 归还） */
void io_picker_reserve(int io);
void io_picker_release(int io);

/* 便捷：返回第一个满足 caps 的空闲脚（-1 无）。编程配置外设时用 */
int io_picker_alloc(uint32_t caps);

/* 当前选择界面是否显示中（供返回手势等判断） */
bool io_picker_active(void);

/* 当前选择界面根对象（供拖动返回目标使用；无界面返回 NULL） */
lv_obj_t *io_picker_get_obj(void);

/* 取消当前选择（等同点空白：回调 io=-1；界面销毁） */
void io_picker_cancel(void);

/* 立即关闭界面且不回调（宿主销毁场景用，防悬挂回调访问已释放内存） */
void io_picker_close_now(void);

#endif /* IO_PICKER_H */