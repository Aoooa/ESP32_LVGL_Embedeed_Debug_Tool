#include "app_display.h"
#include "drv_display.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>

static lv_obj_t *s_label_ip = NULL;
static lv_obj_t *s_label_uart1 = NULL;
static lv_obj_t *s_label_uart2 = NULL;
static lv_obj_t *s_label_web = NULL;

void app_display_start(void)
{
    drv_display_t disp = {0};
    drv_display_init(&disp);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
            disp.panel, disp.io,
            DRV_LCD_H_RES, DRV_LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0);
    lv_display_t *lv_disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(lv_disp != NULL);

    esp_lv_adapter_touch_config_t tp_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lv_disp, disp.touch);
    lv_indev_t *lv_tp = esp_lv_adapter_register_touch(&tp_cfg);
    assert(lv_tp != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        /* 标题 */
        lv_obj_t *title = lv_label_create(scr);
        lv_label_set_text(title, "Embedded Debug Tool");
        lv_obj_set_style_text_color(title, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_set_pos(title, 10, 20);

        /* IP */
        s_label_ip = lv_label_create(scr);
        lv_label_set_text(s_label_ip, "Starting...");
        lv_obj_set_style_text_color(s_label_ip, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(s_label_ip, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(s_label_ip, 10, 55);

        /* UART1 */
        s_label_uart1 = lv_label_create(scr);
        lv_label_set_text(s_label_uart1, "");
        lv_obj_set_style_text_color(s_label_uart1, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(s_label_uart1, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(s_label_uart1, 10, 85);

        /* UART2 */
        s_label_uart2 = lv_label_create(scr);
        lv_label_set_text(s_label_uart2, "");
        lv_obj_set_style_text_color(s_label_uart2, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(s_label_uart2, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(s_label_uart2, 10, 105);

        /* Web */
        s_label_web = lv_label_create(scr);
        lv_label_set_text(s_label_web, "");
        lv_obj_set_style_text_color(s_label_web, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(s_label_web, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(s_label_web, 10, 140);

        esp_lv_adapter_unlock();
    }
}

void app_display_set_info(const char *ip, int uart1_port, int uart2_port)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        char buf[64];

        snprintf(buf, sizeof(buf), "IP: %s", ip);
        lv_label_set_text(s_label_ip, buf);

        snprintf(buf, sizeof(buf), "UART1:%d  IO2/IO4", uart1_port);
        lv_label_set_text(s_label_uart1, buf);

        snprintf(buf, sizeof(buf), "UART2:%d  IO16/IO17", uart2_port);
        lv_label_set_text(s_label_uart2, buf);

        snprintf(buf, sizeof(buf), "Web: http://%s/", ip);
        lv_label_set_text(s_label_web, buf);

        esp_lv_adapter_unlock();
    }
}
