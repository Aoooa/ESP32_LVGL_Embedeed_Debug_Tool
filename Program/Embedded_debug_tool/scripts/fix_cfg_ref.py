p = open('app_usbdisp.c', 'r', encoding='utf-8').read()
old = '    .high_speed_config = NULL,\n};'
new = '    .high_speed_config = NULL,\n    .full_speed_config = s_cfg_desc,\n};'
if old in p:
    p = p.replace(old, new, 1)
    open('app_usbdisp.c', 'w', encoding='utf-8').write(p)
    print('OK: added full_speed_config')
else:
    print('FAIL: marker not found')