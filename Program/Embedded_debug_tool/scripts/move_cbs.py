p = open('app_usbdisp.c', 'r', encoding='utf-8').read()
# 找 ud_auto_enable_cb 和 ud_periodic_restart_cb 的定义 - 移除
import re
# 找两个 cb 的定义
m = re.search(r'#if USDISP_AUTO_ENABLE_ON_BOOT.*?#endif\n\n?#if USDISP_PERIODIC_RESTART_SEC > 0.*?#endif', p, re.DOTALL)
if m:
    p = p[:m.start()] + p[m.end():]
    print('removed cb defs from app_usbdisp.c')
else:
    # 简单版 - 找 #if 块
    print('no regex match, manual needed')