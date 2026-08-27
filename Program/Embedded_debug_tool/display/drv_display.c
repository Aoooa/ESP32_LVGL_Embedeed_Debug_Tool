#include "drv_display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "drv_display";

/* 全局句柄（滚动/旋转等需要直接操作 io/panel） */
static drv_display_t s_disp;

/* ST7789 硬件滚动（阅读器纯文字滚动用，无需重传全屏）：
 * 0x33 VSCRDEF 滚动区（TFA 顶固定 + VSA 滚动区高 + BFA 底固定），
 * 0x37 VSCRSADD 滚动起始地址（按扫描线，1 行像素粒度） */
void drv_display_set_scroll_area(int top, int height)
{
    uint8_t p[6] = {
        (uint8_t)(top >> 8), (uint8_t)(top & 0xFF),
        (uint8_t)(height >> 8), (uint8_t)(height & 0xFF),
        0, 0
    };
    esp_err_t ret = esp_lcd_panel_io_tx_param(s_disp.io, 0x33, p, sizeof(p));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "0x33 tx_param failed: %s", esp_err_to_name(ret));
    }
}

void drv_display_set_scroll_start(int line)
{
    uint8_t p[2] = { (uint8_t)(line >> 8), (uint8_t)(line & 0xFF) };
    esp_err_t ret = esp_lcd_panel_io_tx_param(s_disp.io, 0x37, p, sizeof(p));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "0x37 tx_param failed: %s", esp_err_to_name(ret));
    }
}

esp_lcd_panel_io_handle_t drv_display_get_io(void)
{
    return s_disp.io;
}

esp_lcd_panel_handle_t drv_display_get_panel(void)
{
    return s_disp.panel;
}

void drv_display_set_hw_rotation(bool swap, bool mirror_x, bool mirror_y)
{
    if (!s_disp.panel) return;
    esp_lcd_panel_swap_xy(s_disp.panel, swap);
    esp_lcd_panel_mirror(s_disp.panel, mirror_x, mirror_y);
}

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
        /* spi_master 为 PSRAM 源缓冲在 ISR 中分配 DMA priv buffer（=事务大小，
         * 必须内部 RAM）。过大分配失败；esp_lcd_panel_io_spi 按此上限分块连续传输。
         * 16 行=7.5KB，兼顾分配成功率与事务开销 */
        .max_transfer_sz = DRV_LCD_H_RES * 16 * sizeof(uint16_t),
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
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(disp->panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(disp->panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(disp->panel, true));

    /* 上电先黑屏：面板 RAM 初始未定义（可能白噪），在开显示（disp_on）前
     * 立即填充纯黑，杜绝上电白屏闪烁。ST7789 开了 invert_color：写 0xFFFF
     * 显示为黑。PSRAM 源缓冲分块提交（每块 ≤ max_transfer_sz=7.5KB=16 行）：
     * spi_master 对 PSRAM 源要在 ISR 分配内部 DMA priv buffer（=事务大小），
     * 整帧 153.6KB 一次提交会分配失败（开发时实测过） */
    #define LCD_BLACK_BLOCK_H 16   /* 240px×16 行 ×2B = 7680B ≤ max_transfer_sz */
    uint16_t *fb = heap_caps_malloc((size_t)DRV_LCD_H_RES * LCD_BLACK_BLOCK_H * 2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb) {
        memset(fb, 0xFF, (size_t)DRV_LCD_H_RES * LCD_BLACK_BLOCK_H * 2);
        for (int y = 0; y < DRV_LCD_V_RES; y += LCD_BLACK_BLOCK_H) {
            esp_err_t err = esp_lcd_panel_draw_bitmap(disp->panel, 0, y,
                                                      DRV_LCD_H_RES,
                                                      y + LCD_BLACK_BLOCK_H, fb);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "black fill block y=%d failed: %s", y, esp_err_to_name(err));
                break;
            }
        }
        heap_caps_free(fb);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(disp->panel, true));
    ESP_LOGI(TAG, "LCD ST7789 ready: %dx%d (black-filled before disp_on)",
             DRV_LCD_H_RES, DRV_LCD_V_RES);

    s_disp = *disp;   /* 全部初始化完成后保存全局句柄（滚动命令用） */

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

    /* CST816S 触摸面板：上电后 IC 未就绪可能 I2C 读 ID 失败（flash 后首次
     * 复位偶发）。重试 3 次（每次 100ms），仍失败则按原逻辑 abort */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DRV_LCD_V_RES,
        .y_max = DRV_LCD_H_RES,
        .rst_gpio_num = DRV_TOUCH_PIN_RST,
        .int_gpio_num = DRV_TOUCH_PIN_INT,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_err_t terr = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        terr = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &disp->touch);
        if (terr == ESP_OK) break;
        ESP_LOGW(TAG, "touch init retry %d/3 failed: %s", i + 1, esp_err_to_name(terr));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_ERROR_CHECK(terr);
    ESP_LOGI(TAG, "Touch CST816S ready");
}
