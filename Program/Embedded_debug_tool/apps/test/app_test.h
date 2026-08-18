#ifndef APP_TEST_H
#define APP_TEST_H

/* app_test：APP 回调测试模块（默认不编译，不影响功能）。
 *
 * 用途：遍历 app_manifests 依次验证各 APP 的统一回调链路
 * （launch / debug_event / back / rotate / refresh），输出 [TEST] 日志。
 *
 * 启用方法：
 *   1. main/CMakeLists.txt SRCS 加入 "${MODULE_DIR}/apps/test/app_test.c"
 *   2. main/main.c 在 app_display_start() 后调用 app_test_run()
 *      （需在 LVGL 线程；若在 main 线程，建议经 lv_timer 延迟执行）
 *
 * 说明：触摸手势（真实触摸）属外部输入，不做自动化测试；
 *      back/rotate 事件通过 launcher 路由接口模拟触发。
 */

/* 运行回调测试（须在 LVGL 线程或持 esp_lv_adapter 锁） */
void app_test_run(void);

#endif /* APP_TEST_H */
