import os, shutil, re, subprocess

ROOT = 'D:/Task/embedded_debugger_tool/Program/Embedded_debug_tool'

# 1. Delete directories
for d in ['usb_display', 'apps/usb_display', 'drivers']:
    p = os.path.join(ROOT, d)
    if os.path.exists(p):
        shutil.rmtree(p)
        print(f'rmdir: {p}')

# 2. Delete USB-related scripts
for f in ['auto_flash.ps1', 'check_selftest.py', 'com_owner.ps1', 'cycle_check.py',
          'cycle_selftest.py', 'drv_detail.ps1', 'find_dev.py', 'find_dev2.py',
          'get_dev_path.ps1', 'get_path2.ps1', 'get_path3.ps1', 'get_paths.py',
          'retry_pid4010.ps1', 'winusb_test.py', 'switch_to_vendor.py',
          'add_log.py', 'add_selftest.py', 'add_selftest2.py', 'strip_cb.py',
          'move_cb.py', 'move_cbs.py', 'enable_main_selftest.py',
          'enable_selftest.py', 'fix_msos.py', 'fix_uaf.py', 'fix_cfg_ref.py',
          'apply_msos.py', 'idf.sh']:
    p = os.path.join(ROOT, 'scripts', f)
    if os.path.exists(p):
        os.remove(p)
        print(f'rm: scripts/{f}')

# 3. Delete doc
f = os.path.join(ROOT, 'docs/usb_display_verification.md')
if os.path.exists(f):
    os.remove(f)
    print(f'rm: docs/usb_display_verification.md')

# 4. Clean main/main.c
p = os.path.join(ROOT, 'main/main.c')
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
s = re.sub(r'/\* USB 副屏自测模式.*?#endif\n*', '', s, flags=re.DOTALL)
s = s.replace('#include "app_usbdisp.h"\n', '')
s = re.sub(r'#define USDISP_SELFTEST_ENABLE\s+\d+\n', '', s)
s = re.sub(r'/\*.*?self-test.*?\*/\n?', '', s, flags=re.DOTALL)
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('cleaned: main/main.c')

# 5. Clean CMakeLists.txt
p = os.path.join(ROOT, 'main/CMakeLists.txt')
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
s = s.replace('        "${MODULE_DIR}/usb_display/app_usbdisp.c"\n', '')
s = s.replace('        "${MODULE_DIR}/apps/usb_display/usb_display.c"\n', '')
s = s.replace('        "${MODULE_DIR}/usb_display"\n', '')
s = s.replace('        "${MODULE_DIR}/apps/usb_display"\n', '')
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('cleaned: main/CMakeLists.txt')

# 6. Clean launcher.h
p = os.path.join(ROOT, 'apps/launcher/launcher.h')
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
s = re.sub(r'    LAUNCH_APP_USBDISP,\s*/\*.*?\*/\n?', '', s)
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('cleaned: apps/launcher/launcher.h')

# 7. Clean launcher.c
p = os.path.join(ROOT, 'apps/launcher/launcher.c')
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
s = s.replace('#include "usb_display.h"\n', '')
s = re.sub(r'extern const lv_image_dsc_t launcher_icon_udisp;\n', '', s)
s = re.sub(r'\s*&launcher_icon_udisp,\s*/\*.*?\*/\n?', '\n', s)
s = re.sub(r'\s*\{\s*"UsbDisp".*?\},?\s*\n', '\n', s)
s = re.sub(r'\s*\[LAUNCH_APP_USBDISP\] = \{.*?\},?\s*\n', '\n', s, flags=re.DOTALL)
s = re.sub(r'#define APP_COUNT 13', '#define APP_COUNT 12', s)
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('cleaned: apps/launcher/launcher.c')

# 8. Clean sdkconfig.defaults
p = os.path.join(ROOT, 'sdkconfig.defaults')
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
s = re.sub(r'# USB 副屏 APP.*?(?=\n\n)', '', s, flags=re.DOTALL)
s = re.sub(r'# xfz1986.*?(?=\n\n)', '', s, flags=re.DOTALL)
s = re.sub(r'# USB 驱动签名.*?(?=\n\n)', '', s, flags=re.DOTALL)
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('cleaned: sdkconfig.defaults')

# 9. Regen launcher_icons.c
result = subprocess.run(
    ['python', '../apps/launcher/launcher_icons_gen.py'],
    cwd='D:/Task/embedded_debugger_tool/Program/Embedded_debug_tool/apps/launcher',
    capture_output=True, text=True)
print('regen icons:', result.stdout.strip())
if result.stderr:
    print('err:', result.stderr.strip()[:200])

print('\nDONE')
