#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""launcher_icons_gen.py —— 生成桌面霓虹图标位图 launcher_icons.c + 预览 PNG
风格：赛博朋克霓虹灯。大块几何、低细节，主体居中缩小（四周留光晕空间），
主体外带明显径向模糊辉光（霓虹灯管感）。输出 LVGL ARGB8888 40x40。"""

import math, struct, zlib, os

W = H = 40
OUT_C = os.path.join(os.path.dirname(__file__), "launcher_icons.c")
OUT_PNG = os.path.join(os.path.dirname(__file__), "..", "..", "build", "icons_preview.png")

GLOW_R = 9          # 光晕半径（px），主体外 9px 辉光带
GLOW_PEAK = 150     # 近层光晕峰值 alpha

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

# 预计算径向辉光环：每圈 (alpha, offsets)
def build_rings():
    rings = []
    for d in range(1, GLOW_R + 1):
        cells = []
        for dy in range(-d, d + 1):
            for dx in range(-d, d + 1):
                if round(math.hypot(dx, dy)) == d:
                    cells.append((dx, dy))
        a = int(GLOW_PEAK * ((1.0 - d / (GLOW_R + 1.0)) ** 1.6))
        if a > 0:
            rings.append((a, cells))
    return rings

RINGS = build_rings()

def apply_glow(cv, shape_mask, main_color):
    """外部径向辉光：只向主体外扩散（目标在主体内部则跳过），
    保证图标主体清晰不糊，光晕呈"灯管照射四周"效果"""
    pts = [(x, y) for y in range(H) for x in range(W) if shape_mask[y][x] > 0.5]
    for (mx, my) in pts:
        for (a, cells) in RINGS:
            for (dx, dy) in cells:
                tx, ty = mx + dx, my + dy
                if not (0 <= tx < W and 0 <= ty < H):
                    continue
                if shape_mask[ty][tx] > 0.5:
                    continue   # 不画进主体内部
                blend(cv, tx, ty,
                      (main_color[0], main_color[1], main_color[2], a))

def apply_inner_rim(cv, shape_mask, white=60):
    """主体边缘内侧 1px 加低 alpha 白辉：表面"发光"感但清晰不糊"""
    for y in range(H):
        for x in range(W):
            if not shape_mask[y][x] > 0.5:
                continue
            edge = any(not (0 <= yy < H and 0 <= xx < W) or shape_mask[yy][xx] <= 0.5
                       for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)))
            if edge:
                blend(cv, x, y, (255, 255, 255, white))

def finish(cv, shape_mask, main):
    apply_glow(cv, shape_mask, main)
    for y in range(H):
        for x in range(W):
            if shape_mask[y][x] > 0.5:
                setpx(cv, x, y, (main[0], main[1], main[2], 255))
    apply_inner_rim(cv, shape_mask)

def mark(cv, mask, x0, y0, x1, y1):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            if cv[y][x][3]:
                mask[y][x] = 1.0

# ============ 9 个图标（主体居中，四周留光晕空间） ============

def icon_files():
    """文件夹：标签 + 主体 + 中缝（青 + 白）"""
    main = hexc(0x00F0FF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    fill_rect(cv, 9, 10, 19, 15, white)                # 标签
    round_rect_fill(cv, 9, 15, 31, 32, 3, white)       # 主体
    mark(cv, mask, 9, 10, 31, 32)
    finish(cv, mask, main)
    for x in range(13, 28):
        setpx(cv, x, 23, main)                         # 中缝（主色）
    return cv

def icon_reader():
    """打开的书：两页 + 脊线 + 单行文字（粉 + 紫）"""
    main = hexc(0xFF5FA2); sub = hexc(0xB14CFF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    for y in range(11, 31):
        for x in range(10, 19):
            setpx(cv, x, y, white); mask[y][x] = 1.0
        for x in range(21, 30):
            setpx(cv, x, y, white); mask[y][x] = 1.0
    for y in range(10, 32):
        setpx(cv, 20, y, white); mask[y][20] = 1.0
    finish(cv, mask, main)
    for x in range(13, 17):
        setpx(cv, x, 19, sub)
    for x in range(23, 27):
        setpx(cv, x, 19, sub)
    return cv

def icon_terminal():
    """终端窗口 + 粗 >_ 提示符（绿 + 白）"""
    main = hexc(0x39FF88); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 8, 11, 32, 30, 4, white)
    mark(cv, mask, 8, 11, 32, 30)
    finish(cv, mask, main)
    poly_line(cv, [(13, 18), (18, 21), (13, 24)], 3, white)
    draw_line(cv, 22, 24, 29, 24, 3, white)
    return cv

def icon_serialip():
    """串口接头 + 信号弧（品红 + 橙）"""
    main = hexc(0xFF00E5); sub = hexc(0xFF8C00); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    poly_fill(cv, [(13, 20), (27, 20), (25, 33), (15, 33)], white)
    mark(cv, mask, 13, 20, 27, 33)
    finish(cv, mask, main)
    fill_circle(cv, 17, 25, 2, white)
    fill_circle(cv, 23, 25, 2, white)
    arc(cv, 20, 20, 8, 180, 360, 3, sub)
    arc(cv, 20, 20, 13, 180, 360, 3, sub)
    return cv

def icon_cardr():
    """U 盘：USB 头 + 卡体（金 + 品红）"""
    main = hexc(0xFFD700); sub = hexc(0xFF00E5); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 13, 14, 29, 32, 3, white)      # 卡体
    round_rect_fill(cv, 16, 8, 26, 15, 2, white)       # USB 头
    mark(cv, mask, 13, 8, 29, 32)
    finish(cv, mask, main)
    for y in range(10, 15):
        setpx(cv, 19, y, sub)
        setpx(cv, 23, y, sub)
    return cv

def icon_daplink():
    """芯片 + 粗闪电（红 + 黄）"""
    main = hexc(0xFF2A5C); sub = hexc(0xFFE94D); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 12, 14, 30, 32, 3, white)      # 芯片体
    for y in (18, 23, 28):
        draw_line(cv, 8, y, 12, y, 3, white)
        draw_line(cv, 30, y, 34, y, 3, white)
    for x in (15, 20, 25):
        draw_line(cv, x, 11, x, 14, 3, white)
        draw_line(cv, x, 32, x, 35, 3, white)
    mark(cv, mask, 8, 11, 34, 35)
    finish(cv, mask, main)
    poly_line(cv, [(21, 16), (17, 23), (21, 23), (18, 30)], 3, sub)
    return cv

def icon_wave():
    """粗正弦波（品红）"""
    main = hexc(0xFF00E5); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    sin_wave(cv, 8, 32, 20, 8, 1.5, 3, white)
    mark(cv, mask, 8, 11, 32, 29)
    finish(cv, mask, main)
    return cv

def icon_scope():
    """示波器屏 + 波形（绿 + 琥珀）"""
    main = hexc(0x22FF88); sub = hexc(0xFFC857); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 8, 10, 32, 31, 4, white)
    mark(cv, mask, 8, 10, 32, 31)
    finish(cv, mask, main)
    sin_wave(cv, 11, 29, 22, 5, 1.25, 3, white)
    fill_circle(cv, 29, 14, 1, sub)
    fill_circle(cv, 25, 14, 1, sub)
    return cv

def icon_usb2ttl():
    """USB 插头 → 箭头 → 串口圆头（紫 + 青）"""
    main = hexc(0xB14CFF); sub = hexc(0x00F0FF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 6, 15, 18, 28, 3, white)       # USB 插头
    mark(cv, mask, 6, 15, 18, 28)
    finish(cv, mask, main)
    for y in range(18, 25):
        setpx(cv, 10, y, white)
        setpx(cv, 13, y, white)
    draw_line(cv, 21, 21, 29, 21, 3, sub)              # 箭头
    poly_line(cv, [(29, 21), (25, 17), (25, 25)], 3, sub)
    stroke_circle(cv, 33, 21, 3, 3, sub)               # 串口圆头
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
    scale = 6
    pad = 12 * scale
    cell = 40 * scale + pad
    grid = [[(10, 10, 14, 255) for _ in range(cell * 3)] for _ in range(cell * 3)]
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
