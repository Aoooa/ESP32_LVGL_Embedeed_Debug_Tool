/* app_test.c —— APP 回调测试模块（默认不编译；启用见 app_test.h） */

#include "app_test.h"
#include "app_manifest.h"
#include "launcher.h"
#include "esp_log.h"

#define T_TAG "app_test"

/* 遍历全部 APP：launch → debug_event → back 事件（预期弹栈回桌面） */
static void test_launch_back_chain(void)
{
    for (int i = 0; i < LAUNCH_APP_COUNT; i++) {
        const app_manifest_t *m = &app_manifests[i];
        if (!m->launch) {
            ESP_LOGI(T_TAG, "[%s] no launch cb, skip", m->name);
            continue;
        }
        ESP_LOGI(T_TAG, "=== %s: launch ===", m->name);
        launcher_app_launch(i, NULL);
        if (!launcher_app_running()) {
            ESP_LOGW(T_TAG, "[%s] launch failed (no app running)", m->name);
            continue;
        }
        /* 调试事件（若实现） */
        launcher_event_debug(0x11);
        /* 返回事件：file_browser 根目录/其他 APP 书架页 → true → 弹栈回桌面 */
        launcher_app_swipe_back(NULL);
        if (launcher_app_running()) {
            ESP_LOGW(T_TAG, "[%s] back did NOT pop app (still running)", m->name);
            launcher_app_close(NULL);   /* 兜底清理 */
        } else {
            ESP_LOGI(T_TAG, "[%s] back popped app -> OK", m->name);
        }
    }
}

/* 旋转事件路由：launch → rotate（APP 无 rotate 回调 → 弹栈关闭） */
static void test_rotate_event(void)
{
    const app_manifest_t *m = &app_manifests[LAUNCH_APP_FILES];
    ESP_LOGI(T_TAG, "=== %s: rotate event ===", m->name);
    launcher_app_launch(LAUNCH_APP_FILES, NULL);
    if (launcher_app_running()) {
        launcher_event_rotate(90);   /* 无 rotate 回调 → 弹栈关闭回桌面 */
        if (launcher_app_running()) {
            ESP_LOGW(T_TAG, "[%s] rotate did NOT close app", m->name);
            launcher_app_close(NULL);
        } else {
            ESP_LOGI(T_TAG, "[%s] rotate -> closed -> OK", m->name);
        }
    }
}

/* SD 就绪事件：launch → refresh（栈顶 refresh 回调，无则忽略） */
static void test_sd_ready_event(void)
{
    const app_manifest_t *m = &app_manifests[LAUNCH_APP_FILES];
    ESP_LOGI(T_TAG, "=== %s: sd_ready event ===", m->name);
    launcher_app_launch(LAUNCH_APP_FILES, NULL);
    if (launcher_app_running()) {
        launcher_event_sd_ready();
        ESP_LOGI(T_TAG, "[%s] sd_ready routed -> OK", m->name);
        launcher_app_close(NULL);
    }
}

void app_test_run(void)
{
    ESP_LOGI(T_TAG, "========== APP callback test start ==========");
    test_launch_back_chain();
    test_rotate_event();
    test_sd_ready_event();
    ESP_LOGI(T_TAG, "========== APP callback test done ==========");
}
