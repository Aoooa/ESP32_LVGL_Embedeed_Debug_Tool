p = open('app_usbdisp.c', 'r', encoding='utf-8').read()
old = '''    if (stage == CONTROL_STAGE_SETUP &&
        request->bRequest == 0x01 &&          /* GET_DESCRIPTOR */
        request->wValue == 0x03EE) {          /* MS OS 1.0 vendor descriptor */
        return tud_control_xfer(rhport, request,
                                (void *)s_ms_os_desc, sizeof(s_ms_os_desc));
    }'''
new = '''    if (stage == CONTROL_STAGE_SETUP &&
        request->bRequest == 0x01 &&          /* GET_DESCRIPTOR */
        request->wValue == 0x03EE) {          /* MS OS 1.0 vendor descriptor */
        ESP_LOGI("app_usbdisp", "MS OS 1.0 GET_DESCRIPTOR, sending %u bytes", sizeof(s_ms_os_desc));
        return tud_control_xfer(rhport, request,
                                (void *)s_ms_os_desc, sizeof(s_ms_os_desc));
    }
    ESP_LOGD("app_usbdisp", "vendor req stage=%u bRequest=0x%02x wValue=0x%04x wIndex=0x%04x", stage, request->bRequest, request->wValue, request->wIndex);'''
if old in p:
    p = p.replace(old, new, 1)
    open('app_usbdisp.c', 'w', encoding='utf-8').write(p)
    print('OK: added log')
else:
    print('FAIL: old not found')