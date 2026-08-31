p = open('../main/main.c', 'r', encoding='utf-8').read()
old = '''    ESP_LOGI("usdisp_selftest", "started, enabling once in 5s and holding");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_err_t r = app_usbdisp_enable();
    ESP_LOGI("usdisp_selftest", "ENABLE -> %s (st=%s, fps=%.1f, err=%u)",
             esp_err_to_name(r),
             app_usbdisp_state_str(app_usbdisp_get_state()),
             app_usbdisp_get_fps(),
             (unsigned)app_usbdisp_get_error_count());
    /* Hold vendor mode for PC testing, no cycle */
    vTaskDelete(NULL);'''
new = '''    ESP_LOGI("usdisp_selftest", "started, alternating 3s on / 10s off cycle");
    vTaskDelay(pdMS_TO_TICKS(5000));
    bool on = false;
    while (1) {
        if (!on) {
            esp_err_t r = app_usbdisp_enable();
            ESP_LOGI("usdisp_selftest", "ENABLE -> %s (st=%s)", esp_err_to_name(r), app_usbdisp_state_str(app_usbdisp_get_state()));
            vTaskDelay(pdMS_TO_TICKS(3000));   /* vendor 3s - PC test window */
        } else {
            app_usbdisp_disable();
            ESP_LOGI("usdisp_selftest", "DISABLE (COM back, flash window)");
            vTaskDelay(pdMS_TO_TICKS(10000));  /* USJ 10s - flash window */
        }
        on = !on;
    }'''
if old in p:
    p = p.replace(old, new, 1)
    open('../main/main.c', 'w', encoding='utf-8').write(p)
    print('OK: selftest now cycles enable(3s)/disable(10s)')
else:
    print('FAIL')