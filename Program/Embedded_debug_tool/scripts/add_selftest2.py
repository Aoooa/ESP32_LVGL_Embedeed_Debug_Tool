p = open('../apps/usb_display/usb_display.c', 'r', encoding='utf-8').read()
marker = '''    /* 注册帧回调 */
    app_usbdisp_register_frame_cb(on_frame, ud);

    /* 不自动开启：USB 必须用户主动开启才占用 */

    return ud;
}'''
new_marker = '''    /* 注册帧回调 */
    app_usbdisp_register_frame_cb(on_frame, ud);

#if USDISP_AUTO_ENABLE_ON_BOOT
    /* 自测: APP 创建 500ms 后自动 enable (无需点按钮) */
    ESP_LOGW(TAG, "self-test: auto-enable in 500ms");
    lv_timer_t *auto_t = lv_timer_create(ud_auto_enable_cb, 500, ud);
    lv_timer_set_repeat_count(auto_t, 1);
#endif

#if USDISP_PERIODIC_RESTART_SEC > 0
    /* 自测: 每 N 秒 disable+enable 一次 (让 PC 端能反复测试) */
    ESP_LOGW(TAG, "self-test: periodic restart every %d sec", USDISP_PERIODIC_RESTART_SEC);
    lv_timer_create(ud_periodic_restart_cb, USDISP_PERIODIC_RESTART_SEC * 1000, ud);
#endif

    return ud;
}'''
if marker in p:
    p = p.replace(marker, new_marker, 1)
    print('OK: added self-test hooks to apps/usb_display/usb_display.c')
else:
    print('FAIL: marker not found in apps/usb_display/usb_display.c')