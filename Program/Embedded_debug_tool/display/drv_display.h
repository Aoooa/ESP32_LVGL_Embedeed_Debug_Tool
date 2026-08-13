#ifndef DRV_DISPLAY_H
#define DRV_DISPLAY_H

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

/* LCD 引脚定义 — ESP32-S3-Touch-LCD-2 */
#define DRV_LCD_HOST            SPI2_HOST
#define DRV_LCD_PIN_SCLK        39
#define DRV_LCD_PIN_MOSI        38
#define DRV_LCD_PIN_MISO        40
#define DRV_LCD_PIN_CS          45
#define DRV_LCD_PIN_DC          42
#define DRV_LCD_PIN_RST         -1
#define DRV_LCD_PIN_BL          1

#define DRV_LCD_H_RES           240
#define DRV_LCD_V_RES           320
#define DRV_LCD_PCLK_HZ        (80 * 1000 * 1000)
#define DRV_LCD_CMD_BITS        8
#define DRV_LCD_PARAM_BITS      8

/* 触摸引脚定义 */
#define DRV_TOUCH_I2C_PORT      I2C_NUM_0
#define DRV_TOUCH_PIN_SCL       47
#define DRV_TOUCH_PIN_SDA       48
#define DRV_TOUCH_PIN_INT       -1
#define DRV_TOUCH_PIN_RST       -1

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_handle_t touch;
} drv_display_t;

/**
 * @brief 初始化 LCD + 触摸硬件
 */
void drv_display_init(drv_display_t *disp);

/* ST7789 硬件滚动（纯文字滚动，无需重传全屏） */
void drv_display_set_scroll_area(int top, int height);  /* 0x33 VSCRDEF */
void drv_display_set_scroll_start(int line);            /* 0x37 VSCRSADD */
esp_lcd_panel_io_handle_t drv_display_get_io(void);     /* 直接发命令用 */
esp_lcd_panel_handle_t drv_display_get_panel(void);     /* 直接写 RAM 用 */

/* 硬件旋转（RAM 布局）：0x37 滚动轴 = RAM 行
 * swap=true（横屏布局）：RAM 行 = 屏幕水平 → 0x37 水平滚动
 * swap=false（竖屏布局）：RAM 行 = 屏幕垂直 → 0x37 垂直滚动 */
void drv_display_set_hw_rotation(bool swap, bool mirror_x, bool mirror_y);

#endif
