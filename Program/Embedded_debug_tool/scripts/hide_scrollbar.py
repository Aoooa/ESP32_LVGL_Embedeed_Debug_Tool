p = 'D:/Task/embedded_debugger_tool/Program/Embedded_debug_tool/display/app_display.c'
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()

# 在 lv_obj_t *scr = lv_screen_active(); 之后插入 disable scrollbar
old1 = '    lv_obj_t *scr = lv_screen_active();\n    /* 桌面启动器（默认界面）：黑夜主题起步，全屏背景由 launcher root 承担 */'
new1 = '    lv_obj_t *scr = lv_screen_active();\n    /* 全局关闭屏幕滚动条 (右滑返回桌面不应滚动) */\n    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);\n    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);\n    /* 桌面启动器（默认界面）：黑夜主题起步，全屏背景由 launcher root 承担 */'

count = s.count(old1)
print(f'Found {count} occurrences')
if count >= 1:
    s = s.replace(old1, new1, 2)  # 替换所有
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s)
    print('OK: disabled screen scrollbar')
else:
    print('FAIL: marker not found')