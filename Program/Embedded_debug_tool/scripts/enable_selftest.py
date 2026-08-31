p = open('app_usbdisp.c', 'r', encoding='utf-8').read()
old = '''#ifndef USDISP_AUTO_ENABLE_ON_BOOT
#define USDISP_AUTO_ENABLE_ON_BOOT 0   /* 1=APP创建后 500ms 自动 enable (无需点按钮) */
#endif
#ifndef USDISP_PERIODIC_RESTART_SEC
#define USDISP_PERIODIC_RESTART_SEC 0  /* >0=周期 disable+enable (秒), 0=关闭 */
#endif'''
new = '''#ifndef USDISP_AUTO_ENABLE_ON_BOOT
#define USDISP_AUTO_ENABLE_ON_BOOT 1   /* 1=APP创建后 500ms 自动 enable (无需点按钮) */
#endif
#ifndef USDISP_PERIODIC_RESTART_SEC
#define USDISP_PERIODIC_RESTART_SEC 30 /* >0=周期 disable+enable (秒), 0=关闭 */
#endif'''
if old in p:
    p = p.replace(old, new, 1)
    open('app_usbdisp.c', 'w', encoding='utf-8').write(p)
    print('OK: enabled self-test defaults')