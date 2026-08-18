/* app_font.c —— 中文字体管理器（C 数组字体，编译进固件，零运行时加载） */

#include "app_font.h"
#include "esp_log.h"

static const char *TAG = "app_font";

/* fonts/han_sc_16.c 生成的字体符号 */
LV_FONT_DECLARE(han_sc_16);

lv_font_t *app_font_get(int size)
{
    if (size != 16) {
        ESP_LOGW(TAG, "unsupported size %d (only 16)", size);
        return NULL;
    }
    return &han_sc_16;
}

void app_font_retry(void)
{
    /* C 数组字体随固件烧录，无需重试 */
}
