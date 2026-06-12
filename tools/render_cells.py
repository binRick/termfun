#!/usr/bin/env python3
"""Render an ANSI terminal dump to a PNG for README screenshots.

Capture flow (cell-mode demos):

    tmux new-session -d -s demo -x 140 -y 40 './build/<demo>'
    sleep 5                                   # let the scene develop
    tmux capture-pane -t demo -p -e > /tmp/dump.txt
    tmux kill-session -t demo
    python3 tools/render_cells.py /tmp/dump.txt docs/<demo>-cells.png 140 40

Capture several frames and keep the one with the most non-blank cells —
animation peaks vary. Needs `pip install pyte pillow`.
"""

import sys

import pyte
from PIL import Image, ImageDraw, ImageFont

BG_DEFAULT = (5, 5, 16)
FG_DEFAULT = (140, 140, 170)

# Glyphs drawn as shapes instead of font glyphs, for crisp particles
# (fraction = dot diameter relative to cell height)
DOT_FILL = {'•': 0.45, '●': 0.6, '✸': 0.55, '·': 0.2}

FONTS = [
    "~/Library/Fonts/CascadiaCode.ttf",
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Monaco.ttf",
]

# Tried per glyph when the primary font lacks it (e.g. matrix's katakana)
FALLBACK_FONTS = [
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
]

def has_glyph(font, ch):
    mask = font.getmask(ch, mode="L")
    notdef = font.getmask("", mode="L")
    return mask.getbbox() is not None and mask.getbbox() != notdef.getbbox()

def parse_color(color, default):
    if not color or color == "default":
        return default
    if isinstance(color, tuple):
        return color
    if isinstance(color, str) and len(color) == 6:
        try:
            return (int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16))
        except ValueError:
            pass
    return default

def main():
    ansi_file = sys.argv[1]
    out_file = sys.argv[2]
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 140
    rows = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    cell_w = int(sys.argv[5]) if len(sys.argv) > 5 else 10
    cell_h = int(sys.argv[6]) if len(sys.argv) > 6 else 20
    font_size = int(sys.argv[7]) if len(sys.argv) > 7 else 15

    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)
    with open(ansi_file, "rb") as f:
        stream.feed(f.read())

    import os
    font = None
    for path in FONTS:
        try:
            font = ImageFont.truetype(os.path.expanduser(path), font_size)
            break
        except Exception:
            pass
    if font is None:
        font = ImageFont.load_default()

    fallbacks = []
    for path in FALLBACK_FONTS:
        try:
            fallbacks.append(ImageFont.truetype(path, font_size))
        except Exception:
            pass
    font_for = {}

    def pick_font(ch):
        if ch not in font_for:
            font_for[ch] = next(
                (f for f in [font] + fallbacks if has_glyph(f, ch)), font)
        return font_for[ch]

    img = Image.new("RGB", (cols * cell_w, rows * cell_h), BG_DEFAULT)
    draw = ImageDraw.Draw(img)

    for row_idx in range(rows):
        line = screen.buffer[row_idx]
        for col_idx in range(cols):
            char = line[col_idx]
            ch = char.data if char.data else " "
            fg = parse_color(char.fg, FG_DEFAULT)
            bg = parse_color(char.bg, BG_DEFAULT)
            x = col_idx * cell_w
            y = row_idx * cell_h

            draw.rectangle([x, y, x + cell_w - 1, y + cell_h - 1], fill=bg)
            if not ch.strip():
                continue

            if ch == '█':
                draw.rectangle([x, y, x + cell_w - 1, y + cell_h - 1], fill=fg)
            elif ch == '▪':
                pad = cell_w // 4
                draw.rectangle([x + pad, y + pad, x + cell_w - pad, y + cell_h - pad], fill=fg)
            elif ch == '│':
                cx = x + cell_w // 2
                draw.rectangle([cx - 1, y, cx + 1, y + cell_h - 1], fill=fg)
            elif ch in DOT_FILL:
                r = max(1, int(cell_h * DOT_FILL[ch] / 2))
                cx, cy = x + cell_w // 2, y + cell_h // 2
                draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fg)
            else:
                draw.text((x, y + 1), ch, font=pick_font(ch), fill=fg)

    img.save(out_file)
    print(f"Saved {img.size[0]}x{img.size[1]} -> {out_file}")

if __name__ == "__main__":
    main()
