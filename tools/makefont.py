#!/usr/bin/env python3
# The game's lettering, from Liberation Serif Bold (SIL Open Font Licence 1.1,
# see docs/credits.md): two bitmap fonts (small for the 320 x 240 HUD, large
# for the 640 x 480 screens) and the carved-stone title logo.
#
#   python3 tools/makefont.py [path to LiberationSerif-Bold.ttf]
#
# Writes assets/font_small.tim and assets/font_large.tim (4-bit, 256 wide,
# each in its own texture page: FONT_X, FONT_Y and FONT_X, FONT_Y + 256),
# assets/logo.tim (4-bit, 512 x 112, its own CLUT) and src/font.h (cell
# positions and widths). Needs Pillow.
import os, struct, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
TTF = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf'
FONT_X, FONT_Y, FONT_CLUT = 768, 0, (0, 482)
LOGO_X, LOGO_Y, LOGO_CLUT = 832, 0, (0, 483)
CHARS = [chr(c) for c in range(32, 127)]

def to15(r, g, b):
    v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
    return v or 0x0421

def render(size, cell_w, cell_h, baseline):
    """Each character in a cell: index 1..15 = coverage (0 transparent), plus a
    1-pixel dark drop shadow to the bottom right (index 1)."""
    font = ImageFont.truetype(TTF, size)
    cells, widths = [], []
    for ch in CHARS:
        img = Image.new('L', (cell_w, cell_h), 0)
        ImageDraw.Draw(img).text((1, baseline), ch, font=font, fill=255, anchor='ls')
        w = int(font.getlength(ch)) + 1
        widths.append(min(cell_w, max(2, w)))
        px = img.load()
        out = [[0] * cell_w for _ in range(cell_h)]
        for y in range(cell_h):
            for x in range(cell_w):
                v = px[x, y]
                if v > 40:
                    out[y][x] = 2 + min(13, (v - 40) * 14 // 216)   # 2..15: ink, by coverage
        for y in range(cell_h - 1, 0, -1):
            for x in range(cell_w - 1, 0, -1):
                if out[y][x] == 0 and out[y - 1][x - 1] >= 6:
                    out[y][x] = 1   # shadow
        cells.append(out)
    return cells, widths

small, small_w = render(13, 10, 14, 11)
large, large_w = render(26, 22, 28, 22)

# Two atlases, 256 wide: small cells 25 a row, large cells 11 a row.
W = 256
def atlas_of(cells, cw, ch, per_row):
    rows = (len(cells) + per_row - 1) // per_row
    img = [[0] * W for _ in range(rows * ch)]
    pos = []
    for i, c in enumerate(cells):
        u, v = (i % per_row) * cw, (i // per_row) * ch
        pos.append((u, v))
        for y in range(ch):
            img[v + y][u:u + cw] = c[y]
    return img, pos
small_atlas, pos_small = atlas_of(small, 10, 14, 25)
large_atlas, pos_large = atlas_of(large, 22, 28, 11)
assert len(large_atlas) <= 256

def tim4(path, rows, w, h, x, y, clut_xy, clut):
    data = bytearray()
    for r in rows:
        for i in range(0, w, 2):
            data.append(r[i] | (r[i + 1] << 4))
    clut_bytes = 12 + 32
    out = struct.pack('<II', 0x10, 0x08)
    out += struct.pack('<IHHHH', clut_bytes, clut_xy[0], clut_xy[1], 16, 1)
    out += b''.join(struct.pack('<H', c) for c in clut)
    out += struct.pack('<IHHHH', 12 + len(data), x, y, w // 4, h)
    out += bytes(data)
    open(path, 'wb').write(out)

# White ink fading in, a near-black shadow; the polygon colour tints it.
font_clut = [0, to15(8, 6, 6)] + [to15(*(int(40 + (i - 2) * 215 / 13),) * 3) for i in range(2, 16)]
tim4(os.path.join(ROOT, 'assets', 'font_small.tim'), small_atlas, W, len(small_atlas), FONT_X, FONT_Y, FONT_CLUT, font_clut)
tim4(os.path.join(ROOT, 'assets', 'font_large.tim'), large_atlas, W, len(large_atlas), FONT_X, FONT_Y + 256, FONT_CLUT, font_clut)

# ---- The logo: GRID SOULS carved into a slab of stone -----------------------------
LW, LH = 512, 112
font = ImageFont.truetype(TTF, 84)
mask = Image.new('L', (LW, LH), 0)
d = ImageDraw.Draw(mask)
d.text((LW // 2, LH // 2 + 2), 'GRID SOULS', font=font, fill=255, anchor='mm')
mask = mask.point(lambda v: 255 if v > 110 else 0)
# Bevel: light from the top left on the letters' edges, shadow below right.
inner = mask.filter(ImageFilter.MinFilter(5))
shift_up = Image.new('L', (LW, LH), 0); shift_up.paste(mask, (-2, -2))
shift_dn = Image.new('L', (LW, LH), 0); shift_dn.paste(mask, (3, 3))
m, mi, su, sd = mask.load(), inner.load(), shift_up.load(), shift_dn.load()
import random
random.seed(5)
rows = [[0] * LW for _ in range(LH)]
for y in range(LH):
    for x in range(LW):
        if m[x, y]:
            if not su[x, y]:
                v = 15                              # lit top-left edge
            elif not sd[x, y] or not mi[x, y]:
                v = 6                               # shadowed inner edge
            else:
                v = 10 + random.randint(0, 2) - (1 if (x * 7 + y * 3) % 23 == 0 else 0)   # stone face
            rows[y][x] = max(2, min(15, v))
        elif sd[x, y]:
            rows[y][x] = 1                          # cast shadow
logo_clut = [0, to15(6, 5, 6)] + [to15(*(int(30 + (i - 2) * 205 / 13 * (1 if i < 15 else 1.05)),) * 3) for i in range(2, 16)]
tim4(os.path.join(ROOT, 'assets', 'logo.tim'), rows, LW, LH, LOGO_X, LOGO_Y, LOGO_CLUT, logo_clut)

# ---- Header ---------------------------------------------------------------------------
def table(name, pos, widths):
    return f'static const uint8_t {name}[95][3] = {{   /* u, v, width */\n' + \
        ''.join(f'\t{{ {u}, {v}, {w} }},   /* {repr(c)} */\n' for (u, v), w, c in zip(pos, widths, CHARS)) + '};\n'

open(os.path.join(ROOT, 'src', 'font.h'), 'w').write(f'''/* Generated by tools/makefont.py: do not edit. */
#pragma once
#include <stdint.h>
#define FONT_TPAGE_X {FONT_X}
#define FONT_TPAGE_Y {FONT_Y}   /* the large font: FONT_TPAGE_Y + 256 */
#define FONT_CLUT_X  {FONT_CLUT[0]}
#define FONT_CLUT_Y  {FONT_CLUT[1]}
#define LOGO_TPAGE_X {LOGO_X}
#define LOGO_TPAGE_Y {LOGO_Y}
#define LOGO_CLUT_X  {LOGO_CLUT[0]}
#define LOGO_CLUT_Y  {LOGO_CLUT[1]}
#define LOGO_W {LW}
#define LOGO_H {LH}
#define SMALL_W 10
#define SMALL_H 14
#define LARGE_W 22
#define LARGE_H 28
{table('font_small', pos_small, small_w)}
{table('font_large', pos_large, large_w)}''')
print(f'fonts: small {W} x {len(small_atlas)}, large {W} x {len(large_atlas)}; logo {LW} x {LH}')
