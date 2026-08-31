p = open('app_usbdisp.c', 'r', encoding='utf-8').read()
lines = p.split('\n')
out = []
skip = False
depth = 0
for line in lines:
    if '#if USDISP_AUTO_ENABLE_ON_BOOT' in line or '#if USDISP_PERIODIC_RESTART_SEC' in line:
        skip = True
        depth = 1
        continue
    if skip:
        if line.strip().startswith('#if'): depth += 1
        if line.strip() == '#endif':
            depth -= 1
            if depth == 0: skip = False
        continue
    out.append(line)
open('app_usbdisp.c', 'w', encoding='utf-8').write('\n'.join(out))
print('done')