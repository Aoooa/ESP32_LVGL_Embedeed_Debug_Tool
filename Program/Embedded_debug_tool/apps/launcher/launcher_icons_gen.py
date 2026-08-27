#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""launcher_icons_gen.py —— 生成桌面霓虹图标位图 launcher_icons.c + 预览 PNG
风格：赛博朋克霓虹灯。60x60，大块几何、粗线条、极简清晰；主体居中，
四周留光晕空间；光晕仅贴主体边缘一层（灯管感，不糊主体）。
输出 LVGL ARGB8888 位图（字节序 B,G,R,A）60x60。"""

import math, struct, zlib, os

W = H = 60
OUT_C = os.path.join(os.path.dirname(__file__), "launcher_icons.c")
OUT_PNG = os.path.join(os.path.dirname(__file__), "..", "..", "build", "icons_preview.png")

GLOW_R = 4          # 光晕半径（px）：紧贴主体边缘一层
GLOW_PEAK = 130     # 近层光晕峰值 alpha

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

def arc(cv, cx, cy, r, a0, a1, w, c, steps=48):
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

# 预计算径向辉光环
def build_rings():
    rings = []
    for d in range(1, GLOW_R + 1):
        cells = []
        for dy in range(-d, d + 1):
            for dx in range(-d, d + 1):
                if round(math.hypot(dx, dy)) == d:
                    cells.append((dx, dy))
        a = int(GLOW_PEAK * ((1.0 - d / (GLOW_R + 1.0)) ** 2.5))
        if a > 0:
            rings.append((a, cells))
    return rings

RINGS = build_rings()

def apply_glow(cv, shape_mask, main_color):
    """外部径向辉光：只向主体外扩散（不画进主体内部），贴边一层"""
    pts = [(x, y) for y in range(H) for x in range(W) if shape_mask[y][x] > 0.5]
    for (mx, my) in pts:
        for (a, cells) in RINGS:
            for (dx, dy) in cells:
                tx, ty = mx + dx, my + dy
                if not (0 <= tx < W and 0 <= ty < H):
                    continue
                if shape_mask[ty][tx] > 0.5:
                    continue
                blend(cv, tx, ty,
                      (main_color[0], main_color[1], main_color[2], a))

def apply_inner_rim(cv, shape_mask, white=55):
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

# ============ 9 个图标（60x60，主体居中，极简清晰） ============

def icon_files():
    """文件夹：标签 + 主体 + 中缝（青 + 白）"""
    main = hexc(0x00F0FF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    fill_rect(cv, 13, 13, 31, 21, white)                # 标签
    round_rect_fill(cv, 13, 21, 47, 49, 5, white)       # 主体
    mark(cv, mask, 13, 13, 47, 49)
    finish(cv, mask, main)
    for x in range(20, 41):
        setpx(cv, x, 35, main)                          # 中缝
    return cv

def icon_reader():
    """打开的书：两页 + 脊线 + 单行文字（粉 + 紫）"""
    main = hexc(0xFF5FA2); sub = hexc(0xB14CFF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    for y in range(15, 48):
        for x in range(14, 29):
            setpx(cv, x, y, white); mask[y][x] = 1.0
        for x in range(31, 46):
            setpx(cv, x, y, white); mask[y][x] = 1.0
    for y in range(14, 49):
        setpx(cv, 30, y, white); mask[y][30] = 1.0
    finish(cv, mask, main)
    for x in range(17, 26):
        setpx(cv, x, 28, sub)
    for x in range(34, 43):
        setpx(cv, x, 28, sub)
    return cv

def icon_terminal():
    """终端窗口 + 粗 >_ 提示符（绿 + 白）"""
    main = hexc(0x39FF88); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 11, 15, 49, 47, 6, white)
    mark(cv, mask, 11, 15, 49, 47)
    finish(cv, mask, main)
    poly_line(cv, [(20, 26), (29, 31), (20, 36)], 5, white)
    draw_line(cv, 35, 36, 45, 36, 5, white)
    return cv

def icon_serialip():
    """串口接头 + 信号弧（品红 + 橙）"""
    main = hexc(0xFF00E5); sub = hexc(0xFF8C00); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    poly_fill(cv, [(18, 28), (42, 28), (39, 49), (21, 49)], white)
    mark(cv, mask, 18, 28, 42, 49)
    finish(cv, mask, main)
    fill_circle(cv, 25, 36, 3, white)
    fill_circle(cv, 35, 36, 3, white)
    arc(cv, 30, 28, 11, 180, 360, 4, sub)
    arc(cv, 30, 28, 17, 180, 360, 4, sub)
    return cv

def icon_cardr():
    """U 盘：USB 头 + 卡体（金 + 品红）"""
    main = hexc(0xFFD700); sub = hexc(0xFF00E5); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 19, 20, 43, 49, 4, white)       # 卡体
    round_rect_fill(cv, 23, 9, 39, 22, 3, white)        # USB 头
    mark(cv, mask, 19, 9, 43, 49)
    finish(cv, mask, main)
    for y in range(12, 21):
        setpx(cv, 28, y, sub)
        setpx(cv, 34, y, sub)
    return cv

def icon_daplink():
    """芯片 + 粗闪电（红 + 黄）"""
    main = hexc(0xFF2A5C); sub = hexc(0xFFE94D); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 17, 20, 43, 47, 4, white)       # 芯片体
    for y in (25, 31, 37, 43):
        draw_line(cv, 11, y, 17, y, 4, white)
        draw_line(cv, 43, y, 49, y, 4, white)
    for x in (22, 29, 36):
        draw_line(cv, x, 15, x, 20, 4, white)
        draw_line(cv, x, 47, x, 52, 4, white)
    mark(cv, mask, 11, 15, 49, 52)
    finish(cv, mask, main)
    poly_line(cv, [(31, 22), (24, 33), (30, 33), (26, 45)], 5, sub)
    return cv

def icon_wave():
    """粗正弦波（品红）"""
    main = hexc(0xFF00E5); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    sin_wave(cv, 10, 50, 30, 12, 1.5, 5, white)
    mark(cv, mask, 10, 16, 50, 44)
    finish(cv, mask, main)
    return cv

def icon_scope():
    """示波器屏 + 波形（绿 + 琥珀）"""
    main = hexc(0x22FF88); sub = hexc(0xFFC857); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 11, 13, 49, 49, 6, white)
    mark(cv, mask, 11, 13, 49, 49)
    finish(cv, mask, main)
    sin_wave(cv, 15, 45, 32, 8, 1.25, 5, white)
    fill_circle(cv, 43, 19, 2, sub)
    fill_circle(cv, 37, 19, 2, sub)
    return cv

def icon_usb2ttl():
    """USB 插头 → 箭头 → 串口圆头（紫 + 青）"""
    main = hexc(0xB14CFF); sub = hexc(0x00F0FF); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 8, 22, 27, 41, 4, white)        # USB 插头
    mark(cv, mask, 8, 22, 27, 41)
    finish(cv, mask, main)
    for y in range(26, 38):
        setpx(cv, 14, y, white)
        setpx(cv, 18, y, white)
    draw_line(cv, 31, 31, 43, 31, 5, sub)               # 箭头
    poly_line(cv, [(43, 31), (36, 24), (36, 38)], 5, sub)
    stroke_circle(cv, 50, 31, 5, 5, sub)                # 串口圆头
    return cv

def icon_webfs():
    """网络文件：地球（环 + 纬线 + 弧）+ 网络亮点（青蓝 + 绿）"""
    main = hexc(0x3EC6FF); sub = hexc(0x7DFF8C); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    stroke_circle(cv, 30, 30, 15, 5, white)             # 地球环
    arc(cv, 30, 30, 15, 200, 340, 3, white)             # 纬线
    arc(cv, 30, 30, 9, 20, 160, 3, white)               # 另一侧弧
    mark(cv, mask, 8, 8, 52, 52)
    finish(cv, mask, main)
    fill_circle(cv, 45, 16, 3, sub)                     # 网络亮点
    return cv

def icon_meter():
    """电压表：屏 + 波形 + MIN/MAX 刻度线（金 + 绿）"""
    main = hexc(0xFFC857); sub = hexc(0x22FF88); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 10, 12, 50, 50, 6, white)       # 仪表屏
    mark(cv, mask, 10, 12, 50, 50)
    finish(cv, mask, main)
    sin_wave(cv, 15, 45, 34, 8, 1.2, 4, sub)            # 波形（绿）
    draw_line(cv, 15, 22, 45, 22, 3, white)             # MAX 线
    for x in range(15, 46, 5):                          # MIN 虚线（底）
        draw_line(cv, x, 42, min(x + 2, 45), 42, 3, white)
    return cv

def icon_photo():
    """照片：相框 + 山形 + 太阳（青 + 琥珀）"""
    main = hexc(0x3EC6FF); sub = hexc(0xFFC857); white = hexc(0xFFFFFF)
    cv = new_canvas(); mask = [[0.0] * W for _ in range(H)]
    round_rect_fill(cv, 9, 13, 51, 49, 4, white)        # 相框
    mark(cv, mask, 9, 13, 51, 49)
    finish(cv, mask, main)
    fill_circle(cv, 38, 22, 3, sub)                     # 太阳
    poly_line(cv, [(13, 44), (24, 31), (33, 40), (43, 26), (48, 31), (48, 44)], 4, white)  # 山
    draw_line(cv, 13, 44, 48, 44, 4, white)
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
    ("webfs",    icon_webfs),
    ("meter",    icon_meter),
    ("photo",    icon_photo),
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
        parts.append('    .header.w = %d,' % W)
        parts.append('    .header.h = %d,' % H)
        parts.append('    .header.stride = %d,' % (W * 4))
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
    pad = 14 * scale
    cell = W * scale + pad
    cols = 3
    rows = (len(canvases) + cols - 1) // cols
    grid = [[(10, 10, 14, 255) for _ in range(cell * cols)] for _ in range(cell * rows)]
    for idx, cv in enumerate(canvases):
        gx, gy = idx % cols, idx // cols
        ox, oy = gx * cell + pad // 2, gy * cell + pad // 2
        for y in range(H * scale):
            for x in range(W * scale):
                grid[oy + y][ox + x] = cv[y // scale][x // scale]
    write_png(OUT_PNG, grid)

if __name__ == "__main__":
    emit_c()
    emit_png()
