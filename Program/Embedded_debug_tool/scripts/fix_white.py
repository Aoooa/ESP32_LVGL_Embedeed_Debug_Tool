p = 'D:/Task/embedded_debugGER_tool/Program/Embedded_debug_tool/display/app_display.c'
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
# scr bg_color 改黑 - 替换 '0x080A0C' -> '0x000000' (两处)
s = s.replace('lv_color_hex(0x080A0C)', 'lv_color_hex(0x000000)', 2)
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('OK: bg_color changed to black')