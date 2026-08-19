/*
 * DAP_config.h - 平台适配：ESP32-S3 GPIO 位敲 SWD 调试口
 *
 * DAP 内核（DAP.c/SW_DP.c）为 ARM DAPLink（Apache-2.0）原样移植；
 * 本文件提供平台 IO/延时/信息实现，参考 windowsair/wireless-esp8266-dap
 * （MIT）的 ESP32-S3 引脚方案改写为纯 GPIO 位敲（不占用 SPI 外设，
 * 与本项目 LCD/SD 共享的 SPI2 总线无冲突）。
 *
 * 引脚（避开板子已占用：LCD SPI2、触摸 I2C、SD CS、UART0、USB、PSRAM）：
 *   SWCLK  = GPIO12    SWDIO = GPIO11    nRESET = GPIO13（可配置）
 * 对外接线：SWCLK/SWDIO/GND（可选 nRESET），接目标板 SWD 口。
 */

#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

#include <stdint.h>
#include <string.h>
#include <stdbool.h>   /* DAP.h 的 `#if defined(true)` 依赖 */
#include "sdkconfig.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_rom_sys.h"
#include "cmsis_compiler.h"

/* ── 调试单元配置 ── */
#define CPU_CLOCK               240000000U
#define IO_PORT_WRITE_CYCLES    2U
#define DAP_SWD                 1U      /* 仅 SWD（JTAG 不启用，少占代码） */
#define DAP_JTAG                0U
#define DAP_JTAG_DEV_CNT        1U
#define DAP_DEFAULT_PORT        1U      /* 1 = SWD */
#define DAP_DEFAULT_SWJ_CLOCK   1000000U /* 1MHz 位敲默认时钟 */
#define DAP_PACKET_SIZE         64U     /* HID 包大小（CMSIS-DAP v1 固定 64B） */
#define DAP_PACKET_COUNT        1U

/* SWO 关 / UART 关 */
#define SWO_FUNCTION_ENABLE     0U
#define SWO_UART                0U
#define SWO_MANCHESTER          0U
#define SWO_BUFFER_SIZE         2048U
#define SWO_STREAM              0U
#define TIMESTAMP_CLOCK         1000000U
#define DAP_UART                0U
#define DAP_UART_DRIVER         1U
#define DAP_UART_RX_BUFFER_SIZE 1024U
#define DAP_UART_TX_BUFFER_SIZE 1024U
#define DAP_UART_USB_COM_PORT   0U
#define TARGET_FIXED            0U

/* ── 引脚定义（可改） ── */
#define PIN_SWDIO_MOSI  11      /* SWDIO 数据线 */
#define PIN_SWCLK       12      /* SWCLK 时钟线 */
#define PIN_TDO         9
#define PIN_TDI         10
#define PIN_nTRST       14
#define PIN_nRESET      13      /* 目标复位线 */

/* nRESET 使能：0 = 未接复位线（PIN_nRESET_OUT 空转） */
#ifndef DAP_NRESET_ENABLE
#define DAP_NRESET_ENABLE 1
#endif

/* ── GPIO 位敲基元 ── */
#define GPIO_SET_LEVEL_HIGH(pin)  gpio_ll_set_level(&GPIO, pin, 1)
#define GPIO_SET_LEVEL_LOW(pin)   gpio_ll_set_level(&GPIO, pin, 0)
#define GPIO_GET_LEVEL(pin)       gpio_ll_get_level(&GPIO, pin)
#define GPIO_FUNCTION_SET(pin)    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO)

/* 延时：DAP.h 提供默认 PIN_DELAY_SLOW/FAST 实现（slow 空循环 ≈3 周期/次，
 * 与 DELAY_SLOW_CYCLES=3 匹配；fast 零延时，GPIO 操作本身构成时钟）。 */

/* ── 端口初始化 ── */

__STATIC_INLINE void dap_pins_init(void)
{
    GPIO_FUNCTION_SET(PIN_TDO);
    GPIO_FUNCTION_SET(PIN_TDI);
    GPIO_FUNCTION_SET(PIN_nTRST);
    GPIO_FUNCTION_SET(PIN_nRESET);

    /* SWDIO/SWCLK：推挽输出 */
    gpio_ll_output_enable(&GPIO, PIN_SWDIO_MOSI);
    gpio_ll_od_disable(&GPIO, PIN_SWDIO_MOSI);
    gpio_ll_output_enable(&GPIO, PIN_SWCLK);
    gpio_ll_od_disable(&GPIO, PIN_SWCLK);
    GPIO_SET_LEVEL_HIGH(PIN_SWCLK);

    /* TDO：输入 */
    gpio_ll_output_disable(&GPIO, PIN_TDO);
    gpio_ll_input_enable(&GPIO, PIN_TDO);

    /* TDI：输出 */
    gpio_ll_output_enable(&GPIO, PIN_TDI);
    gpio_ll_od_disable(&GPIO, PIN_TDI);

    /* nTRST：开漏 + 上拉 */
    gpio_ll_output_enable(&GPIO, PIN_nTRST);
    gpio_ll_od_enable(&GPIO, PIN_nTRST);
    gpio_ll_pulldown_dis(&GPIO, PIN_nTRST);
    gpio_ll_pullup_en(&GPIO, PIN_nTRST);

#if DAP_NRESET_ENABLE
    /* nRESET：开漏 + 上拉（高=释放复位） */
    gpio_ll_output_enable(&GPIO, PIN_nRESET);
    gpio_ll_od_enable(&GPIO, PIN_nRESET);
    gpio_ll_pulldown_dis(&GPIO, PIN_nRESET);
    gpio_ll_pullup_en(&GPIO, PIN_nRESET);
    GPIO_SET_LEVEL_HIGH(PIN_nRESET);
#endif

    /* SWDIO 初始输入态（SWD 空闲） */
    gpio_ll_output_disable(&GPIO, PIN_SWDIO_MOSI);
    gpio_ll_input_enable(&GPIO, PIN_SWDIO_MOSI);
}

__STATIC_INLINE void PORT_SWD_SETUP(void)
{
    dap_pins_init();
}

__STATIC_INLINE void PORT_JTAG_SETUP(void)
{
    dap_pins_init();
}

__STATIC_INLINE void PORT_OFF(void)
{
#if DAP_NRESET_ENABLE
    gpio_ll_output_enable(&GPIO, PIN_nRESET);
    gpio_ll_od_enable(&GPIO, PIN_nRESET);
    GPIO_SET_LEVEL_HIGH(PIN_nRESET);
#endif
}

__STATIC_INLINE void DAP_SETUP(void)
{
    dap_pins_init();
}

/* ── SWCLK/TCK ── */
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) { return 0U; }
__STATIC_FORCEINLINE void      PIN_SWCLK_TCK_SET(void) { GPIO_SET_LEVEL_HIGH(PIN_SWCLK); }
__STATIC_FORCEINLINE void      PIN_SWCLK_TCK_CLR(void) { GPIO_SET_LEVEL_LOW(PIN_SWCLK); }

/* ── SWDIO/TMS ── */
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void)   { return GPIO_GET_LEVEL(PIN_SWDIO_MOSI); }
__STATIC_FORCEINLINE void      PIN_SWDIO_TMS_SET(void)  { GPIO_SET_LEVEL_HIGH(PIN_SWDIO_MOSI); }
__STATIC_FORCEINLINE void      PIN_SWDIO_TMS_CLR(void)  { GPIO_SET_LEVEL_LOW(PIN_SWDIO_MOSI); }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void)       { return PIN_SWDIO_TMS_IN(); }
__STATIC_FORCEINLINE void      PIN_SWDIO_OUT(uint32_t bit)
{
    if (bit & 1U) PIN_SWDIO_TMS_SET();
    else          PIN_SWDIO_TMS_CLR();
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void)
{
    gpio_ll_output_enable(&GPIO, PIN_SWDIO_MOSI);
    gpio_ll_od_disable(&GPIO, PIN_SWDIO_MOSI);
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void)
{
    gpio_ll_output_disable(&GPIO, PIN_SWDIO_MOSI);
    gpio_ll_input_enable(&GPIO, PIN_SWDIO_MOSI);
}

/* ── TDI/TDO ── */
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return GPIO_GET_LEVEL(PIN_TDI); }
__STATIC_FORCEINLINE void      PIN_TDI_OUT(uint32_t bit)
{
    if (bit & 1U) GPIO_SET_LEVEL_HIGH(PIN_TDI);
    else          GPIO_SET_LEVEL_LOW(PIN_TDI);
}
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return GPIO_GET_LEVEL(PIN_TDO); }

/* ── nTRST ── */
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return 0U; }
__STATIC_FORCEINLINE void      PIN_nTRST_OUT(uint32_t bit)
{
    if (bit & 1U) GPIO_SET_LEVEL_HIGH(PIN_nTRST);
    else          GPIO_SET_LEVEL_LOW(PIN_nTRST);
}

/* ── nRESET（可选） ── */
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void)
{
#if DAP_NRESET_ENABLE
    return GPIO_GET_LEVEL(PIN_nRESET);
#else
    return 1U;
#endif
}
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit)
{
#if DAP_NRESET_ENABLE
    if (bit & 1U) GPIO_SET_LEVEL_HIGH(PIN_nRESET);
    else          GPIO_SET_LEVEL_LOW(PIN_nRESET);
#endif
}

/* ── LED（无硬件 LED，空实现） ── */
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) { (void)(bit); }
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit)   { (void)(bit); }

/* ── 时间戳（SWO 关闭，返回 0） ── */
__STATIC_INLINE uint32_t TIMESTAMP_GET(void) { return 0U; }

/* ── 延时 ──
 * 注意：不定义 DAP_Delay 宏——CherryDAP 版 DAP.c 内置 ID_DAP_Delay 命令处理
 * （同名函数）。需要 ms 级忙等直接用 esp_rom_delay_us（见 RESET_TARGET）。 */

/* 无设备特定复位时序（标准 nRESET 由 DAP_SWJ_Pins 命令驱动） */
__STATIC_INLINE uint8_t RESET_TARGET(void) { return 0U; }

/* ── DAP 信息字符串 ── */
__STATIC_INLINE uint8_t DAP_GetVendorString(char *str)
{
    strcpy(str, "Espressif");
    return (uint8_t)(sizeof("Espressif"));
}

__STATIC_INLINE uint8_t DAP_GetProductString(char *str)
{
    strcpy(str, "CMSIS-DAP");
    return (uint8_t)(sizeof("CMSIS-DAP"));
}

__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str)
{
    strcpy(str, "00000000");
    return (uint8_t)(sizeof("00000000"));
}

__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str)  { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str)    { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str)   { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str)     { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str)
{
    (void)str;
    return 0U;
}

#endif /* __DAP_CONFIG_H__ */
