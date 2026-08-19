#include "drv_dap.h"
#include "DAP_config.h"   /* 先于 DAP.h：提供 stdbool/PIN 基元 */
#include "DAP.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "drv_dap";

/* 目标连接缓存：DAP_Connect 成功后置位，DAP_Disconnect 清位 */
static bool s_target_connected;

esp_err_t drv_dap_init(void)
{
    DAP_Setup();   /* 引脚初始化（DAP_SETUP→dap_pins_init）+ DAP 内核默认状态 */
    s_target_connected = false;
    ESP_LOGI(TAG, "DAP ready: SWD GPIO%u(SWDIO)/GPIO%u(SWCLK)/GPIO%u(nRESET), 1MHz default",
             11, 12, 13);
    return ESP_OK;
}

uint32_t drv_dap_execute(const uint8_t *req, uint8_t *rsp)
{
    if (!req || !rsp) return 0;

    /* 连接状态跟踪（仅缓存，不干预协议） */
    uint8_t cmd = req[0];
    if (cmd == 0x02U) {            /* DAP_Connect */
        s_target_connected = true;
        ESP_LOGI(TAG, "target connected (port=%u)", req[1]);
    } else if (cmd == 0x03U) {     /* DAP_Disconnect */
        s_target_connected = false;
        ESP_LOGI(TAG, "target disconnected");
    }

    /* 返回 DAP 内核 num：低 16 位 = 响应长度，高 16 位 = 请求长度 */
    return DAP_ExecuteCommand(req, rsp);
}

bool drv_dap_target_connected(void)
{
    return s_target_connected;
}
