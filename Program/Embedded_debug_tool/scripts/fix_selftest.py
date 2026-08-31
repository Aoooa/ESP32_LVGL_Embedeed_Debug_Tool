p = open('../main/main.c', 'r', encoding='utf-8').read()
old = '''static void usdisp_selftest_task(void *arg) {
    (void)arg;
    ESP_LOGI("usdisp_selftest", "started, first enable in 5s");
    vTaskDelay(pdMS_TO_TICKS(5000));
    bool on = false;
    while (1) {
        if (!on) {
            esp_err_t r = app_usbdisp_enable();
            ESP_LOGI("usdisp_selftest", "ENABLE -> %s (st=%s, fps=%.1f, err=%u)",
                     esp_err_to_name(r),
                     app_usbdisp_state_str(app_usbdisp_get_state()),
                     app_usbdisp_get_fps(),
                     (unsigned)app_usbdisp_get_error_count());
        } else {
            app_usbdisp_disable();
            ESP_LOGI("usdisp_selftest", "DISABLE -> st=%s",
                     app_usbdisp_state_str(app_usbdisp_get_state()));
        }
        on = !on;
        vTaskDelay(pdMS_TO_TICKS(on ? 15000 : 5000));  /* on 15s, off 5s */
    }
}'''
new = '''static void usdisp_selftest_task(void *arg) {
    (void)arg;
    ESP_LOGI("usdisp_selftest", "started, enabling once in 5s and holding");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_err_t r = app_usbdisp_enable();
    ESP_LOGI("usdisp_selftest", "ENABLE -> %s (st=%s, fps=%.1f, err=%u)",
             esp_err_to_name(r),
             app_usbdisp_state_str(app_usbdisp_get_state()),
             app_usbdisp_get_fps(),
             (unsigned)app_usbdisp_get_error_count());
    /* Hold vendor mode for PC testing, no cycle */
    vTaskDelete(NULL);
}'''
if old in p:
    p = p.replace(old, new, 1)
    open('../main/main.c', 'w', encoding='utf-8').write(p)
    print('OK: selftest now enable-once-and-hold')
else:
    print('FAIL: old not found')