p = open('../apps/usb_display/usb_display.c', 'r', encoding='utf-8').read()
old = 'static lv_font_t *ud_font(void) {'
new = '''#if USDISP_AUTO_ENABLE_ON_BOOT || USDISP_PERIODIC_RESTART_SEC > 0
static void ud_auto_enable_cb(lv_timer_t *t) {
    usb_display_t *ud = (usb_display_t *)lv_timer_get_user_data(t);
    if (!ud) return;
    esp_err_t ret = app_usbdisp_enable();
    if (ret == ESP_OK) ud_enter_streaming(ud);
    ESP_LOGW("usb_display", "self-test enable -> %s", esp_err_to_name(ret));
}
#endif

#if USDISP_PERIODIC_RESTART_SEC > 0
static void ud_periodic_restart_cb(lv_timer_t *t) {
    usb_display_t *ud = (usb_display_t *)lv_timer_get_user_data(t);
    if (!ud) return;
    ESP_LOGW("usb_display", "self-test: periodic restart");
    app_usbdisp_disable();
    lv_timer_t *re_t = lv_timer_create(ud_auto_enable_cb, 500, ud);
    lv_timer_set_repeat_count(re_t, 1);
}
#endif

static lv_font_t *ud_font(void) {'''
if old in p:
    p = p.replace(old, new, 1)
    open('../apps/usb_display/usb_display.c', 'w', encoding='utf-8').write(p)
    print('OK: cb defs moved')
else:
    print('FAIL')