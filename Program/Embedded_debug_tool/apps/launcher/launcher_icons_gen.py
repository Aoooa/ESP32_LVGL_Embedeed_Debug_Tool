#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""launcher_icons_gen.py —— 生成桌面霓虹图标位图 launcher_icons.c + 预览 PNG
风格：赛博朋克霓虹灯，每图标 2-3 色（各 APP 不同主色），简约造型贴合功能，
主体外带主色光晕。输出 LVGL ARGB8888 位图（字节序 B,G,R,A）40x40。"""

import math, struct, zlib, os

W = H = 40
OUT_C = os.path.join(os.path.dirname(__file__), "launcher_icons.c")
OUT_PNG = os.path.join(os.path.dirname(__file__), "..", "..", "build", "icons_preview.png")

def new_canvas():
    return [[(0, 0, 0, 0) for _ in range(W)] for _ in range(H)]

def setpx(cv, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        cv[y][x] = c

def blend(cv, x, y, c):
    """alpha 混合（光晕叠加用）"""
    if not (0 <= x < W and 0 <= y < H):
        return
    r, g, b, a = cv[y][x]
    r2, g2, b2, a2 = c
    if a2 == 0:
        return
    # 输出 alpha = a + a2*(1-a)
    na = a + (a2 * (255 - a) + 127) // 255
    if na == 0:
        return
    # 颜色按 alpha 加权
    nr = (r * a * (255 - a2) + r2 * a2 * 255 + 127) // (na * 255)
    ng = (g * a * (255 - a2) + g2 * a2 * 255 + 127) // (na * 255)
    nb = (b * a * (255 - a2) + b2 * a2 * 255 + 127) // (na * 255)
    cv[y][x] = (max(0, min(255, nr)), max(0, min(255, ng)), max(0, min(255, nb)), na)

def fill_rect(cv, x0, y0, x1, y1, c):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            setpx(cv, x, y, c)

def fill_circle(cv, cx, cy, r, c):
    for y in range(max(0, cy - r), min(H, cy + r + 1)):
        for x in range(max(0, cx - r), min(W, cx + r + 1)):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                setpx(cv, x, y, c)

def stroke_circle(cv, cx, cy, r, w, c):
    r0 = r - w / 2.0
    r1 = r + w / 2.0
    for y in range(max(0, cy - r - 1), min(H, cy + r + 2)):
        for x in range(max(0, cx - r - 1), min(W, cx + r + 2)):
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            if r0 * r0 <= d2 <= r1 * r1:
                setpx(cv, x, y, c)

def dist_seg(px, py, x0, y0, x1, y1):
    vx, vy = x1 - x0, y1 - y0
    wx, wy = px - x0, py - y0
    L2 = vx * vx + vy * vy
    if L2 == 0:
        return math.hypot(px - x0, py - y0)
    t = max(0.0, min(1.0, (wx * vx + wy * vy) / L2))
    return math.hypot(px - (x0 + t * vx), py - (y0 + t * vy))

def draw_line(cv, x0, y0, x1, y1, w, c):
    x0, y0, x1, y1 = float(x0), float(y0), float(x1), float(y1)
    xa, xb = int(math.floor(min(x0, x1))) - 1, int(math.ceil(max(x0, x1))) + 1
    ya, yb = int(math.floor(min(y0, y1))) - 1, int(math.ceil(max(y0, y1))) + 1
    for y in range(max(0, ya), min(H, yb + 1)):
        for x in range(max(0, xa), min(W, xb + 1)):
            if dist_seg(x + 0.5, y + 0.5, x0, y0, x1, y1) <= w / 2.0:
                setpx(cv, x, y, c)

def stroke_rect(cv, x0, y0, x1, y1, w, c):
    draw_line(cv, x0, y0, x1, y0, w, c)
    draw_line(cv, x1, y0, x1, y1, w, c)
    draw_line(cv, x1, y1, x0, y1, w, c)
    draw_line(cv, x0, y1, x0, y0, w, c)

def round_rect(cv, x0, y0, x1, y1, r, w, c, fill_color=None):
    if fill_color:
        fill_rect(cv, x0 + r, y0, x1 - r, y1, fill_color)
        fill_rect(cv, x0, y0 + r, x1, y1 - r, fill_color)
        fill_circle(cv, x0 + r, y0 + r, r, fill_color)
        fill_circle(cv, x1 - r, y0 + r, r, fill_color)
        fill_circle(cv, x0 + r, y1 - r, r, fill_color)
        fill_circle(cv, x1 - r, y1 - r, r, fill_color)
    stroke_rect(cv, x0, y0, x1, y1, w, c)
    # 圆角（覆盖直角边角）
    for (cx, cy) in [(x0 + r, y0 + r), (x1 - r, y0 + r), (x0 + r, y1 - r), (x1 - r, y1 - r)]:
        stroke_circle(cv, cx, cy, r, w, c)

def arc(cv, cx, cy, r, a0, a1, w, c, steps=32):
    pts = []
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    for i in range(len(pts) - 1):
        draw_line(cv, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], w, c)

def poly_line(cv, pts, w, c):
    for i in range(len(pts) - 1):
        draw_line(cv, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], w, c)

def box_blur_alpha(mask, iters=3):
    """mask: 2D float alpha；3x3 均值迭代，生成光晕 alpha"""
    m = [row[:] for row in mask]
    for _ in range(iters):
        nm = [[0.0] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                s = 0.0
                n = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        yy, xx = y + dy, x + dx
                        if 0 <= yy < H and 0 <= xx < W:
                            s += m[yy][xx]
                            n += 1
                nm[y][x] = s / n
        m = nm
    return m

def apply_glow(cv, shape_mask, main_color, strength=0.55):
    """主体 mask 模糊后以主色叠加光晕"""
    glow = box_blur_alpha(shape_mask, iters=3)
    for y in range(H):
        for x in range(W):
            a = int(glow[y][x] * 255 * strength)
            if a > 0:
                blend(cv, x, y, (main_color[0], main_color[1], main_color[2], a))

def sin_wave(cv, x0, x1, base_y, amp, period, w, c, phase=0.0):
    pts = []
    for x in range(x0, x1 + 1):
        y = base_y - amp * math.sin((x - x0) / float(x1 - x0) * 2 * math.pi * period + phase)
        pts.append((x, y))
    poly_line(cv, pts, w, c)

def hexc(h):
    return ((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF, 255)

# ============ 9 个图标绘制 ============

def icon_files():
    """SD 存储卡：左上切角矩形 + 芯片线（青 + 白）"""
    main = hexc(0x00F0FF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # 主体：SD 卡轮廓（左上切角）
    for y in range(7, 34):
        for x in range(11, 30):
            chop = (y <= 14 and x <= 11 + (y - 7))
            if not chop:
                setpx(cv, x, y, (main[0], main[1], main[2], 255)); mask[y][x] = 1.0
    # 内部白线（芯片/触点）
    for y in range(18, 31):
        setpx(cv, 15, y, white); mask[y][15] = 1.0
        setpx(cv, 21, y, white); mask[y][21] = 1.0
        setpx(cv, 26, y, white); mask[y][26] = 1.0
    apply_glow(cv, mask, main)
    return cv

def icon_reader():
    """打开的书：双页 + 书脊 + 文字行（粉 + 紫）"""
    main = hexc(0xFF5FA2); sub = hexc(0xB14CFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # 左页轮廓（多边形：上角圆一点用简单矩形）
    for y in range(10, 31):
        for x in range(8, 20):
            setpx(cv, x, y, (main[0], main[1], main[2], 255)); mask[y][x] = 1.0
    for y in range(10, 31):
        for x in range(21, 33):
            setpx(cv, x, y, (main[0], main[1], main[2], 255)); mask[y][x] = 1.0
    # 书脊线（中间）
    for y in range(9, 32):
        setpx(cv, 20, y, (main[0], main[1], main[2], 255)); mask[y][20] = 1.0
    # 文字行（紫）
    for y in (15, 19, 23, 27):
        for x in range(11, 18):
            setpx(cv, x, y, (sub[0], sub[1], sub[2], 255)); mask[y][x] = 1.0
        for x in range(24, 30):
            if y != 27:
                setpx(cv, x, y, (sub[0], sub[1], sub[2], 255)); mask[y][x] = 1.0
    apply_glow(cv, mask, main)
    return cv

def icon_terminal():
    """终端窗口 + >_ 提示符（荧光绿 + 青）"""
    main = hexc(0x39FF88); sub = hexc(0x00F0FF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # 窗口圆角框 + 标题条
    round_rect(cv, 7, 11, 33, 31, 3, 2, (main[0], main[1], main[2], 255))
    for y in range(11, 15):
        for x in range(7, 34):
            setpx(cv, x, y, (main[0], main[1], main[2], 255)); mask[y][x] = 1.0
    # 标题条两个按钮点（青）
    fill_circle(cv, 11, 13, 1, (sub[0], sub[1], sub[2], 255))
    fill_circle(cv, 15, 13, 1, (sub[0], sub[1], sub[2], 255))
    # 提示符 >_ （绿 + 青）
    draw_line(cv, 12, 20, 16, 20, 2, (main[0], main[1], main[2], 255))
    draw_line(cv, 16, 20, 12, 25, 2, (main[0], main[1], main[2], 255))
    draw_line(cv, 20, 25, 27, 25, 2, (sub[0], sub[1], sub[2], 255))
    for y in range(13, 30):
        for x in range(8, 33):
            if cv[y][x][3]:
                mask[y][x] = 1.0
    apply_glow(cv, mask, main)
    return cv

def icon_serialip():
    """DB9 串口接头 + 无线信号弧（品红 + 橙）"""
    main = hexc(0xFF00E5); sub = hexc(0xFF8C00)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # 接头梯形（上窄下宽矩形近似）
    for y in range(18, 32):
        wd = 8 + (y - 18) * 2 // 3
        x0 = 20 - wd // 2
        for x in range(x0, x0 + wd):
            setpx(cv, x, y, (main[0], main[1], main[2], 255)); mask[y][x] = 1.0
    # 接头内针点（圆点，3x2）
    for px in (17, 20, 23):
        for py in (21, 27):
            fill_circle(cv, px, py, 1, (main[0], main[1], main[2], 255))
    # 无线弧（橙，两弧）
    arc(cv, 20, 20, 9, 180, 360, 2, (sub[0], sub[1], sub[2], 255))
    arc(cv, 20, 20, 14, 180, 360, 2, (sub[0], sub[1], sub[2], 255))
    for y in range(6, 32):
        for x in range(8, 33):
            if cv[y][x][3]:
                mask[y][x] = 1.0
    apply_glow(cv, mask, main)
    return cv

def icon_cardr():
    """U 盘（USB 闪存）：插头 + 卡体（金 + 品红）"""
    main = hexc(0xFFD700); sub = hexc(0xFF00E5)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # USB 插头（顶部小矩形 + 内部金属舌片）
    stroke_rect(cv, 15, 5, 25, 11, 2, (main[0], main[1], main[2], 255))
    for y in range(8, 11):
        setpx(cv, 18, y, (main[0], main[1], main[2], 255)); mask[y][18] = 1.0
        setpx(cv, 22, y, (main[0], main[1], main[2], 255)); mask[y][22] = 1.0
    # 卡体（品红圆角矩形）
    round_rect(cv, 12, 12, 28, 32, 3, 2, (sub[0], sub[1], sub[2], 255))
    for y in range(13, 32):
        for x in range(13, 28):
            if cv[y][x][3]:
                mask[y][x] = 1.0
    # 卡体指示灯（金小圆）
    fill_circle(cv, 24, 16, 1, (main[0], main[1], main[2], 255))
    apply_glow(cv, mask, sub)
    return cv

def icon_daplink():
    """芯片 + 四边引脚 + 闪电（霓虹红 + 荧光黄）"""
    main = hexc(0xFF2A5C); sub = hexc(0xFFE94D)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # 芯片体（红圆角矩形）
    round_rect(cv, 10, 12, 30, 32, 3, 2, (main[0], main[1], main[2], 255))
    # 四边引脚短线
    for y in (14, 20, 26, 30):
        draw_line(cv, 7, y, 10, y, 2, (main[0], main[1], main[2], 255))
        draw_line(cv, 30, y, 33, y, 2, (main[0], main[1], main[2], 255))
    for x in (13, 19, 25):
        draw_line(cv, x, 9, x, 12, 2, (main[0], main[1], main[2], 255))
        draw_line(cv, x, 32, x, 35, 2, (main[0], main[1], main[2], 255))
    # 内部闪电（黄）
    poly_line(cv, [(20, 14), (16, 22), (20, 22), (18, 30)], 2, (sub[0], sub[1], sub[2], 255))
    for y in range(13, 32):
        for x in range(11, 30):
            if cv[y][x][3]:
                mask[y][x] = 1.0
    apply_glow(cv, mask, main)
    return cv

def icon_wave():
    """正弦波形 + 基线刻度（品红 + 荧光绿）"""
    main = hexc(0xFF00E5); sub = hexc(0x39FF88)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    sin_wave(cv, 7, 33, 20, 7, 1.5, 2, (main[0], main[1], main[2], 255))
    # 基线 + 刻度（绿）
    draw_line(cv, 7, 29, 33, 29, 1, (sub[0], sub[1], sub[2], 255))
    for x in (11, 16, 21, 26, 31):
        draw_line(cv, x, 27, x, 31, 1, (sub[0], sub[1], sub[2], 255))
    for y in range(10, 32):
        for x in range(8, 33):
            if cv[y][x][3]:
                mask[y][x] = 1.0
    apply_glow(cv, mask, main)
    return cv

def icon_scope():
    """示波器屏框 + 波形 + 网格点（荧光绿 + 琥珀）"""
    main = hexc(0x22FF88); sub = hexc(0xFFC857)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # 屏框（绿圆角矩形 + 顶栏）
    round_rect(cv, 7, 10, 33, 32, 3, 2, (main[0], main[1], main[2], 255))
    # 顶栏按钮（琥珀点）
    fill_circle(cv, 30, 13, 1, (sub[0], sub[1], sub[2], 255))
    fill_circle(cv, 26, 13, 1, (sub[0], sub[1], sub[2], 255))
    # 屏内波形（绿）
    sin_wave(cv, 10, 30, 22, 5, 1.25, 2, (main[0], main[1], main[2], 255))
    # 网格点（琥珀，稀疏）
    for gy in (17, 22, 27):
        for gx in (12, 18, 24, 29):
            if 12 <= gx <= 30 and 16 <= gy <= 28:
                setpx(cv, gx, gy, (sub[0], sub[1], sub[2], 255))
    for y in range(11, 32):
        for x in range(8, 33):
            if cv[y][x][3]:
                mask[y][x] = 1.0
    apply_glow(cv, mask, main)
    return cv

def icon_usb2ttl():
    """USB 插头 → 箭头 → 串口线头（紫 + 青）"""
    main = hexc(0xB14CFF); sub = hexc(0x00F0FF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # USB 插头（紫矩形 + 内部金属片）
    round_rect(cv, 5, 13, 16, 27, 2, 2, (main[0], main[1], main[2], 255))
    for y in range(16, 24):
        setpx(cv, 9, y, (main[0], main[1], main[2], 255)); mask[y][9] = 1.0
        setpx(cv, 12, y, (main[0], main[1], main[2], 255)); mask[y][12] = 1.0
    # 传输箭头（青）
    draw_line(cv, 19, 20, 29, 20, 2, (sub[0], sub[1], sub[2], 255))
    poly_line(cv, [(29, 20), (24, 17), (24, 23)], 2, (sub[0], sub[1], sub[2], 255))
    # 串口线头（青圆）
    stroke_circle(cv, 33, 20, 3, 2, (sub[0], sub[1], sub[2], 255))
    for y in range(14, 27):
        for x in range(6, 36):
            if cv[y][x][3]:
                mask[y][x] = 1.0
    apply_glow(cv, mask, main)
    return cv

ICONS = [
    ("files",    icon_files),
    ("reader",   icon_reader),
    ("terminal", icon_terminal),
    ("serialip", icon_serialip),
    ("cardr",    icon_cardr),
    ("dap",      icon_daplink),
    ("wave",     icon_wave),
    ("scope",    icon_scope),
    ("usb2ttl",  icon_usb2ttl),
]

def to_argb_bytes(cv):
    """LVGL ARGB8888：字节序 B,G,R,A"""
    out = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b, a = cv[y][x]
            out += bytes((b, g, r, a))
    return bytes(out)

def emit_c():
    parts = []
    parts.append('/* 由 launcher_icons_gen.py 生成的霓虹图标位图（光晕已烘入，非实时渲染） */')
    parts.append('#include "lvgl.h"')
    for name, fn in ICONS:
        cv = fn()
        data = to_argb_bytes(cv)
        parts.append('')
        parts.append('static const uint8_t icon_%s_data[%d] = {' % (name, len(data)))
        for i in range(0, len(data), 12):
            row = ", ".join("0x%02x" % b for b in data[i:i + 12])
            parts.append("    " + row + ",")
        parts.append('};')
        parts.append('const lv_image_dsc_t launcher_icon_%s = {' % name)
        parts.append('    .header.magic = LV_IMAGE_HEADER_MAGIC,')
        parts.append('    .header.cf = LV_COLOR_FORMAT_ARGB8888,')
        parts.append('    .header.flags = 0,')
        parts.append('    .header.w = 40,')
        parts.append('    .header.h = 40,')
        parts.append('    .header.stride = 160,')
        parts.append('    .data_size = %d,' % len(data))
        parts.append('    .data = icon_%s_data,' % name)
        parts.append('};')
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts) + "\n")
    print("written", OUT_C, len(parts), "lines")

# ---- 预览 PNG（纯标准库：zlib + struct）----
def png_chunk(tag, payload):
    c = struct.pack(">I", len(payload)) + tag + payload
    c += struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    return c

def write_png(path, img, scale=4):
    """img: list of rows (list of (r,g,b,a))"""
    h = len(img); w = len(img[0])
    sh, sw = h * scale, w * scale
    raw = bytearray()
    for y in range(sh):
        raw.append(0)  # filter None
        for x in range(sw):
            r, g, b, a = img[y // scale][x // scale]
            raw += bytes((r, g, b, a))
    ihdr = struct.pack(">IIBBBBB", sw, sh, 8, 6, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n"
    data += png_chunk(b"IHDR", ihdr)
    data += png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += png_chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)
    print("written", path, "%dx%d" % (sw, sh))

def emit_png():
    canvases = [fn() for _, fn in ICONS]
    # 3x3 网格（带黑色空隙），scale 4
    scale = 4
    pad = 8 * scale
    cell = 40 * scale + pad
    grid = [[(10, 10, 12, 255) for _ in range(cell * 3)] for _ in range(cell * 3)]
    for idx, cv in enumerate(canvases):
        gx, gy = idx % 3, idx // 3
        ox, oy = gx * cell + pad // 2, gy * cell + pad // 2
        for y in range(40 * scale):
            for x in range(40 * scale):
                grid[oy + y][ox + x] = cv[y // scale][x // scale]
    write_png(OUT_PNG, grid, scale=1)

if __name__ == "__main__":
    emit_c()
    emit_png()
