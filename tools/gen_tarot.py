#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「电子塔罗牌」资源：22 张大阿尔克纳像素画 + 牌背 + 中文点阵字体。

依赖: Pillow（仅生成资源时需要，固件不需要）
字体源: C:/Windows/Fonts/simhei.ttf

用法（项目根目录）:
    python tools/gen_tarot.py            # 只生成 C 资源
    python tools/gen_tarot.py --preview  # 额外输出 tools/preview_tarot.png 方便检查画面

输出:
    main/tarot_data.h / main/tarot_data.c   30x40 原生 RGB565 卡面（运行时放大 4 倍）
    main/tarot_font.h / tarot_font_16.c     16px 中文（名称/释义/提示）
"""
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFont

W, H = 30, 40                      # 原生像素画尺寸（运行时 4x -> 120x160）
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "main")
FONT_TTF = r"C:\Windows\Fonts\simhei.ttf"
ASCII_CHARS = "".join(chr(c) for c in range(0x20, 0x7F))
UI_EXTRA = "·"                     # 名称分隔符


def C(h):
    return ((h >> 16) & 255, (h >> 8) & 255, h & 255)


def rgb565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


# ---------------------------------------------------------------- 像素画工具
class Art:
    def __init__(self):
        self.p = [[C(0x000000)] * W for _ in range(H)]

    def px(self, x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < W and 0 <= y < H:
            self.p[y][x] = c

    def grad(self, top, bot):
        for y in range(H):
            t = y / (H - 1)
            c = (int(top[0] + (bot[0] - top[0]) * t),
                 int(top[1] + (bot[1] - top[1]) * t),
                 int(top[2] + (bot[2] - top[2]) * t))
            for x in range(W):
                self.p[y][x] = c

    def rect(self, x0, y0, x1, y1, c, fill=True):
        for y in range(min(y0, y1), max(y0, y1) + 1):
            for x in range(min(x0, x1), max(x0, x1) + 1):
                if fill or x in (x0, x1) or y in (y0, y1):
                    self.px(x, y, c)

    def hline(self, x0, x1, y, c):
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.px(x, y, c)

    def vline(self, x, y0, y1, c):
        for y in range(min(y0, y1), max(y0, y1) + 1):
            self.px(x, y, c)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.px(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def disc(self, cx, cy, r, c):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r * 0.4:
                    self.px(x, y, c)

    def ring(self, cx, cy, r, c):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                d2 = (x - cx) ** 2 + (y - cy) ** 2
                if (r - 0.7) ** 2 <= d2 <= r * r + r * 0.4:
                    self.px(x, y, c)

    def ellipse_ring(self, cx, cy, rx, ry, c):
        for y in range(cy - ry, cy + ry + 1):
            for x in range(cx - rx, cx + rx + 1):
                v = (x - cx) ** 2 / (rx * rx) + (y - cy) ** 2 / (ry * ry)
                if 0.72 <= v <= 1.0:
                    self.px(x, y, c)

    def tri(self, x0, y0, x1, y1, x2, y2, c):
        pts = [(x0, y0), (x1, y1), (x2, y2)]
        minx, maxx = min(p[0] for p in pts), max(p[0] for p in pts)
        miny, maxy = min(p[1] for p in pts), max(p[1] for p in pts)
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                if point_in_tri((x, y), pts):
                    self.px(x, y, c)

    def star4(self, cx, cy, r, c):
        for i in range(1, r + 1):
            self.px(cx + i, cy, c)
            self.px(cx - i, cy, c)
            self.px(cx, cy + i, c)
            self.px(cx, cy - i, c)
        self.px(cx, cy, c)

    def star8(self, cx, cy, r, c):
        for i in range(1, r + 1):
            self.px(cx + i, cy, c)
            self.px(cx - i, cy, c)
            self.px(cx, cy + i, c)
            self.px(cx, cy - i, c)
        for i in range(1, r // 2 + 1):
            self.px(cx + i, cy + i, c)
            self.px(cx - i, cy + i, c)
            self.px(cx + i, cy - i, c)
            self.px(cx - i, cy - i, c)
        self.disc(cx, cy, 1, c)

    def crescent(self, cx, cy, r, c, dx=2):
        for dy in range(-r, r + 1):
            ox = int(math.sqrt(max(0.0, r * r - dy * dy)))
            for x in range(cx - ox, cx + ox + 1):
                if dy * dy < (r - 1) * (r - 1):
                    ix = int(math.sqrt(max(0.0, (r - 1) ** 2 - dy * dy)))
                    if cx + dx - ix <= x <= cx + dx + ix:
                        continue
                self.px(x, cy + dy, c)

    def bitmap(self, cx, cy, rows, c):
        w = len(rows[0])
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch not in (" ", "."):
                    self.px(cx - w // 2 + i, cy + j, c)


def point_in_tri(p, pts):
    def sign(a, b, c):
        return (a[0] - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (a[1] - c[1])
    d1 = sign(p, pts[0], pts[1])
    d2 = sign(p, pts[1], pts[2])
    d3 = sign(p, pts[2], pts[0])
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


FONT5x7 = {
    "I": [".X...", ".X...", ".X...", ".X...", ".X...", ".X...", ".X..."],
    "V": ["X...X", "X...X", "X...X", "X...X", "X...X", ".X.X.", "..X.."],
    "X": ["X...X", "X...X", ".X.X.", "..X..", ".X.X.", "X...X", "X...X"],
    "0": [".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."],
}


def roman(n):
    if n == 0:
        return "0"
    out = ""
    for v, s in [(10, "X"), (9, "IX"), (5, "V"), (4, "IV"), (1, "I")]:
        while n >= v:
            out += s
            n -= v
    return out


def numeral(a, text, y, c):
    step = 6
    total = len(text) * step - 1
    x = (W - total) // 2
    for ch in text:
        a.bitmap(x + 2, y, FONT5x7[ch], c)
        x += step


def base(top, bot, accent, num, star_c):
    """通用卡框：渐变夜空 + 星尘 + 金色内框 + 罗马数字 + 地平线"""
    a = Art()
    a.grad(C(top), C(bot))

    # 固定位置的细碎星光（避免随机带来的不确定）
    for (x, y) in [(4, 10), (26, 13), (6, 21), (24, 22), (5, 30), (25, 30),
                   (9, 8), (21, 8), (13, 34), (17, 34), (3, 17), (27, 17)]:
        a.px(x, y, star_c)

    # 内框（双层：外金、内浅）
    a.rect(2, 2, 27, 37, C(accent), fill=False)
    a.rect(3, 3, 26, 36, C(0x6B5A33), fill=False)
    # 四角饰点
    for (x, y) in [(5, 5), (24, 5), (5, 34), (24, 34)]:
        a.px(x, y, C(accent))

    numeral(a, num, 3, C(accent))
    a.hline(4, 25, 33, C(0x4A3F26))
    return a


# ---------------------------------------------------------------- 22 张牌面
GOLD = 0xE7C878
CREAM = 0xF2E3B8
SKIN = 0xE9D7B6


def draw_fool(a):
    a.star8(8, 12, 3, C(0xF7E3A0))
    # 悬崖
    a.rect(3, 27, 11, 32, C(0x1F2A44))
    a.hline(3, 11, 27, C(0x3A5178))
    # 人物
    a.disc(18, 18, 2, C(SKIN))
    a.tri(15, 29, 21, 29, 18, 20, C(0x3F6FB5))
    a.line(16, 29, 14, 32, C(0x2C3E5E))
    a.line(20, 29, 22, 32, C(0x2C3E5E))
    a.line(14, 21, 11, 31, C(0xB08D57))
    a.rect(9, 17, 12, 20, C(0x8A6A3A))
    a.px(24, 30, C(0x9AA7BC))
    a.px(25, 30, C(0x9AA7BC))


def draw_magician(a):
    a.ring(12, 12, 2, C(CREAM))
    a.ring(17, 12, 2, C(CREAM))
    a.line(13, 30, 19, 12, C(0xD9B86A))
    a.star4(20, 11, 2, C(0xFFFFFF))
    a.rect(6, 28, 23, 29, C(0x7E5A33))
    a.vline(8, 29, 32, C(0x5A4524))
    a.vline(21, 29, 32, C(0x5A4524))
    for x, c in [(9, 0xE2C275), (13, 0xCF5B5B), (17, 0x5BCF9E), (21, 0x5B8FCF)]:
        a.px(x, 27, C(c))
    a.tri(11, 31, 20, 31, 15, 22, C(0x7A2E3B))
    a.disc(15, 21, 1, C(SKIN))


def draw_priestess(a):
    a.rect(6, 15, 9, 31, C(0x2C4A5E))
    a.vline(7, 15, 31, C(0x4A7590))
    a.rect(20, 15, 23, 31, C(0x2C4A5E))
    a.vline(22, 15, 31, C(0x4A7590))
    a.crescent(15, 18, 4, C(0xF0E3A8))
    a.star4(15, 25, 2, C(0xD8C98F))
    for y in (27, 29, 31):
        a.hline(11, 19, y, C(0x2E4256))


def draw_empress(a):
    a.ring(15, 17, 4, C(0xF0C86E))
    a.vline(15, 21, 30, C(0xF0C86E))
    a.hline(11, 19, 27, C(0xF0C86E))
    # 麦穗
    for sx in (6, 23):
        a.vline(sx, 22, 31, C(0xC9A24B))
        for y in (23, 26, 29):
            a.px(sx - 1, y, C(0xE7C878))
            a.px(sx + 1, y, C(0xE7C878))
    # 小皇冠
    a.hline(11, 19, 12, C(0xF0C86E))
    for x in (11, 15, 19):
        a.px(x, 11, C(0xF0C86E))


def draw_emperor(a):
    a.rect(8, 16, 22, 31, C(0x5A2A2E), fill=False)
    a.rect(9, 17, 21, 30, C(0x241014))
    # 羊角
    a.line(11, 14, 7, 10, C(0xE0B85C))
    a.line(19, 14, 23, 10, C(0xE0B85C))
    # 王冠
    a.hline(10, 20, 25, C(0xE7C878))
    for x in (10, 15, 20):
        a.px(x, 22, C(0xE7C878))
        a.px(x, 21, C(0xE7C878))
    a.px(12, 23, C(0xE7C878))
    a.px(18, 23, C(0xE7C878))
    # 权杖
    a.vline(15, 18, 24, C(0xE7C878))
    a.star4(15, 17, 2, C(CREAM))


def draw_hierophant(a):
    a.vline(15, 11, 31, C(0xE3C887))
    a.hline(11, 19, 14, C(0xE3C887))
    a.hline(10, 20, 18, C(0xE3C887))
    a.hline(10, 20, 22, C(0xE3C887))
    a.line(10, 31, 20, 27, C(0xC9D3E2))
    a.line(20, 31, 10, 27, C(0xC9D3E2))
    for (x, y) in [(10, 27), (20, 27), (10, 31), (20, 31)]:
        a.px(x, y, C(0x9FB0C8))


def draw_lovers(a):
    a.disc(15, 11, 3, C(0xF0C070))
    a.bitmap(15, 18, [" XX XX ", "XXXXXXX", "XXXXXXX", " XXXXX ", "  XXX  ", "   X   "], C(0xD9534F))
    a.disc(10, 17, 2, C(SKIN))
    a.rect(8, 19, 12, 27, C(0x5B7FBF))
    a.disc(20, 17, 2, C(SKIN))
    a.rect(18, 19, 22, 27, C(0xA05C8A))


def draw_chariot(a):
    a.rect(8, 12, 22, 19, C(0x2C4A72), fill=False)
    a.rect(9, 13, 21, 18, C(0x18253C))
    for x in (11, 15, 19):
        a.px(x, 15, C(0xE8C46A))
    a.rect(10, 19, 20, 22, C(0x8A6A3A))
    a.ring(15, 27, 5, C(0xE8C46A))
    for (dx, dy) in [(0, -4), (0, 4), (-4, 0), (4, 0), (3, 3), (-3, 3), (3, -3), (-3, -3)]:
        a.px(15 + dx, 27 + dy, C(0xE8C46A))
    a.disc(15, 27, 1, C(0xF7E3A0))


def draw_strength(a):
    a.ring(12, 13, 2, C(CREAM))
    a.ring(17, 13, 2, C(CREAM))
    # 鬃毛
    for (dx, dy) in [(0, -7), (0, 7), (-7, 0), (7, 0), (5, -5), (5, 5), (-5, -5), (-5, 5)]:
        a.px(15 + dx, 22 + dy, C(0xB8862F))
        a.px(15 + dx * 5 // 4, 22 + dy * 5 // 4, C(0xB8862F))
    a.disc(15, 22, 5, C(0xD9A54B))
    # 耳朵
    a.px(11, 17, C(0xB8862F)); a.px(12, 17, C(0xB8862F))
    a.px(18, 17, C(0xB8862F)); a.px(19, 17, C(0xB8862F))
    a.px(13, 21, C(0x2A1A08))
    a.px(17, 21, C(0x2A1A08))
    a.px(15, 23, C(0x7A4A10))
    a.hline(13, 17, 25, C(0x2A1A08))


def draw_hermit(a):
    a.tri(8, 31, 22, 31, 15, 14, C(0x2E4A44))
    a.disc(15, 18, 2, C(0x14201E))
    a.rect(19, 16, 23, 21, C(0xB08D57), fill=False)
    a.disc(21, 18, 1, C(0xFFF3C4))
    a.star4(21, 18, 2, C(0xFFF3C4))
    a.vline(21, 21, 26, C(0x8A6A3A))


def draw_wheel(a):
    a.ring(15, 21, 8, C(0xE8C46A))
    a.ring(15, 21, 7, C(0xB08D57))
    for (dx, dy) in [(0, -8), (0, 8), (-8, 0), (8, 0), (5, 5), (-5, 5), (5, -5), (-5, -5)]:
        a.line(15, 21, 15 + dx, 21 + dy, C(0xB08D57))
    a.disc(15, 21, 2, C(0xE8C46A))
    for (x, y) in [(5, 12), (25, 12), (5, 30), (25, 30)]:
        a.px(x, y, C(0x9FD8F0))


def draw_justice(a):
    a.vline(15, 10, 32, C(0xC9D3E2))
    a.hline(11, 19, 20, C(0xC9D3E2))
    a.disc(15, 10, 1, C(0xF2E3B8))
    for side in (-1, 1):
        cx = 15 + side * 6
        a.ring(cx, 16, 2, C(0xE6D9B8))
        a.line(cx, 18, cx - 3, 22, C(0x8A7F5E))
        a.line(cx, 18, cx + 3, 22, C(0x8A7F5E))
        a.hline(cx - 3, cx + 3, 23, C(0xE6D9B8))


def draw_hanged(a):
    a.hline(5, 25, 10, C(0xB08D57))
    a.vline(6, 10, 16, C(0x8A6A3A))
    a.vline(15, 10, 17, C(0xD9B86A))
    # 倒吊的人：脚在上，头在下
    a.line(15, 17, 11, 15, C(0x2C3E5E))
    a.line(15, 17, 19, 15, C(0x2C3E5E))
    a.rect(13, 18, 17, 25, C(0x4C6FB0))
    a.line(13, 20, 10, 26, C(0x3F6FB5))
    a.line(17, 20, 20, 26, C(0x3F6FB5))
    a.disc(15, 28, 2, C(SKIN))


def draw_death(a):
    a.disc(14, 20, 5, C(0xD8D3E8))
    a.px(12, 19, C(0x16121C))
    a.px(16, 19, C(0x16121C))
    a.px(14, 22, C(0x16121C))
    a.rect(11, 24, 17, 26, C(0xC9C2D8))
    for x in (12, 14, 16):
        a.px(x, 25, C(0x16121C))
    # 镰刀
    a.line(21, 30, 22, 14, C(0x8A7F5E))
    a.crescent(14, 13, 4, C(0xC9D3E2), dx=-3)


def draw_temperance(a):
    # 两只圣杯，水从上杯流向下杯
    def cup(cx, cy, c):
        a.hline(cx - 3, cx + 3, cy, c)
        a.hline(cx - 2, cx + 2, cy + 1, c)
        a.hline(cx - 1, cx + 1, cy + 2, c)
        a.vline(cx, cy + 3, cy + 4, c)
        a.hline(cx - 2, cx + 2, cy + 5, c)

    cup(10, 13, C(0xE7C878))
    cup(20, 21, C(0xE7C878))
    for i in range(6):
        a.px(11 + i, 19 + i, C(0x9FE0D0))
        a.px(12 + i, 19 + i, C(0x9FE0D0))
    a.hline(19, 22, 27, C(0x5FCFB8))
    # 翅膀
    for i in range(4):
        a.line(20 + i, 11 - i, 24 + i, 7 - i, C(0xCFE8E2))


def draw_devil(a):
    # 倒五芒星
    pts = []
    for k in range(5):
        ang = math.radians(90 + k * 72)
        pts.append((15 + 8 * math.cos(ang), 20 + 8 * math.sin(ang)))
    for k in range(5):
        p, q = pts[k], pts[(k + 2) % 5]
        a.line(int(p[0]), int(p[1]), int(q[0]), int(q[1]), C(0xD14B4B))
    a.disc(15, 20, 1, C(0xF0A0A0))
    # 角
    a.line(12, 10, 9, 7, C(0x8A2E2E))
    a.line(18, 10, 21, 7, C(0x8A2E2E))
    # 锁链
    for i in range(5):
        a.px(5 + (i % 2), 21 + i * 2, C(0x9AA7BC))
        a.px(25 - (i % 2), 21 + i * 2, C(0x9AA7BC))


def draw_tower(a):
    a.rect(11, 17, 19, 31, C(0x4A3A50))
    a.rect(11, 17, 19, 31, C(0x8A6A3A), fill=False)
    for x in (11, 14, 17, 19):
        a.px(x, 16, C(0x4A3A50))
    a.rect(14, 26, 16, 31, C(0x1A1218))
    a.line(21, 9, 15, 17, C(0xFFE9A8))
    a.line(15, 17, 19, 20, C(0xFFE9A8))
    a.px(24, 23, C(0xFFE9A8))
    a.px(8, 21, C(0xFFE9A8))


def draw_star(a):
    a.star8(15, 16, 6, C(0xBFE8F8))
    a.star4(15, 16, 3, C(0xFFFFFF))
    a.star4(7, 11, 2, C(0x9FD8F0))
    a.star4(23, 11, 2, C(0x9FD8F0))
    for y in (29, 31):
        a.hline(9, 21, y, C(0x5A8FB8))


def draw_moon(a):
    a.crescent(15, 16, 5, C(0xE2ECFA))
    a.px(13, 16, C(0xC8D6EE))
    a.px(17, 16, C(0xC8D6EE))
    a.rect(4, 24, 8, 31, C(0x3A4A66))
    a.rect(22, 24, 26, 31, C(0x3A4A66))
    for (x, y) in [(15, 31), (13, 29), (15, 27), (17, 25)]:
        a.px(x, y, C(0xD8C98F))


def draw_sun(a):
    a.disc(15, 19, 6, C(0xFFD24A))
    a.disc(15, 19, 4, C(0xFFE99B))
    for (dx, dy) in [(0, -9), (0, 9), (-9, 0), (9, 0), (6, -6), (6, 6), (-6, -6), (-6, 6)]:
        a.px(15 + dx, 19 + dy, C(0xFFC93C))
        a.px(15 + dx * 4 // 5, 19 + dy * 4 // 5, C(0xFFC93C))
    a.hline(4, 25, 31, C(0xB07A2A))


def draw_judgement(a):
    a.line(8, 10, 18, 22, C(0xEBD9A8))
    a.rect(6, 8, 9, 12, C(0xEBD9A8))
    for i in range(3):
        a.line(20 + i, 16 - i, 24 + i, 12 - i, C(0xCFE8E2))
    for (x, y) in [(11, 26), (15, 24), (19, 26)]:
        a.disc(x, y, 1, C(0xD8C9A8))
        a.vline(x, y + 2, 31, C(0x5A6E8A))


def draw_world(a):
    a.ellipse_ring(15, 20, 8, 6, C(0x8FD8A8))
    a.disc(15, 20, 4, C(0x3F8F8A))
    a.hline(11, 19, 19, C(0x9FE0C0))
    a.hline(11, 19, 22, C(0x9FE0C0))
    a.vline(15, 16, 24, C(0x9FE0C0))
    for (x, y) in [(5, 11), (25, 11), (5, 29), (25, 29)]:
        a.px(x, y, C(0xE7C878))


CARDS = [
    ("愚者", "放下顾虑出发，未知正是礼物", "鲁莽冒进，先看清脚下的路",
     0x101830, 0x070A14, 0xE7C878, "0", draw_fool),
    ("魔术师", "资源就在手边，动手就能成形", "想法多于行动，小心自我欺骗",
     0x14102A, 0x090718, 0xE7C878, "I", draw_magician),
    ("女祭司", "静下来倾听直觉，答案在水面下", "忽视内心声音，被表象牵着走",
     0x0B1A24, 0x05090F, 0xD8C98F, "II", draw_priestess),
    ("皇后", "丰盛与滋养正在流向你", "过度付出，先照顾自己",
     0x1E0F2A, 0x0A0512, 0xF0C86E, "III", draw_empress),
    ("皇帝", "立好规矩与边界，秩序带来自由", "控制欲过强，学会适度松手",
     0x1A0E1C, 0x08040A, 0xE0B85C, "IV", draw_emperor),
    ("教皇", "向传统与师长借一点智慧", "规则未必适合你，可以另辟道路",
     0x1A1426, 0x08060E, 0xE3C887, "V", draw_hierophant),
    ("恋人", "听从真心，选让你发亮的那边", "犹豫摇摆，先弄清自己要什么",
     0x2A1024, 0x0C0410, 0xF0C070, "VI", draw_lovers),
    ("战车", "握紧方向，多头拉车也能前进", "用力过猛，先停下校准方向",
     0x101C2C, 0x060A12, 0xE8C46A, "VII", draw_chariot),
    ("力量", "温柔比强硬更有力量", "先安抚心里的那头兽",
     0x1C1808, 0x0A0804, 0xF0C868, "VIII", draw_strength),
    ("隐者", "独处一会儿，灯在你自己手里", "闭关太久，别与世界断线",
     0x0E1A18, 0x05090A, 0xE8CE8A, "IX", draw_hermit),
    ("命运之轮", "转机正在出现，顺势而为", "低潮只是轮子的一半",
     0x141A2C, 0x070A12, 0xE8C46A, "X", draw_wheel),
    ("正义", "秤会归位，诚实是最好的筹码", "有笔账没算清，先面对它",
     0x121A28, 0x060910, 0xE6D9B8, "XI", draw_justice),
    ("倒吊人", "换个角度看，暂停也是进展", "无谓的牺牲，别再硬撑",
     0x121A2E, 0x06090F, 0xE3C88E, "XII", draw_hanged),
    ("死神", "旧章节结束，腾出新的空间", "抗拒告别，反而拖得更久",
     0x16121C, 0x060408, 0xD8D3E8, "XIII", draw_death),
    ("节制", "调配好比例，慢慢就会合拍", "用力不均，先找回节奏",
     0x0C1A20, 0x050A0D, 0x9FE0D0, "XIV", draw_temperance),
    ("恶魔", "看清欲望的绳索，你能解开", "松绑的时候到了，别再交换自由",
     0x220A12, 0x0A0406, 0xD14B4B, "XV", draw_devil),
    ("高塔", "地基在震，塌掉的是假的东西", "危机正在过去，别急着重建",
     0x1A1420, 0x07050A, 0xE8C46A, "XVI", draw_tower),
    ("星星", "希望重新亮起，愿望值得等待", "信念蒙尘，先擦亮一小颗星",
     0x0A1424, 0x04070E, 0x9FD8F0, "XVII", draw_star),
    ("月亮", "雾里看不清，别凭恐惧下判断", "雾在散，真相慢慢浮出水面",
     0x0A1420, 0x04070C, 0xD8E2F0, "XVIII", draw_moon),
    ("太阳", "坦荡明亮，今天适合走到台前", "光被遮住一会儿，仍会回来",
     0x241604, 0x0C0702, 0xFFD24A, "XIX", draw_sun),
    ("审判", "听见召唤，旧账可以清算了", "迟迟不肯回应，机会会冷掉",
     0x141A26, 0x06090F, 0xEBD9A8, "XX", draw_judgement),
    ("世界", "一圈走完，你已完整", "差最后一步，别在此收手",
     0x0C1A14, 0x040A08, 0x9FE0B0, "XXI", draw_world),
]


def card_back():
    a = Art()
    a.grad(C(0x1A1234), C(0x07040E))
    # 双层边框 + 角饰
    a.rect(2, 2, 27, 37, C(0xC9A24B), fill=False)
    a.rect(4, 4, 25, 35, C(0x6B5A8A), fill=False)
    for (x, y) in [(6, 6), (23, 6), (6, 33), (23, 33)]:
        a.px(x, y, C(0xE7C878))
    # 中心徽记
    a.star8(15, 20, 7, C(0xE7C878))
    a.star4(15, 20, 4, C(0xF7EBC8))
    a.crescent(15, 20, 4, C(0x9A8CC8), dx=-3)
    a.disc(15, 20, 1, C(0xFFF6DA))
    # 斜纹点阵
    for i in range(3, 27, 3):
        a.px(i, 9, C(0x4A3F6B))
        a.px(i, 31, C(0x4A3F6B))
    for j in range(11, 31, 4):
        a.px(4, j, C(0x4A3F6B))
        a.px(25, j, C(0x4A3F6B))
    return a


# ---------------------------------------------------------------- C 输出
def art_to_c(a):
    vals = [rgb565(a.p[y][x]) for y in range(H) for x in range(W)]
    out = []
    for i in range(0, len(vals), 16):
        out.append("    " + ",".join(f"0x{v:04X}" for v in vals[i:i + 16]) + ",")
    return "\n".join(out)


def write_data(cards, back):
    h = []
    h.append("/** 自动生成: tools/gen_tarot.py —— 大阿尔克纳像素画（30x40 RGB565） */")
    h.append("#pragma once")
    h.append("")
    h.append("#include <stdint.h>")
    h.append("")
    h.append("#define TAROT_ART_W 30")
    h.append("#define TAROT_ART_H 40")
    h.append("#define TAROT_COUNT 22")
    h.append("")
    h.append("typedef struct {")
    h.append("    const char *name;      /* 牌名 */")
    h.append("    const char *upright;   /* 正位释义 */")
    h.append("    const char *reversed;  /* 逆位释义 */")
    h.append("    const uint16_t art[TAROT_ART_W * TAROT_ART_H];")
    h.append("} tarot_card_t;")
    h.append("")
    h.append("extern const tarot_card_t tarot_cards[TAROT_COUNT];")
    h.append("extern const uint16_t tarot_card_back[TAROT_ART_W * TAROT_ART_H];")
    open(os.path.join(OUT_DIR, "tarot_data.h"), "w", encoding="utf-8").write("\n".join(h) + "\n")

    c = []
    c.append("/** 自动生成: tools/gen_tarot.py，勿手改 */")
    c.append('#include "tarot_data.h"')
    c.append("")
    c.append("const tarot_card_t tarot_cards[TAROT_COUNT] = {")
    for name, up, rev, *_rest in cards:
        art = _rest[-1]
        c.append("  {")
        c.append(f'    "{name}",')
        c.append(f'    "{up}",')
        c.append(f'    "{rev}",')
        c.append("    {")
        c.append(art_to_c(art))
        c.append("    },")
        c.append("  },")
    c.append("};")
    c.append("")
    c.append("const uint16_t tarot_card_back[TAROT_ART_W * TAROT_ART_H] = {")
    c.append(art_to_c(back))
    c.append("};")
    open(os.path.join(OUT_DIR, "tarot_data.c"), "w", encoding="utf-8").write("\n".join(c) + "\n")

    print(f"tarot_data.c: {len(cards)} cards + back, {len(cards) * W * H * 2 + W * H * 2} bytes RGB565")


# ---------------------------------------------------------------- 字体
def render_glyph(font, ch, size):
    adv = int(round(font.getlength(ch)))
    if ch == " ":
        return adv, 0, 0, 0, 0, b""
    bbox = font.getbbox(ch, anchor="ls")
    left, top, right, bottom = bbox
    w, h = right - left, bottom - top
    if w <= 0 or h <= 0:
        return adv, 0, 0, 0, 0, b""
    canvas = Image.new("L", (size * 4, size * 4), 0)
    ImageDraw.Draw(canvas).text((size, size), ch, font=font, fill=255, anchor="ls")
    crop = canvas.crop((size + left, size + top, size + right, size + bottom))
    return adv, left, (-top) - h, w, h, crop.tobytes()


def write_font(chars, size, name, out_path):
    font = ImageFont.truetype(FONT_TTF, size)
    ascent, descent = font.getmetrics()

    glyphs, bitmaps = [], bytearray()
    for ch in chars:
        adv, ox, oy, w, h, data = render_glyph(font, ch, size)
        glyphs.append((ord(ch), adv, ox, oy, w, h, len(bitmaps)))
        bitmaps += data

    L = []
    L.append(f"/* 自动生成: tools/gen_tarot.py  size={size}  glyphs={len(glyphs)}  bitmap={len(bitmaps)}B */")
    L.append("#include <string.h>")
    L.append('#include "tarot_font.h"')
    L.append("")
    L.append("typedef struct {")
    L.append("    uint32_t cp;")
    L.append("    uint16_t adv;")
    L.append("    int16_t ofs_x, ofs_y;")
    L.append("    uint8_t w, h;")
    L.append("    uint32_t data_ofs;")
    L.append("} glyph_t;")
    L.append("")
    L.append("static const uint8_t s_bitmap[] = {")
    for i in range(0, len(bitmaps), 16):
        L.append("    " + ",".join(f"0x{b:02X}" for b in bitmaps[i:i + 16]) + ",")
    L.append("};")
    L.append("")
    L.append("static const glyph_t s_glyphs[] = {")
    for cp, adv, ox, oy, w, h, ofs in glyphs:
        L.append(f"    {{0x{cp:04X}, {adv}, {ox}, {oy}, {w}, {h}, {ofs}}},  /* {chr(cp)} */")
    L.append("};")
    L.append("")
    L.append("static bool font_get_glyph_dsc(const lv_font_t * font, lv_font_glyph_dsc_t * dsc,")
    L.append("                                  uint32_t letter, uint32_t letter_next)")
    L.append("{")
    L.append("    LV_UNUSED(font); LV_UNUSED(letter_next);")
    L.append("    int lo = 0, hi = (int)(sizeof(s_glyphs) / sizeof(s_glyphs[0])) - 1;")
    L.append("    while (lo <= hi) {")
    L.append("        int mid = (lo + hi) / 2;")
    L.append("        if (s_glyphs[mid].cp == letter) {")
    L.append("            dsc->adv_w = s_glyphs[mid].adv;")
    L.append("            dsc->box_w = s_glyphs[mid].w;")
    L.append("            dsc->box_h = s_glyphs[mid].h;")
    L.append("            dsc->ofs_x = s_glyphs[mid].ofs_x;")
    L.append("            dsc->ofs_y = s_glyphs[mid].ofs_y;")
    L.append("            dsc->format = LV_FONT_GLYPH_FORMAT_A8;")
    L.append("            dsc->is_placeholder = false;")
    L.append("            dsc->gid.index = (uint32_t)mid;")
    L.append("            return true;")
    L.append("        }")
    L.append("        if (s_glyphs[mid].cp < letter) lo = mid + 1; else hi = mid - 1;")
    L.append("    }")
    L.append("    return false;")
    L.append("}")
    L.append("")
    L.append("static const void * font_get_glyph_bitmap(lv_font_glyph_dsc_t * dsc, lv_draw_buf_t * draw_buf)")
    L.append("{")
    L.append("    const glyph_t * g = &s_glyphs[dsc->gid.index];")
    L.append("    if (g->w == 0 || g->h == 0) return NULL;")
    L.append("    uint32_t stride = draw_buf->header.stride;")
    L.append("    const uint8_t * src = s_bitmap + g->data_ofs;")
    L.append("    for (int y = 0; y < g->h; y++) {")
    L.append("        memcpy(draw_buf->data + y * stride, src + y * g->w, g->w);")
    L.append("    }")
    L.append("    return draw_buf;")
    L.append("}")
    L.append("")
    L.append(f"const lv_font_t {name} = {{")
    L.append("    .get_glyph_dsc = font_get_glyph_dsc,")
    L.append("    .get_glyph_bitmap = font_get_glyph_bitmap,")
    L.append(f"    .line_height = {ascent + descent},")
    L.append(f"    .base_line = {descent},")
    L.append("    .subpx = LV_FONT_SUBPX_NONE,")
    L.append("    .kerning = LV_FONT_KERNING_NONE,")
    L.append("    .underline_position = -2,")
    L.append("    .underline_thickness = 1,")
    L.append("};")

    open(out_path, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print(f"{os.path.basename(out_path)}: {len(glyphs)} glyphs, {len(bitmaps)} bytes, "
          f"line_height={ascent + descent}")


def write_fonts(cards):
    header = os.path.join(OUT_DIR, "tarot_font.h")
    with open(header, "w", encoding="utf-8") as f:
        f.write("/** 自动生成: tools/gen_tarot.py —— 塔罗牌中文点阵字体 */\n")
        f.write('#pragma once\n\n#include "lvgl.h"\n\nextern const lv_font_t tarot_font_16;\n')

    text = "大阿尔克纳正逆位按下抽一张牌心中默念你的问题BOOT·"
    for name, up, rev, *_ in cards:
        text += name + up + rev
    chars = sorted(set(ASCII_CHARS) | set(text) | set(UI_EXTRA), key=ord)
    write_font(chars, 16, "tarot_font_16", os.path.join(OUT_DIR, "tarot_font_16.c"))


# ---------------------------------------------------------------- 预览
def preview(cards, back):
    scale = 5
    pad = 8
    cols = 8
    rows = (len(cards) + 1 + cols - 1) // cols
    img = Image.new("RGB", (cols * (W * scale + pad) + pad, rows * (H * scale + pad) + pad), (18, 18, 24))
    items = [(c[0], c[3]) for c in cards] + [("BACK", back)]
    font = ImageFont.truetype(FONT_TTF, 14)
    draw = ImageDraw.Draw(img)
    for i, (title, art) in enumerate(items):
        cx = pad + (i % cols) * (W * scale + pad)
        cy = pad + (i // cols) * (H * scale + pad)
        tile = Image.new("RGB", (W, H))
        tile.putdata([art.p[y][x] for y in range(H) for x in range(W)])
        img.paste(tile.resize((W * scale, H * scale), Image.NEAREST), (cx, cy))
        draw.text((cx + 2, cy + H * scale - 2), title, font=font, fill=(230, 220, 190), anchor="lb")
    out = os.path.join(ROOT, "tools", "preview_tarot.png")
    img.save(out)
    print("preview ->", out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--preview", action="store_true")
    args = p.parse_args()

    cards = []
    for name, up, rev, top, bot, accent, num, fn in CARDS:
        a = base(top, bot, accent, num, C(0x5E6B88))
        fn(a)
        cards.append((name, up, rev, a))
    back = card_back()

    write_data(cards, back)
    write_fonts(cards)
    if args.preview:
        preview(cards, back)


if __name__ == "__main__":
    main()
