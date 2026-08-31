import re
p = 'D:/Task/embedded_debugger_tool/Program/Embedded_debug_tool/apps/launcher/launcher_icons_gen.py'
with open(p, 'r', encoding='utf-8') as f:
    s = f.read()
s = re.sub(r'def icon_usbdisp.*?\}\n\n', '', s, flags=re.DOTALL)
s = re.sub(r'\s+\("udisp".*?\n', '\n', s)
with open(p, 'w', encoding='utf-8') as f:
    f.write(s)
print('cleaned')