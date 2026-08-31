p = open('../main/main.c', 'r', encoding='utf-8').read()
old = '#define USDISP_SELFTEST_ENABLE     0'
new = '#define USDISP_SELFTEST_ENABLE     1   /* start auto-enable, no APP needed */'
if old in p:
    p = p.replace(old, new, 1)
    open('../main/main.c', 'w', encoding='utf-8').write(p)
    print('OK')
else:
    print('FAIL')