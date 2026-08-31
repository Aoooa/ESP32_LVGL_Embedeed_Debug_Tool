p = open('../main/main.c', 'r', encoding='utf-8').read()
# 找到 enable-once-and-hold 块
import re
# 看现有 selftest task 的实际代码
m = re.search(r'static void usdisp_selftest_task.*?^\}', p, re.DOTALL | re.MULTILINE)
if m:
    print(m.group()[:500])