#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""launcher_icons_gen.py —— 生成桌面霓虹图标位图 launcher_icons.c + 预览 PNG
风格：赛博朋克霓虹灯。大块几何、低细节（40x40 像素下保证清晰），每图标
2 色左右（各 APP 不同主色），主体外带明显模糊霓虹光晕。
输出 LVGL ARGB8888 位图（字节序 B,G,R,A）40x40。"""

import math, struct, zlib, os

W = H = 40
OUT_C = os.path.join(os.path.dirname(__file__), "launcher_icons.c")
OUT_PNG = os.path.join(os.path.dirname(__file__), "..", "..", "build", "icons_preview.png")

def hexc(h):
    return ((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF, 255)

def new_canvas():
    return [[(0, 0, 0, 0) for _ in range(W)] for _ in range(H)]

def setpx(cv, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        cv[y][x] = c

def blend(cv, x, y, c):
    if not (0 <= x < W and 0 <= y < H):
        return
    r, g, b, a = cv[y][x]
    r2, g2, b2, a2 = c
    if a2 == 0:
        return
    na = a + (a2 * (255 - a) + 127) // 255
    if na == 0:
        return
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
    r0, r1 = r - w / 2.0, r + w / 2.0
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

def poly_fill(cv, pts, c):
    ys = [p[1] for p in pts]
    for y in range(max(0, int(math.floor(min(ys)))), min(H, int(math.ceil(max(ys))) + 1)):
        xs = []
        for i in range(len(pts)):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % len(pts)]
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / float(y1 - y0))
        if xs:
            xs.sort()
            for x in range(int(math.floor(xs[0])), int(math.ceil(xs[-1])) + 1):
                setpx(cv, x, y, c)

def poly_line(cv, pts, w, c):
    for i in range(len(pts) - 1):
        draw_line(cv, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], w, c)

def arc(cv, cx, cy, r, a0, a1, w, c, steps=40):
    pts = []
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    poly_line(cv, pts, w, c)

def round_rect_fill(cv, x0, y0, x1, y1, r, c):
    fill_rect(cv, x0 + r, y0, x1 - r, y1, c)
    fill_rect(cv, x0, y0 + r, x1, y1 - r, c)
    for (cx, cy) in [(x0 + r, y0 + r), (x1 - r, y0 + r), (x0 + r, y1 - r), (x1 - r, y1 - r)]:
        fill_circle(cv, cx, cy, r, c)

def sin_wave(cv, x0, x1, base_y, amp, period, w, c, phase=0.0):
    pts = []
    for x in range(x0, x1 + 1):
        y = base_y - amp * math.sin((x - x0) / float(x1 - x0) * 2 * math.pi * period + phase)
        pts.append((x, y))
    poly_line(cv, pts, w, c)

def box_blur_alpha(mask, iters=6):
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

def apply_glow(cv, shape_mask, main_color, strength=0.85):
    """两层霓虹光晕：大范围弱辉 + 近层亮辉"""
    glow = box_blur_alpha(shape_mask, iters=6)
    near = box_blur_alpha(shape_mask, iters=2)
    for y in range(H):
        for x in range(W):
            a = int(glow[y][x] * 255 * strength)
            if a > 0:
                blend(cv, x, y, (main_color[0], main_color[1], main_color[2], a))
            a2 = int(near[y][x] * 255 * 0.9)
            if a2 > 0:
                blend(cv, x, y, (main_color[0], main_color[1], main_color[2], a2))

def finish(cv, shape_mask, main):
    """光晕叠底后，主体再实色重画（更亮、更霓虹）"""
    apply_glow(cv, shape_mask, main)
    for y in range(H):
        for x in range(W):
            if shape_mask[y][x] > 0.5:
                setpx(cv, x, y, (main[0], main[1], main[2], 255))

def mark(cv, mask, x0, y0, x1, y1):
    """把矩形区域标入 mask（用于小部件光晕）"""
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            if cv[y][x][3]:
                mask[y][x] = 1.0

# ============ 9 个图标（大块几何、低细节、强光晕） ============

def icon_files():
    """文件夹：标签 + 主体 + 中缝（青 + 白）"""
    main = hexc(0x00F0FF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    # 顶部标签
    fill_rect(cv, 7, 9, 18, 14, white)
    # 主体（大圆角矩形）
    round_rect_fill(cv, 7, 14, 33, 31, 4, white)
    mark(cv, mask, 7, 9, 33, 31)
    finish(cv, mask, main)
    # 中缝（主色横线，主体之上）
    for x in range(11, 30):
        setpx(cv, x, 22, main)
    return cv

def icon_reader():
    """打开的书：两页 + 脊线 + 单行文字（粉 + 紫）"""
    main = hexc(0xFF5FA2); sub = hexc(0xB14CFF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    for y in range(10, 31):
        for x in range(8, 19):
            setpx(cv, x, y, white); mask[y][x] = 1.0
        for x in range(21, 32):
            setpx(cv, x, y, white); mask[y][x] = 1.0
    for y in range(9, 32):
        setpx(cv, 20, y, white); mask[y][20] = 1.0
    finish(cv, mask, main)
    # 文字行（紫粗）
    for x in range(11, 17):
        setpx(cv, x, 18, sub)
    for x in range(23, 29):
        setpx(cv, x, 18, sub)
    return cv

def icon_terminal():
    """终端窗口 + 粗 >_ 提示符（绿 + 白）"""
    main = hexc(0x39FF88); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 7, 10, 33, 30, 4, white)
    mark(cv, mask, 7, 10, 33, 30)
    finish(cv, mask, main)
    poly_line(cv, [(12, 17), (17, 20), (12, 23)], 3, white)
    draw_line(cv, 21, 23, 29, 23, 3, white)
    return cv

def icon_serialip():
    """串口接头 + 信号弧（品红 + 橙）"""
    main = hexc(0xFF00E5); sub = hexc(0xFF8C00); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    poly_fill(cv, [(13, 18), (27, 18), (25, 32), (15, 32)], white)
    mark(cv, mask, 13, 18, 27, 32)
    finish(cv, mask, main)
    fill_circle(cv, 17, 23, 2, white)
    fill_circle(cv, 23, 23, 2, white)
    arc(cv, 20, 18, 9, 180, 360, 3, sub)
    arc(cv, 20, 18, 14, 180, 360, 3, sub)
    return cv

def icon_cardr():
    white = hexc(0xFFFFFF)
    """U 盘：USB 头 + 卡体（金 + 品红）"""
    main = hexc(0xFFD700); sub = hexc(0xFF00E5)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 12, 13, 28, 32, 3, white)
    round_rect_fill(cv, 15, 7, 25, 14, 2, white)
    mark(cv, mask, 12, 7, 28, 32)
    finish(cv, mask, main)
    for y in range(9, 14):
        setpx(cv, 18, y, sub)
        setpx(cv, 22, y, sub)
    return cv

def icon_daplink():
    white = hexc(0xFFFFFF)
    """芯片 + 粗闪电（红 + 黄）"""
    main = hexc(0xFF2A5C); sub = hexc(0xFFE94D)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 11, 13, 29, 31, 3, white)
    for y in (16, 22, 28):
        draw_line(cv, 7, y, 11, y, 3, white)
        draw_line(cv, 29, y, 33, y, 3, white)
    for x in (14, 20, 26):
        draw_line(cv, x, 10, x, 13, 3, white)
        draw_line(cv, x, 31, x, 34, 3, white)
    mark(cv, mask, 7, 10, 33, 34)
    finish(cv, mask, main)
    poly_line(cv, [(20, 15), (16, 22), (20, 22), (17, 29)], 3, sub)
    return cv

def icon_wave():
    white = hexc(0xFFFFFF)
    """粗正弦波（品红）"""
    main = hexc(0xFF00E5)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    sin_wave(cv, 6, 34, 20, 8, 1.5, 3, white)
    mark(cv, mask, 6, 10, 34, 30)
    finish(cv, mask, main)
    return cv

def icon_scope():
    white = hexc(0xFFFFFF)
    """示波器屏 + 波形（绿 + 琥珀）"""
    main = hexc(0x22FF88); sub = hexc(0xFFC857)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 7, 9, 33, 31, 4, white)
    mark(cv, mask, 7, 9, 33, 31)
    finish(cv, mask, main)
    sin_wave(cv, 10, 30, 21, 5, 1.25, 3, white)
    fill_circle(cv, 29, 13, 1, sub)
    fill_circle(cv, 25, 13, 1, sub)
    return cv

def icon_usb2ttl():
    """USB 插头 → 箭头 → 串口圆头（紫 + 青）"""
    main = hexc(0xB14CFF); sub = hexc(0x00F0FF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 5, 14, 17, 27, 3, white)
    mark(cv, mask, 5, 14, 17, 27)
    finish(cv, mask, main)
    for y in range(17, 24):
        setpx(cv, 9, y, white)
        setpx(cv, 12, y, white)
    draw_line(cv, 20, 20, 29, 20, 3, sub)
    poly_line(cv, [(29, 20), (24, 16), (24, 24)], 3, sub)
    stroke_circle(cv, 34, 20, 3, 3, sub)
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
            parts.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
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
    print("written", OUT_C)

def png_chunk(tag, payload):
    c = struct.pack(">I", len(payload)) + tag + payload
    c += struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    return c

def write_png(path, img, scale=1):
    h = len(img); w = len(img[0])
    sh, sw = h * scale, w * scale
    raw = bytearray()
    for y in range(sh):
        raw.append(0)
        for x in range(sw):
            r, g, b, a = img[y // scale][x // scale]
            raw += bytes((r, g, b, a))
    ihdr = struct.pack(">IIBBBBB", sw, sh, 8, 6, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + png_chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)
    print("written", path, "%dx%d" % (sw, sh))

def emit_png():
    canvases = [fn() for _, fn in ICONS]
    scale = 4
    pad = 8 * scale
    cell = 40 * scale + pad
    grid = [[(12, 12, 16, 255) for _ in range(cell * 3)] for _ in range(cell * 3)]
    for idx, cv in enumerate(canvases):
        gx, gy = idx % 3, idx // 3
        ox, oy = gx * cell + pad // 2, gy * cell + pad // 2
        for y in range(40 * scale):
            for x in range(40 * scale):
                grid[oy + y][ox + x] = cv[y // scale][x // scale]
    write_png(OUT_PNG, grid)

if __name__ == "__main__":
    emit_c()
    emit_png()
