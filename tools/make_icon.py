#!/usr/bin/env python3
"""生成 ChargeLimit 的 App 图标（绿色底、白色电池、电池里一道闪电）。

纯标准库，不依赖 Pillow。图标是满版方形：圆角由 iOS 自己裁，不要预先画圆角。
用法: python3 tools/make_icon.py app/icons
"""
import math
import os
import struct
import sys
import zlib

SIZES = {
    "AppIcon20x20@2x~ipad.png": 40,
    "AppIcon29x29@2x~ipad.png": 58,
    "AppIcon40x40@2x~ipad.png": 80,
    "AppIcon60x60@2x.png": 120,
    "AppIcon76x76@2x~ipad.png": 152,
    "AppIcon83.5x83.5@2x~ipad.png": 167,
}

# 闪电多边形（单位坐标，原点在左上）
BOLT = [(0.54, 0.34), (0.40, 0.60), (0.49, 0.60), (0.45, 0.78), (0.60, 0.52), (0.51, 0.52)]


def round_box(x, y, cx, cy, hx, hy, r):
    """圆角矩形的有向距离，小于 0 表示在内部。"""
    dx = abs(x - cx) - (hx - r)
    dy = abs(y - cy) - (hy - r)
    return math.hypot(max(dx, 0.0), max(dy, 0.0)) + min(max(dx, dy), 0.0) - r


def in_polygon(x, y, pts):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def pixel(x, y):
    t = y
    bg = (round(52 + (22 - 52) * t), round(199 + (150 - 199) * t), round(110 + (84 - 110) * t))
    col = bg
    if round_box(x, y, 0.5, 0.56, 0.19, 0.27, 0.05) < 0:      # 电池主体
        col = (255, 255, 255)
    if round_box(x, y, 0.5, 0.25, 0.07, 0.03, 0.015) < 0:     # 电池正极帽
        col = (255, 255, 255)
    if in_polygon(x, y, BOLT):                                # 闪电
        col = (30, 160, 90)
    return col


def render(size, ss=3):
    rows = []
    for j in range(size):
        row = bytearray([0])  # PNG 每行的滤波类型字节
        for i in range(size):
            r = g = b = 0
            for sj in range(ss):
                for si in range(ss):
                    pr, pg, pb = pixel((i + (si + 0.5) / ss) / size, (j + (sj + 0.5) / ss) / size)
                    r += pr; g += pg; b += pb
            n = ss * ss
            row += bytes((r // n, g // n, b // n))
        rows.append(bytes(row))
    return b"".join(rows)


def write_png(path, size, raw):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))   # 8 位 RGB，无透明通道
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "icons"
    os.makedirs(out, exist_ok=True)
    cache = {}
    for name, size in SIZES.items():
        if size not in cache:
            cache[size] = render(size)
        write_png(os.path.join(out, name), size, cache[size])
        print(f"{name} ({size}x{size})")


if __name__ == "__main__":
    main()
