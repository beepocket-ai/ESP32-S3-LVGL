#!/usr/bin/env python3
"""把项目根目录的 1.png / 2.png / 3.png 转成 LVGL 可直接使用的 RGB565 二进制（240x240）。

只依赖 Python 标准库（zlib），不需要 Pillow。
支持：8bit、非隔行的 PNG，灰度/灰度+Alpha/RGB/RGBA/调色板 五种颜色类型；
尺寸不是 240x240 时按“覆盖裁切”最近邻缩放到 240x240，透明部分压黑底。

用法（项目根目录）:
    python tools/png_to_rgb565.py

输出: main/images/img1.bin ~ img3.bin（每张 240*240*2 = 115200 字节），
由 main/CMakeLists.txt 的 target_add_binary_data 打包进固件。
"""
import os
import struct
import zlib

W = H = 240
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "main", "images")


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """返回 (width, height, rows, ctype, palette, trns)；rows 为未滤波的原始行字节。"""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG file")

    pos = 8
    ihdr = None
    idat = bytearray()
    palette = None
    trns = None
    while pos < len(data):
        (length, ctype) = struct.unpack(">I4s", data[pos:pos + 8])
        pos += 8
        chunk = data[pos:pos + length]
        pos += length + 4                      # 跳过 CRC
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"PLTE":
            palette = [chunk[i:i + 3] for i in range(0, len(chunk), 3)]
        elif ctype == b"tRNS":
            trns = chunk
        elif ctype == b"IEND":
            break

    if ihdr is None:
        raise ValueError(f"{path}: missing IHDR")
    w, h, depth, ctype, comp, filt, interlace = ihdr
    if depth != 8 or interlace != 0 or comp != 0 or filt != 0:
        raise ValueError(f"{path}: unsupported PNG (depth={depth}, interlace={interlace})")
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]

    raw = zlib.decompress(bytes(idat))
    stride = w * nch
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(h):
        f = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        if f == 1:
            for x in range(nch, stride):
                line[x] = (line[x] + line[x - nch]) & 0xFF
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                c = prev[x - nch] if x >= nch else 0
                line[x] = (line[x] + _paeth(a, prev[x], c)) & 0xFF
        rows.append(line)
        prev = line
    return w, h, rows, ctype, palette, trns


def make_pixel_fn(w, h, rows, ctype, palette, trns):
    """返回 fn(x, y) -> (r, g, b)，已按黑底合成 Alpha。"""
    def pixel(x, y):
        line = rows[y]
        if ctype == 6:                         # RGBA
            r, g, b, a = line[x * 4:x * 4 + 4]
            return (r * a // 255, g * a // 255, b * a // 255)
        if ctype == 2:                         # RGB
            return tuple(line[x * 3:x * 3 + 3])
        if ctype == 0:                         # 灰度
            v = line[x]
            return (v, v, v)
        if ctype == 4:                         # 灰度 + Alpha
            v, a = line[x * 2:x * 2 + 2]
            v = v * a // 255
            return (v, v, v)
        if ctype == 3:                         # 调色板
            idx = line[x]
            r, g, b = palette[idx] if palette else (0, 0, 0)
            a = trns[idx] if trns is not None and idx < len(trns) else 255
            return (r * a // 255, g * a // 255, b * a // 255)
        raise ValueError("unsupported color type")
    return pixel


def convert(src_path, dst_path):
    w, h, rows, ctype, palette, trns = read_png(src_path)
    pixel = make_pixel_fn(w, h, rows, ctype, palette, trns)

    # 覆盖式缩放：等比放大到铺满 240x240，再居中裁切；最近邻取色
    scale = max(W / w, H / h)
    off_x = (w * scale - W) / 2.0
    off_y = (h * scale - H) / 2.0

    data = bytearray(W * H * 2)
    i = 0
    for y in range(H):
        sy = min(h - 1, max(0, int((y + off_y) / scale)))
        for x in range(W):
            sx = min(w - 1, max(0, int((x + off_x) / scale)))
            r, g, b = pixel(sx, sy)
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data[i] = v & 0xFF
            data[i + 1] = v >> 8
            i += 2

    with open(dst_path, "wb") as f:
        f.write(data)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for idx in (1, 2, 3):
        src = os.path.join(ROOT, f"{idx}.png")
        dst = os.path.join(OUT_DIR, f"img{idx}.bin")
        convert(src, dst)
        print(f"{os.path.basename(src)} -> {os.path.relpath(dst, ROOT)}  ({os.path.getsize(dst)} bytes)")


if __name__ == "__main__":
    main()
