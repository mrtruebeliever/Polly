#!/usr/bin/env python3
"""Builds Polly's full-screen jungle backdrop (jungle_bg.png, 200x228) for the
Pebble Time 2 (`emery`) and maps it onto the watch's 64-colour palette.

The PT2 screen is GColor8 (ARGB2222): 2 bits per channel, so every channel is
one of {0, 85, 170, 255} -> 64 opaque colours. The SDK would quantise for us,
but doing it here (with optional Floyd-Steinberg dither) lets us *see* and tune
exactly what lands on the watch instead of being surprised by banding on a busy
image.

Two sources, in priority order:
  1. An AI render: drop any image named  jungle_source.*  (png/jpg/webp) in this
     folder. It's cropped to 200x228 (cover), darkened top & bottom (so the white
     status label + speech bubble stay readable and the parrot pops), then
     quantised. This is the intended path -- generate the jungle with an image
     model, save it here, re-run this script.
  2. No source -> a procedural layered-foliage placeholder so the app still
     builds and you can judge the layout.

    .art-venv/bin/python resources/images/make_bg.py
"""
import glob
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 200, 228              # Pebble Time 2 screen
SS = 4                       # supersample for the procedural placeholder
OUT = os.path.join(HERE, 'jungle_bg.png')
DITHER = True                # Floyd-Steinberg when mapping to the 64-colour palette

# --- Pebble 64-colour palette -------------------------------------------------

def pebble_palette():
    levels = (0, 85, 170, 255)   # 2 bits per channel
    return [(r, g, b) for r in levels for g in levels for b in levels]

def quantize_pebble(img, dither=DITHER):
    """Map an RGB image onto the 64 Pebble colours (nearest, optional dither)."""
    pal_img = Image.new('P', (1, 1))
    flat = [c for rgb in pebble_palette() for c in rgb]
    flat += [0, 0, 0] * (256 - len(pebble_palette()))   # pad to 256 entries
    pal_img.putpalette(flat)
    q = img.convert('RGB').quantize(
        palette=pal_img, dither=Image.FLOYDSTEINBERG if dither else Image.NONE)
    return q.convert('RGB')

# --- Shared post-processing ---------------------------------------------------

def add_vignette(img):
    """Darken top & bottom so the white status label (top) and speech bubble
    (bottom) read clearly, and the centred parrot stands out from the scene."""
    grad = Image.new('L', (1, img.height), 0)
    px = grad.load()
    for y in range(img.height):
        t = y / (img.height - 1)
        top = max(0.0, 1 - t / 0.16)          # fade in over the top ~16%
        bot = max(0.0, (t - 0.66) / 0.34)     # fade in over the bottom ~34%
        px[0, y] = int(150 * max(top, bot))
    mask = grad.resize(img.size)
    dark = Image.new('RGB', img.size, (8, 16, 12))
    return Image.composite(dark, img, mask)

# --- Source 1: AI render ------------------------------------------------------

def load_source():
    for path in sorted(glob.glob(os.path.join(HERE, 'jungle_source.*'))):
        if os.path.splitext(path)[1].lower() in ('.png', '.jpg', '.jpeg', '.webp'):
            return path
    return None

def cover_crop(img, w, h):
    """Scale to cover w x h then centre-crop -- fills the screen, no letterboxing."""
    scale = max(w / img.width, h / img.height)
    img = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
    left = (img.width - w) // 2
    top = (img.height - h) // 2
    return img.crop((left, top, left + w, top + h))

# --- Source 2: procedural placeholder ----------------------------------------

SKY_TOP, SKY_BOT = (28, 64, 44), (74, 120, 70)     # filtered canopy light
LEAF_DK, LEAF, LEAF_LT = (38, 86, 52), (70, 130, 74), (132, 176, 96)
DAPPLE = (210, 226, 150)

def procedural_jungle():
    size = (W * SS, H * SS)

    def ebox(cx, cy, rx, ry):
        return [(cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS, (cy + ry) * SS]

    # vertical canopy-light gradient
    base = Image.new('RGBA', size)
    d = ImageDraw.Draw(base)
    for y in range(size[1]):
        t = y / (size[1] - 1)
        d.line([(0, y), (size[0], y)],
               fill=tuple(int(SKY_TOP[i] + (SKY_BOT[i] - SKY_TOP[i]) * t) for i in range(3)) + (255,))

    def add_blobs(blobs, blur):
        nonlocal base
        lyr = Image.new('RGBA', size, (0, 0, 0, 0))
        dd = ImageDraw.Draw(lyr)
        for cx, cy, rx, ry, col, a in blobs:
            dd.ellipse(ebox(cx, cy, rx, ry), fill=col + (a,))
        base = Image.alpha_composite(base, lyr.filter(ImageFilter.GaussianBlur(blur)))

    # far, soft foliage masses
    add_blobs([
        (20, 30, 46, 40, LEAF_DK, 200), (170, 24, 52, 44, LEAF_DK, 200),
        (96, 8, 50, 30, LEAF, 170), (4, 120, 40, 60, LEAF_DK, 190),
        (196, 130, 44, 64, LEAF_DK, 190), (150, 200, 60, 46, LEAF_DK, 210),
        (40, 210, 56, 44, LEAF_DK, 210),
    ], 9 * SS)
    # nearer, sharper leaf clusters
    add_blobs([
        (54, 18, 30, 20, LEAF, 200), (134, 40, 26, 18, LEAF_LT, 170),
        (24, 86, 22, 16, LEAF, 190), (182, 78, 24, 18, LEAF, 190),
        (108, 188, 30, 22, LEAF, 200), (76, 214, 26, 18, LEAF_LT, 180),
    ], 4 * SS)
    # dappled sunlight specks
    add_blobs([
        (60, 60, 5, 5, DAPPLE, 150), (150, 30, 4, 4, DAPPLE, 140),
        (30, 150, 5, 5, DAPPLE, 130), (185, 160, 4, 4, DAPPLE, 130),
        (120, 110, 4, 4, DAPPLE, 120), (95, 175, 5, 5, DAPPLE, 130),
    ], 2.5 * SS)

    return base.convert('RGB').resize((W, H), Image.LANCZOS)

# --- Main ---------------------------------------------------------------------

if __name__ == '__main__':
    src = load_source()
    if src:
        img = cover_crop(Image.open(src).convert('RGB'), W, H)
        # photographic detail benefits from dithering to fake intermediate colours
        dither = DITHER
        print('using AI source', os.path.basename(src))
    else:
        img = procedural_jungle()
        # the placeholder is smooth & near-palette already; dithering just adds speckle
        dither = False
        print('no jungle_source.* found -> procedural placeholder')

    img = add_vignette(img)
    img = quantize_pebble(img, dither)
    img.save(OUT)
    print('wrote', OUT, img.size)
