#include "app_display.h"
#include "drv_display.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "app_display";

void app_display_start(void)
{
    /* 硬件初始化 */
    drv_display_t disp = {0};
    drv_display_init(&disp);

    /* LVGL 适配器初始化 */
    ESP_LOGI(TAG, "init LVGL adapter...");
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    /* 注册显示屏 */
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_DEFAULT_CONFIG(
            disp.panel, NULL,
            DRV_LCD_H_RES, DRV_LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0);
    lv_display_t *lv_disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(lv_disp != NULL);

    /* 注册触摸 */
    esp_lv_adapter_touch_config_t tp_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lv_disp, disp.touch);
    lv_indev_t *lv_tp = esp_lv_adapter_register_touch(&tp_cfg);
    assert(lv_tp != NULL);

    /* 启动 LVGL 任务 */
    ESP_ERROR_CHECK(esp_lv_adapter_start());

    /* 绘制 Hello World */
    ESP_LOGI(TAG, "drawing Hello World...");
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x1F1F1F), 0);

        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label, "Hello World!");
        lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_center(label);

        esp_lv_adapter_unlock();
    }

    ESP_LOGI(TAG, "ready");
}
