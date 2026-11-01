#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「呼吸引导器」用到的中文字体（LVGL 自定义 lv_font_t，A8 灰度位图）。

依赖: Pillow（仅重新生成字体时需要，固件不需要）
字体源: C:/Windows/Fonts/simhei.ttf（黑体）
说明: 不是 lv_font_conv，而是直接生成 lv_font_t 所需的回调表：
      - glyphs[]:  码点 / 步进 / 偏移 / 位图偏移
      - bitmaps[]: A8（每像素 1 字节）灰度位图
      get_glyph_bitmap 里把位图按行拷贝到 LVGL 给的 draw_buf，返回 draw_buf。

用法（项目根目录）:
    python tools/gen_ui_font.py
输出:
    main/ui_font_16.c  (正文/提示 16px)
    main/ui_font_28.c  (标题/倒计时 28px)
    main/ui_font.h
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_TTF = r"C:\Windows\Fonts\simhei.ttf"
OUT_DIR = os.path.join(ROOT, "main")

ASCII_CHARS = "".join(chr(c) for c in range(0x20, 0x7F))
# 16px 界面文案用字：呼吸引导 / 4-7-8 呼吸法 / 吸气 屏息 呼气 / 第 x/x 轮
# 按下 BOOT 开始 / 再按 BOOT 结束 / 完成
UI_CHARS_16 = "呼吸引导法气屏息按下开始再结束第轮完成"
# 28px 只用于标题与「完成」（倒计时数字在 ASCII 里）
UI_CHARS_28 = "呼吸引导完成"


def render_glyph(font, ch, size):
    """返回 (adv, ofs_x, ofs_y, w, h, bytes) —— ofs_y 语义见下方注释。"""
    adv = int(round(font.getlength(ch)))
    if ch == " ":
        return adv, 0, 0, 0, 0, b""
    # anchor="ls": 以基线左端为原点
    bbox = font.getbbox(ch, anchor="ls")
    left, top, right, bottom = bbox
    w, h = right - left, bottom - top
    if w <= 0 or h <= 0:
        return adv, 0, 0, 0, 0, b""

    canvas = Image.new("L", (size * 4, size * 4), 0)
    dr = ImageDraw.Draw(canvas)
    dr.text((size, size), ch, font=font, fill=255, anchor="ls")
    crop = canvas.crop((size + left, size + top, size + right, size + bottom))
    # LVGL: letter_y1 = pos.y + (line_height - base_line) - box_h - ofs_y
    # 期望 glyph 顶边 = 基线 - above(=-top)  =>  ofs_y = above - box_h
    ofs_y = (-top) - h
    return adv, left, ofs_y, w, h, crop.tobytes()


def gen_font(chars, size, name, out_path):
    font = ImageFont.truetype(FONT_TTF, size)
    ascent, descent = font.getmetrics()

    glyphs = []
    bitmaps = bytearray()
    for ch in chars:
        adv, ofs_x, ofs_y, w, h, data = render_glyph(font, ch, size)
        glyphs.append((ord(ch), adv, ofs_x, ofs_y, w, h, len(bitmaps)))
        bitmaps += data

    lines = []
    lines.append(f"/* 自动生成: tools/gen_ui_font.py  size={size}  glyphs={len(glyphs)}  bitmap={len(bitmaps)}B */")
    lines.append('#include <string.h>')
    lines.append('#include "ui_font.h"')
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    uint32_t cp;")
    lines.append("    uint16_t adv;")
    lines.append("    int16_t ofs_x, ofs_y;")
    lines.append("    uint8_t w, h;")
    lines.append("    uint32_t data_ofs;")
    lines.append("} glyph_t;")
    lines.append("")
    lines.append("static const uint8_t s_bitmap[] = {")
    for i in range(0, len(bitmaps), 16):
        chunk = ",".join(f"0x{b:02X}" for b in bitmaps[i:i + 16])
        lines.append("    " + chunk + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const glyph_t s_glyphs[] = {")
    for cp, adv, ofs_x, ofs_y, w, h, ofs in glyphs:
        lines.append(f"    {{0x{cp:04X}, {adv}, {ofs_x}, {ofs_y}, {w}, {h}, {ofs}}},  /* {chr(cp)} */")
    lines.append("};")
    lines.append("")
    lines.append("static bool font_get_glyph_dsc(const lv_font_t * font, lv_font_glyph_dsc_t * dsc,")
    lines.append("                                  uint32_t letter, uint32_t letter_next)")
    lines.append("{")
    lines.append("    LV_UNUSED(font); LV_UNUSED(letter_next);")
    lines.append("    int lo = 0, hi = (int)(sizeof(s_glyphs) / sizeof(s_glyphs[0])) - 1;")
    lines.append("    while (lo <= hi) {")
    lines.append("        int mid = (lo + hi) / 2;")
    lines.append("        if (s_glyphs[mid].cp == letter) {")
    lines.append("            dsc->adv_w = s_glyphs[mid].adv;")
    lines.append("            dsc->box_w = s_glyphs[mid].w;")
    lines.append("            dsc->box_h = s_glyphs[mid].h;")
    lines.append("            dsc->ofs_x = s_glyphs[mid].ofs_x;")
    lines.append("            dsc->ofs_y = s_glyphs[mid].ofs_y;")
    lines.append("            dsc->format = LV_FONT_GLYPH_FORMAT_A8;")
    lines.append("            dsc->is_placeholder = false;")
    lines.append("            dsc->gid.index = (uint32_t)mid;")
    lines.append("            return true;")
    lines.append("        }")
    lines.append("        if (s_glyphs[mid].cp < letter) lo = mid + 1; else hi = mid - 1;")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("static const void * font_get_glyph_bitmap(lv_font_glyph_dsc_t * dsc, lv_draw_buf_t * draw_buf)")
    lines.append("{")
    lines.append("    const glyph_t * g = &s_glyphs[dsc->gid.index];")
    lines.append("    if (g->w == 0 || g->h == 0) return NULL;")
    lines.append("    uint32_t stride = draw_buf->header.stride;")
    lines.append("    const uint8_t * src = s_bitmap + g->data_ofs;")
    lines.append("    for (int y = 0; y < g->h; y++) {")
    lines.append("        memcpy(draw_buf->data + y * stride, src + y * g->w, g->w);")
    lines.append("    }")
    lines.append("    return draw_buf;")
    lines.append("}")
    lines.append("")
    lines.append(f"const lv_font_t {name} = {{")
    lines.append("    .get_glyph_dsc = font_get_glyph_dsc,")
    lines.append("    .get_glyph_bitmap = font_get_glyph_bitmap,")
    lines.append(f"    .line_height = {ascent + descent},")
    lines.append(f"    .base_line = {descent},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .kerning = LV_FONT_KERNING_NONE,")
    lines.append("    .underline_position = -2,")
    lines.append("    .underline_thickness = 1,")
    lines.append("};")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{os.path.basename(out_path)}: {len(glyphs)} glyphs, {len(bitmaps)} bytes bitmap, "
          f"line_height={ascent + descent}, base_line={descent}")


def main():
    header = os.path.join(OUT_DIR, "ui_font.h")
    with open(header, "w", encoding="utf-8") as f:
        f.write("/** 自动生成: tools/gen_ui_font.py —— 呼吸引导器中文点阵字体 */\n")
        f.write("#pragma once\n\n#include \"lvgl.h\"\n\n")
        f.write("extern const lv_font_t ui_font_16;\nextern const lv_font_t ui_font_28;\n")

    chars16 = sorted(set(ASCII_CHARS) | set(UI_CHARS_16), key=ord)
    chars28 = sorted(set(ASCII_CHARS) | set(UI_CHARS_28), key=ord)
    gen_font(chars16, 16, "ui_font_16", os.path.join(OUT_DIR, "ui_font_16.c"))
    gen_font(chars28, 28, "ui_font_28", os.path.join(OUT_DIR, "ui_font_28.c"))


if __name__ == "__main__":
    main()
