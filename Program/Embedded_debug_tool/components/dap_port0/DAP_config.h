/*
 * DAP_config.h - SWD 端口 0 引脚配置（公共实现见 DAP_config_common.h）
 *
 * 引脚（避开板子已占用：LCD SPI2、触摸 I2C、SD CS、UART0/1/2、USB、PSRAM）：
 *   SWD1：SWDIO=GPIO11  SWCLK=GPIO12  nRESET=GPIO13（可改）
 * 对外接线：SWCLK/SWDIO/GND（可选 nRESET），接目标板 SWD 口。
 */

#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

/* ── 引脚定义（可改） ── */
#define PIN_SWDIO_MOSI  11      /* SWDIO 数据线 */
#define PIN_SWCLK       12      /* SWCLK 时钟线 */
#define PIN_TDO         9       /* JTAG 未启用（DAP_JTAG=0），定义不重叠即可 */
#define PIN_TDI         10
#define PIN_nTRST       14
#define PIN_nRESET      13      /* 目标复位线 */

/* 公共实现（GPIO 基元/引脚操作/延时/DAP 信息，引用上方引脚宏） */
#include "DAP_config_common.h"

#endif /* __DAP_CONFIG_H__ */
