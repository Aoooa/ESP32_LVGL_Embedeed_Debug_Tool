#include "drv_display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"

static const char *TAG = "drv_display";

void drv_display_init(drv_display_t *disp)
{
    /* 背光 */
    gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DRV_LCD_PIN_BL,
    };
    gpio_config(&bk_cfg);
    gpio_set_level(DRV_LCD_PIN_BL, 1);

    /* SPI 总线 */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = DRV_LCD_PIN_SCLK,
        .mosi_io_num = DRV_LCD_PIN_MOSI,
        .miso_io_num = DRV_LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DRV_LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DRV_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* ST7789 面板 IO */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = DRV_LCD_PIN_DC,
        .cs_gpio_num = DRV_LCD_PIN_CS,
        .pclk_hz = DRV_LCD_PCLK_HZ,
        .lcd_cmd_bits = DRV_LCD_CMD_BITS,
        .lcd_param_bits = DRV_LCD_PARAM_BITS,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)DRV_LCD_HOST, &io_cfg, &io_handle));
    disp->io = io_handle;

    /* ST7789 面板 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DRV_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &disp->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(disp->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(disp->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(disp->panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(disp->panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(disp->panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(disp->panel, true));
    ESP_LOGI(TAG, "LCD ST7789 ready: %dx%d", DRV_LCD_H_RES, DRV_LCD_V_RES);

    /* I2C 总线 */
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = DRV_TOUCH_I2C_PORT,
        .scl_io_num = DRV_TOUCH_PIN_SCL,
        .sda_io_num = DRV_TOUCH_PIN_SDA,
        .glitch_ignore_cnt = 7,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    /* CST816S 触摸 IO */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = { .disable_control_phase = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(i2c_bus, &tp_io_cfg, &tp_io));

    /* CST816S 触摸面板 */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DRV_LCD_V_RES,
        .y_max = DRV_LCD_H_RES,
        .rst_gpio_num = DRV_TOUCH_PIN_RST,
        .int_gpio_num = DRV_TOUCH_PIN_INT,
        .flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &disp->touch));
    ESP_LOGI(TAG, "Touch CST816S ready");
}
