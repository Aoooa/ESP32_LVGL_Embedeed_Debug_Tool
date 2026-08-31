p = open('app_usbdisp.c', 'r', encoding='utf-8').read()

# 1. Add macros after includes
old = '''#include <string.h>
#include <stdatomic.h>'''
new = '''#include <string.h>
#include <stdatomic.h>

/* ==== self-test mode (让 ESP32 不需要用户操作就能跑) ==== */
#ifndef USDISP_AUTO_ENABLE_ON_BOOT
#define USDISP_AUTO_ENABLE_ON_BOOT 0   /* 1=APP创建后 500ms 自动 enable (无需点按钮) */
#endif
#ifndef USDISP_PERIODIC_RESTART_SEC
#define USDISP_PERIODIC_RESTART_SEC 0  /* >0=周期 disable+enable (秒), 0=关闭 */
#endif'''

if old in p:
    p = p.replace(old, new, 1)
    print('OK: added macros')
else:
    print('FAIL: include marker not found')

# 2. Find usb_display_create and add auto-enable timer after the existing init
# Look for the end of usb_display_create (return ud; })
import re
# Add a self-test timer that runs periodically
marker = '''    /* 注册帧回调 */
    app_usbdisp_register_frame_cb(on_frame, ud);

    /* 不自动开启：USB 必须用户主动开启才占用 */

    return ud;
}'''
new_marker = '''    /* 注册帧回调 */
    app_usbdisp_register_frame_cb(on_frame, ud);

#if USDISP_AUTO_ENABLE_ON_BOOT
    /* 自测：APP 创建 500ms 后自动 enable */
    ESP_LOGW(TAG, "self-test mode: auto-enable in 500ms");
    lv_timer_t *auto_t = lv_timer_create(ud_auto_enable_cb, 500, ud);
    lv_timer_set_repeat_count(auto_t, 1);
#endif

#if USDISP_PERIODIC_RESTART_SEC > 0
    /* 自测：每 N 秒 disable+enable 一次（让 PC 端能反复测试）*/
    ESP_LOGW(TAG, "self-test mode: periodic restart every %d sec", USDISP_PERIODIC_RESTART_SEC);
    lv_timer_create(ud_periodic_restart_cb, USDISP_PERIODIC_RESTART_SEC * 1000, ud);
#endif

    return ud;
}'''

if marker in p:
    p = p.replace(marker, new_marker, 1)
    print('OK: added self-test hooks')
else:
    print('FAIL: create marker not found')

# 3. Add the periodic restart callback before enable/disable functions
# Find a good insertion point - before app_usbdisp_enable
enable_marker = 'esp_err_t app_usbdisp_enable(void) {'
if enable_marker in p:
    # Insert callback before enable
    cb_code = '''#if USDISP_AUTO_ENABLE_ON_BOOT || USDISP_PERIODIC_RESTART_SEC > 0
static void ud_auto_enable_cb(lv_timer_t *t) {
    usb_display_t *ud = (usb_display_t *)lv_timer_get_user_data(t);
    if (!ud) return;
    esp_err_t ret = app_usbdisp_enable();
    if (ret == ESP_OK) ud_enter_streaming(ud);
    ESP_LOGW("app_usbdisp", "self-test enable -> %s", esp_err_to_name(ret));
}
#endif

#if USDISP_PERIODIC_RESTART_SEC > 0
static void ud_periodic_restart_cb(lv_timer_t *t) {
    usb_display_t *ud = (usb_display_t *)lv_timer_get_user_data(t);
    if (!ud) return;
    ESP_LOGW(TAG, "self-test: periodic restart");
    app_usbdisp_disable();
    lv_timer_t *re_t = lv_timer_create(ud_auto_enable_cb, 500, ud);
    lv_timer_set_repeat_count(re_t, 1);
}
#endif

'''
    p = p.replace(enable_marker, cb_code + enable_marker, 1)
    print('OK: added self-test callbacks')
else:
    print('FAIL: enable marker not found')

open('app_usbdisp.c', 'w', encoding='utf-8').write(p)
print('written')