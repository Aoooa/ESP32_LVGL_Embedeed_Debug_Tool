#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_qr.py —— 生成 WebFS 地址二维码模块矩阵存进固件（wf_qr.inc）。
内容：http://192.168.4.1/fs  （WiFi AP 固定 192.168.4.1，无密码）
依赖：python3 + qrcode 库（pip install qrcode）
输出：apps/web_fs/wf_qr.inc  —— 矩阵行列数 + 0/1 模块数组（含 quiet zone）"""
import qrcode, os

TEXT = "http://192.168.4.1/fs"

qr = qrcode.QRCode(version=None, error_correction=qrcode.constants.ERROR_CORRECT_M,
                   box_size=1, border=2)
qr.add_data(TEXT)
qr.make(fit=True)
m = qr.get_matrix()          # list[list[bool]]，含 border
n = len(m)

out = [
    "/* 由 gen_qr.py 生成：<%s> 的 QR 模块矩阵（含 quiet zone）。白=0 黑=1 */" % TEXT,
    "static const int wf_qr_n = %d;" % n,
    "static const uint8_t wf_qr_modules[] = {",
]
for row in m:
    out.append("    " + ",".join("1" if c else "0" for c in row) + ",")
out.append("};")

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wf_qr.inc")
with open(path, "w", newline="\n") as f:
    f.write("\n".join(out) + "\n")
print("written", path, "%dx%d" % (n, n))