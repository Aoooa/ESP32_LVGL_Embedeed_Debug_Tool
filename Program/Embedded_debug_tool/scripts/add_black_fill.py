p = 'D:/Task/embedded_debugGER_tool/Program/Embedded_debug_tool/display/app_display.c'
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()

# 在 build_ui() 里的 "lv_obj_t *scr = lv_screen_active();" 之后插入 fill
# 找到第二个 build_ui() (普通 boot), 不是第一个 (FSP)
import re

old_marker = '    lv_obj_t *scr = lv_screen_active();\n    /* 全局关闭屏幕滚动条 (右滑返回桌面不应滚动) */\n    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);\n    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);\n    /* 桌面启动器（默认界面）：黑夜主题起步，全屏背景由 launcher root 承担 */'

new_marker = '''    lv_obj_t *scr = lv_screen_active();
    /* 全局关闭屏幕滚动条 (右滑返回桌面不应滚动) */
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* 强制黑屏：LVGL 注册 display 后首次渲染可能透传脏数据/默认白底，
     * 这里直接画一层黑覆盖整个屏幕再交给 launcher root 接管背景 */
    {
        extern esp_lcd_panel_handle_t drv_display_get_panel(void);
        extern esp_lcd_panel_io_handle_t drv_display_get_io(void);
        esp_lcd_panel_handle_t panel = drv_display_get_panel();
        static uint16_t black_fb[240];  /* 240*2 = 480B, 单行 RGB565 black */
        memset(black_fb, 0xFF, sizeof(black_fb));  /* ST7789 INVON: 写 0xFF 显示黑 */
        for (int y = 0; y < 320; y++) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, 240, y + 1, black_fb);
        }
    }

    /* 桌面启动器（默认界面）：黑夜主题起步，全屏背景由 launcher root 承担 */'''

count = s.count(old_marker)
print(f'Found {count} build_ui markers')
if count >= 1:
    s = s.replace(old_marker, new_marker, 2)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s)
    print('OK: added black fill in build_ui')
else:
    print('FAIL: marker not found')