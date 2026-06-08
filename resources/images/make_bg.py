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
import random
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
    dark = Image.new('RGB', img.size, (6, 14, 0))   # blue 0: keep the whole image blue-free
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

# Blue is EXACTLY 0 on every colour here -- the whole point of the fix. Neutral
# grey (85,85,85) on the PT2 palette needs blue ~85; Floyd-Steinberg dithers each
# channel independently, so a small non-zero blue would still stipple some pixels
# up to blue=85 (reintroducing grey/teal). With blue pinned to 0, the blue channel
# carries no quantisation error at all, so dither only ever blends (r,g,0) cells --
# greens, limes and warm olives, never the grey plate that sat behind the parrot.
SKY_TOP, SKY_BOT = (10, 64, 0), (26, 128, 0)          # shaded canopy depth
LEAF_DK, LEAF = (16, 72, 0), (30, 132, 0)             # shadow & body greens
LEAF_LT, DAPPLE = (96, 196, 0), (176, 244, 0)         # sunlit lime & bright specks

def procedural_jungle():
    size = (W * SS, H * SS)
    rnd = random.Random(11)   # fixed seed -> reproducible canopy

    # vertical canopy-light gradient (darker at top, opening up lower down)
    base = Image.new('RGBA', size)
    d = ImageDraw.Draw(base)
    for y in range(size[1]):
        t = y / (size[1] - 1)
        d.line([(0, y), (size[0], y)],
               fill=tuple(int(SKY_TOP[i] + (SKY_BOT[i] - SKY_TOP[i]) * t) for i in range(3)) + (255,))

    def stamp_cluster(dd, cx, cy, spread, leaf_r, col, alpha, n):
        """A clump of overlapping leaf ellipses -> reads as foliage, not a blob."""
        for _ in range(n):
            ox = rnd.uniform(-spread, spread)
            oy = rnd.uniform(-spread * 0.55, spread * 0.55)
            rx = leaf_r * rnd.uniform(0.6, 1.1)
            ry = rx * rnd.uniform(0.45, 0.7)
            x, y = (cx + ox) * SS, (cy + oy) * SS
            dd.ellipse([x - rx * SS, y - ry * SS, x + rx * SS, y + ry * SS],
                       fill=col + (alpha,))

    def add_layer(clusters, leaf_r, col, alpha, n, blur):
        nonlocal base
        lyr = Image.new('RGBA', size, (0, 0, 0, 0))
        dd = ImageDraw.Draw(lyr)
        for cx, cy, spread in clusters:
            stamp_cluster(dd, cx, cy, spread, leaf_r, col, alpha, n)
        base = Image.alpha_composite(base, lyr.filter(ImageFilter.GaussianBlur(blur)))

    # Layer 1 -- far, very soft dark masses for depth (whole frame, behind all).
    add_layer([(24, 26, 40), (176, 22, 42), (100, 6, 46), (6, 116, 38),
               (196, 128, 40), (150, 206, 48), (40, 212, 46), (100, 120, 50)],
              leaf_r=24, col=LEAF_DK, alpha=205, n=7, blur=7 * SS)

    # Layer 2 -- the canopy itself: mid-green leaf clumps framing the parrot,
    # dense along the top and down both sides, sparse over the open centre.
    add_layer([(18, 14, 26), (54, 8, 26), (96, 6, 26), (140, 10, 26), (182, 16, 26),
               (10, 60, 22), (12, 104, 22), (16, 150, 22),
               (190, 56, 22), (188, 100, 22), (184, 148, 22),
               (40, 210, 24), (150, 206, 24), (96, 220, 22)],
              leaf_r=17, col=LEAF, alpha=210, n=7, blur=3.5 * SS)

    # Layer 3 -- sunlit highlights catching the tops of the canopy clumps.
    add_layer([(50, 10, 18), (108, 8, 18), (170, 14, 18),
               (12, 66, 14), (188, 74, 14), (16, 132, 14), (190, 130, 14),
               (44, 206, 16), (152, 202, 16)],
              leaf_r=10, col=LEAF_LT, alpha=185, n=6, blur=2.0 * SS)

    # Layer 4 -- bright dappled-sunlight specks scattered through the leaves.
    lyr = Image.new('RGBA', size, (0, 0, 0, 0))
    dd = ImageDraw.Draw(lyr)
    for _ in range(34):
        x = rnd.uniform(0, W); y = rnd.uniform(0, H * 0.92)
        r = rnd.uniform(2.0, 4.0)
        dd.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS],
                   fill=DAPPLE + (rnd.randint(110, 165),))
    base = Image.alpha_composite(base, lyr.filter(ImageFilter.GaussianBlur(2.0 * SS)))

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
        # Dither ON: with blue pinned low everywhere, Floyd-Steinberg only blends
        # neighbouring green/lime palette cells, turning the layered canopy into a
        # soft leafy stipple (no grey, which would need blue ~85).
        dither = True
        print('no jungle_source.* found -> procedural placeholder')

    img = add_vignette(img)
    img = quantize_pebble(img, dither)
    img.save(OUT)
    print('wrote', OUT, img.size)
