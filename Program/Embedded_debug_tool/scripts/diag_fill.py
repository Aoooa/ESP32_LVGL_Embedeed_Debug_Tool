p = 'D:/Task/embedded_debugGER_tool/Program/Embedded_debug_tool/display/drv_display.c'
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
# 改 fill 0xFF 为 0x00 验证 INVON 是否工作
# backup 原始 0xFF -> 改为 0x00 测试
old = 'memset(fb, 0xFF, (size_t)DRV_LCD_H_RES * LCD_BLACK_BLOCK_H * 2);'
new = 'memset(fb, 0x00, (size_t)DRV_LCD_H_RES * LCD_BLACK_BLOCK_H * 2);  /* DIAG: 测 INVON */'
if old in s:
    s = s.replace(old, new, 1)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s)
    print('OK: changed fill 0xFF -> 0x00 (diagnostic)')
else:
    print('FAIL')