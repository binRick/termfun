#!/usr/bin/env python3
"""Turn frames captured by capture_kitty.py into README images.

Composites the RGBA frames over the night background, scores them for
action, overlays the demo's real status bar (recovered from the cell
stream), and writes docs/<demo>-kitty.png plus docs/<demo>.gif.

Usage: python3 tools/compose_kitty.py <demo-name>
"""

import os
import re
import sys

import pyte
from PIL import Image, ImageDraw, ImageFont

DEMO = sys.argv[1] if len(sys.argv) > 1 else 'fireworks'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_STILL = os.path.join(ROOT, f'docs/{DEMO}-kitty.png')
OUT_GIF = os.path.join(ROOT, f'docs/{DEMO}.gif')

BG = (5, 5, 16)
FRAME_DIR = '/tmp/gfx_frames'
COLS, ROWS = 140, 40        # must match capture_kitty.py
STILL_MIN_FRAME = 15        # skip warm-up frames for the still
GIF_FRAMES = (14, 52)       # launch -> bloom -> fade arc of the first volley
GIF_WIDTH = 800
GIF_FRAME_MS = 100          # 0.1 sim seconds per frame = real-time playback

FONTS = [
    os.path.expanduser('~/Library/Fonts/CascadiaCode.ttf'),
    '/System/Library/Fonts/SFNSMono.ttf',
    '/System/Library/Fonts/Monaco.ttf',
]

def load_frames():
    entries = []
    with open(os.path.join(FRAME_DIR, 'index.txt')) as f:
        for line in f:
            m = re.match(r'(\d+) t=([\d.]+) (\d+)x(\d+) cell_len=(\d+)', line)
            if m:
                entries.append((int(m[1]), float(m[2]), int(m[5])))
    frames = []
    for n, t, cell_len in entries:
        img = Image.open(f'{FRAME_DIR}/frame_{n:03d}.png').convert('RGBA')
        frames.append((n, t, img, cell_len))
    return frames

def composite(img):
    base = Image.new('RGBA', img.size, BG + (255,))
    return Image.alpha_composite(base, img).convert('RGB')

def score(img):
    # count of punchy pixels in the sky — favors fresh dense bursts
    w, h = img.size
    sky = img.crop((0, 0, w, h * 3 // 4)).convert('L')
    hist = sky.histogram()
    bright = sum(c for i, c in enumerate(hist) if i > 50)
    glow = sum(c for i, c in enumerate(hist) if 15 < i <= 50)
    return bright + glow * 0.25

def hud_text(cell_len):
    with open('/tmp/gfx_cell_stream.bin', 'rb') as f:
        stream = f.read(cell_len)
    # pyte can't parse private-mode DSR queries sent during detection
    stream = re.sub(rb'\x1b\[\?[0-9;]*n', b'', stream)
    screen = pyte.Screen(COLS, ROWS)
    pyte.ByteStream(screen).feed(stream)
    row = screen.buffer[0]
    return [(row[c].data or ' ', row[c].fg) for c in range(COLS)]

def parse_color(color, default):
    if isinstance(color, str) and len(color) == 6:
        try:
            return (int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16))
        except ValueError:
            pass
    return default

def draw_hud(img, cells):
    w, _ = img.size
    cell_w = w / COLS
    font = None
    for path in FONTS:
        try:
            font = ImageFont.truetype(path, 12)
            break
        except Exception:
            pass
    if font is None:
        font = ImageFont.load_default()
    draw = ImageDraw.Draw(img)
    for col, (ch, fg) in enumerate(cells):
        if ch.strip():
            draw.text((col * cell_w + 1, 2), ch, font=font,
                      fill=parse_color(fg, (170, 170, 200)))
    return img

def main():
    frames = load_frames()
    print(f"loaded {len(frames)} frames")

    scored = []
    for n, t, img, cell_len in frames:
        rgb = composite(img)
        scored.append((score(rgb), n, t, rgb, cell_len))

    candidates = [x for x in scored if x[1] >= STILL_MIN_FRAME]
    s, n, t, rgb, cell_len = max(candidates, key=lambda x: x[0])
    print(f"best still: frame {n} at t={t:.2f} (score {s:.0f})")

    draw_hud(rgb.copy(), hud_text(cell_len)).save(OUT_STILL)
    print(f"saved {OUT_STILL}")

    gif_frames = [x for x in scored if GIF_FRAMES[0] <= x[1] <= GIF_FRAMES[1]]
    imgs = []
    for _, _, _, rgb_, _ in gif_frames:
        w, h = rgb_.size
        imgs.append(rgb_.resize((GIF_WIDTH, int(h * GIF_WIDTH / w)), Image.LANCZOS))
    imgs[0].save(OUT_GIF, save_all=True, append_images=imgs[1:],
                 duration=GIF_FRAME_MS, loop=0, optimize=True)
    print(f"saved {OUT_GIF} ({len(imgs)} frames, {os.path.getsize(OUT_GIF) // 1024} KB)")

if __name__ == '__main__':
    main()
