#!/usr/bin/env python3
"""Generates Polly's parrot artwork as anti-aliased RGBA PNGs via Pillow.

Renders every frame at 4x and downsamples with LANCZOS for smooth edges, and
layers soft radial highlights + blurred shadows on top of flat silhouettes for
a glossy, lit-from-one-side look.

The parrot is a *transparent* sprite (no scene baked in): the jungle backdrop is
a separate full-screen image (see make_bg.py) that the C side draws underneath,
so the parrot can sit anywhere and the background fills all 200x228 px. To keep
the bird readable against a busy jungle, each sprite gets a dark contour rim and
a soft contact shadow on its perch.

    uv venv .art-venv && uv pip install --python .art-venv/bin/python pillow
    .art-venv/bin/python resources/images/gen_art.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'

SS = 4                       # supersample factor; rendered at NxSS then LANCZOS-downsampled
W, H = 140, 168              # logical parrot-frame size (drawing coordinate space)

# Each saved sprite is held in RAM as a 1-byte-per-pixel GBitmap (W*PARROT_SCALE
# x H*PARROT_SCALE bytes -- color complexity doesn't matter, only pixel count).
# `pebble build` reports ~124KB free heap before the window even loads, and the
# jungle_bg alone costs ~45.6KB (200x228, full screen). parrot_window.c now
# loads at most 3 pose bitmaps at once (down from all 5), but at 1.18 those
# alone would be ~96KB -- on top of the backdrop that overflows the heap
# ("PNG memory allocation failed" -> broken bitmap -> crash the moment that
# pose is first drawn, i.e. exactly when the speak poses first appear). At 0.85
# a sprite is ~17KB, so backdrop + 3 sprites is ~96KB, leaving a comfortable
# ~28KB for layers, AppMessage buffers, Clay settings and the rest.
PARROT_SCALE = 0.85
ICON = 25                    # final menu-icon size
SIZE = (W * SS, H * SS)

# --- Palette (scarlet macaw) --------------------------------------------------
RED, RED_DARK, RED_LIGHT     = (208, 47, 39, 255), (163, 30, 30, 255), (236, 108, 96, 255)
YELLOW, YELLOW_LT            = (247, 200, 56, 255), (252, 226, 134, 255)
ORANGE, ORANGE_LT            = (236, 140, 38, 255), (246, 180, 104, 255)
BLUE, BLUE_DARK, BLUE_LT     = (61, 110, 199, 255), (44, 79, 153, 255), (130, 168, 232, 255)
WHITE, BLACK                 = (255, 255, 255, 255), (35, 30, 30, 255)
BEAK, BEAK_DARK              = (84, 84, 94, 255), (56, 56, 64, 255)
BRANCH, BRANCH_DK, BRANCH_LT = (122, 86, 61, 255), (93, 64, 44, 255), (156, 117, 87, 255)
SHADOW                       = (50, 60, 74)
RIM                          = (26, 22, 22, 255)   # dark contour that separates the bird from the jungle

# --- Drawing helpers -----------------------------------------------------------
# Shapes are described in the logical 140x168 (or 25x25 icon) coordinate space and
# scaled up to the supersampled canvas by `ebox`/`pts`, so the layout below reads
# the same as the old rasterizer's coordinates.

def layer(size=SIZE):
    return Image.new('RGBA', size, (0, 0, 0, 0))

def ebox(cx, cy, rx, ry):
    return [(cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS, (cy + ry) * SS]

def pts(points):
    return [(x * SS, y * SS) for x, y in points]

def glossy(size, draw_fn, base, highlight, hl_rel=(-0.32, -0.36), hl_radius_rel=0.65,
           blur=5 * SS, outline=None, outline_width=0):
    """Fills a silhouette (drawn by draw_fn) and blends a soft round highlight into its
    upper-left, clipped back to the silhouette -- turns a flat fill into a glossy,
    lit-from-one-side shape with a smooth gradient instead of a hard colour edge.
    draw_fn(draw, **kwargs) should forward kwargs straight to a Pillow draw call."""
    silhouette = layer(size)
    draw_fn(ImageDraw.Draw(silhouette), fill=base)
    mask = silhouette.split()[3]
    bbox = mask.getbbox()
    if bbox is None:
        return silhouette
    x0, y0, x1, y1 = bbox
    cx = x0 + (x1 - x0) * (0.5 + hl_rel[0])
    cy = y0 + (y1 - y0) * (0.5 + hl_rel[1])
    r = max(x1 - x0, y1 - y0) * hl_radius_rel
    hl = layer(size)
    ImageDraw.Draw(hl).ellipse([cx - r, cy - r, cx + r, cy + r], fill=highlight)
    hl = hl.filter(ImageFilter.GaussianBlur(blur))
    blended = Image.alpha_composite(silhouette, hl)
    result = Image.composite(blended, layer(size), mask)
    if outline:
        draw_fn(ImageDraw.Draw(result), outline=outline, width=outline_width)
    return result

def blurred_blob(draw_fn, color, alpha, blur):
    """A soft, semi-transparent silhouette -- used for contact/cast shadows."""
    blob = layer()
    draw_fn(ImageDraw.Draw(blob), fill=color + (alpha,))
    return blob.filter(ImageFilter.GaussianBlur(blur))

def with_rim(sprite, color=RIM, grow=3 * SS, thresh=55):
    """Wraps an opaque sprite in a soft dark contour by growing its alpha mask and
    filling the halo behind it -- gives the parrot a clean edge so it reads clearly
    on top of the busy jungle background instead of blending into the greenery."""
    grown = sprite.split()[3].filter(ImageFilter.GaussianBlur(grow)).point(
        lambda v: 255 if v > thresh else 0)
    rim = layer(sprite.size)
    rim.paste(color, (0, 0), grown)
    return Image.alpha_composite(rim, sprite)

# --- Perch -------------------------------------------------------------------

def contact_shadow():
    """The soft shadow the parrot casts on its perch -- kept on its own layer (below
    the bird) so the contour rim never traces around this faint blob."""
    c = layer()
    c.alpha_composite(blurred_blob(
        lambda d, **kw: d.ellipse(ebox(72, H - 31, 38, 7), **kw), SHADOW, 70, 5 * SS))
    return c

def branch(c):
    c.alpha_composite(glossy(
        SIZE, lambda d, **kw: d.rounded_rectangle(ebox(W / 2, H - 21, W / 2 + 3, 5.5), radius=6 * SS, **kw),
        BRANCH, BRANCH_LT, hl_rel=(-0.42, -0.7), hl_radius_rel=0.5))
    ImageDraw.Draw(c).rounded_rectangle(ebox(W / 2, H - 14, W / 2 + 3, 4), radius=5 * SS, fill=BRANCH_DK)

# --- Parrot parts ------------------------------------------------------------

def tail(c, lean=0):
    bx = 70 + lean
    feathers = [
        ((bx, 96), (bx - 11, 152), (bx + 5, 152), BLUE, BLUE_LT),
        ((bx + 7, 96), (bx - 1, 153), (bx + 16, 153), YELLOW, YELLOW_LT),
        ((bx + 16, 98), (bx + 7, 150), (bx + 25, 150), RED, RED_LIGHT),
    ]
    for p0, p1, p2, base, hl in feathers:
        c.alpha_composite(glossy(
            SIZE, lambda d, p0=p0, p1=p1, p2=p2, **kw: d.polygon(pts([p0, p1, p2]), **kw),
            base, hl, hl_rel=(-0.25, -0.55), hl_radius_rel=0.45))

def wing(c, lean=0):
    wx = 86 + lean
    c.alpha_composite(glossy(SIZE, lambda d, **kw: d.ellipse(ebox(wx, 92, 26, 38), **kw),
                             BLUE_DARK, BLUE_LT))
    c.alpha_composite(glossy(SIZE, lambda d, **kw: d.ellipse(ebox(wx, 92, 17, 28), **kw),
                             BLUE, BLUE_LT))
    c.alpha_composite(glossy(SIZE, lambda d, **kw: d.ellipse(ebox(wx - 2, 110, 12, 16), **kw),
                             YELLOW, YELLOW_LT))

def wing_spread(c):
    """Both wings raised and open -- the flap pose. Drawn behind the body (like
    wing()) so the body overlaps their inner edge; the outer halves rise above
    and beside the body to read clearly as spread wings."""
    for wx in (44, 92):
        c.alpha_composite(glossy(SIZE, lambda d, wx=wx, **kw: d.ellipse(ebox(wx, 78, 15, 33), **kw),
                                 BLUE_DARK, BLUE_LT))
        c.alpha_composite(glossy(SIZE, lambda d, wx=wx, **kw: d.ellipse(ebox(wx, 80, 9, 23), **kw),
                                 BLUE, BLUE_LT))
        c.alpha_composite(glossy(SIZE, lambda d, wx=wx, **kw: d.ellipse(ebox(wx, 96, 7, 11), **kw),
                                 YELLOW, YELLOW_LT))

def body(c):
    c.alpha_composite(glossy(SIZE, lambda d, **kw: d.ellipse(ebox(68, 98, 34, 44), **kw),
                             RED, RED_LIGHT, outline=RED_DARK, outline_width=SS))
    c.alpha_composite(glossy(SIZE, lambda d, **kw: d.ellipse(ebox(68, 113, 21, 27), **kw),
                             ORANGE, ORANGE_LT))

def head(c, dx=0, dy=0, tilt=0):
    hx, hy = 70 + dx + tilt, 56 + dy
    c.alpha_composite(glossy(SIZE, lambda d, **kw: d.ellipse(ebox(hx, hy, 26, 26), **kw),
                             RED, RED_LIGHT, outline=RED_DARK, outline_width=SS))
    d = ImageDraw.Draw(c)
    d.ellipse(ebox(hx - 9, hy - 3, 9, 9), fill=WHITE, outline=(222, 224, 228, 255), width=max(1, SS // 2))
    return hx, hy

def eye(c, hx, hy, tilt=0, closed=False):
    ex, ey = hx - 9 + tilt / 2, hy - 3
    d = ImageDraw.Draw(c)
    if closed:
        d.line(pts([(ex - 4.5, ey), (ex + 4.5, ey)]), fill=BLACK, width=2 * SS)
        d.ellipse(ebox(ex - 4.5, ey, 1.1, 1.1), fill=BLACK)
        d.ellipse(ebox(ex + 4.5, ey, 1.1, 1.1), fill=BLACK)
    else:
        d.ellipse(ebox(ex, ey, 4, 4), fill=BLACK)
        d.ellipse(ebox(ex - 1.3, ey - 1.3, 1.3, 1.3), fill=WHITE)

def beak(c, hx, hy, open_amount=4):
    bx, by = hx + 16, hy + 6
    c.alpha_composite(glossy(
        SIZE, lambda d, **kw: d.polygon(pts([(bx - 6, by - 6), (bx + 17, by - open_amount), (bx - 6, by + 2)]), **kw),
        ORANGE, ORANGE_LT, hl_rel=(-0.4, -0.45), hl_radius_rel=0.5))
    c.alpha_composite(glossy(
        SIZE, lambda d, **kw: d.polygon(pts([(bx - 6, by + 2), (bx + 15, by + open_amount + 2), (bx - 6, by + 12)]), **kw),
        BEAK, (150, 150, 160, 255), hl_rel=(-0.4, -0.3), hl_radius_rel=0.5))

def crest(c, hx, hy, tilt=0):
    c.alpha_composite(glossy(
        SIZE, lambda d, **kw: d.polygon(pts([(hx - 6 + tilt, hy - 24), (hx + 2, hy - 37), (hx + 6 + tilt, hy - 22)]), **kw),
        RED_DARK, RED_LIGHT, hl_rel=(-0.2, -0.6), hl_radius_rel=0.4))
    c.alpha_composite(glossy(
        SIZE, lambda d, **kw: d.polygon(pts([(hx + 2 + tilt, hy - 25), (hx + 10, hy - 35), (hx + 12 + tilt, hy - 20)]), **kw),
        YELLOW, YELLOW_LT, hl_rel=(-0.2, -0.6), hl_radius_rel=0.4))

# --- Frames ------------------------------------------------------------------

def parrot_base():
    """Transparent bird-on-perch, no scene behind it."""
    c = layer()
    branch(c)
    tail(c)
    wing(c)
    body(c)
    return c

def finish(parrot):
    """Add the contour rim around the bird, then drop it onto its contact shadow."""
    out = contact_shadow()
    out.alpha_composite(with_rim(parrot))
    return out

def make_idle():
    c = parrot_base()
    hx, hy = head(c)
    crest(c, hx, hy)
    eye(c, hx, hy)
    beak(c, hx, hy, open_amount=3)
    return finish(c)

def make_blink():
    c = parrot_base()
    hx, hy = head(c)
    crest(c, hx, hy)
    eye(c, hx, hy, closed=True)
    beak(c, hx, hy, open_amount=3)
    return finish(c)

def make_tilt():
    c = parrot_base()
    hx, hy = head(c, tilt=8, dy=-2)
    crest(c, hx, hy, tilt=8)
    eye(c, hx, hy, tilt=8)
    beak(c, hx, hy, open_amount=3)
    return finish(c)

def make_speak(open_amount):
    c = parrot_base()
    hx, hy = head(c)
    crest(c, hx, hy)
    eye(c, hx, hy)
    beak(c, hx, hy, open_amount=open_amount)
    return finish(c)

def make_flap():
    """Wings-open flap pose: same body/head as idle but spread wings instead of
    the single folded wing."""
    c = layer()
    branch(c)
    tail(c)
    wing_spread(c)
    body(c)
    hx, hy = head(c)
    crest(c, hx, hy)
    eye(c, hx, hy)
    beak(c, hx, hy, open_amount=3)
    return finish(c)

# --- Action-bar button icons (white glyphs on the black bar) -----------------

AICON = 20  # logical action-bar icon size

def aicon():
    return Image.new('RGBA', (AICON * SS, AICON * SS), (0, 0, 0, 0))

def make_icon_talk():
    """Microphone -- SELECT dictates a sentence to speak."""
    c = aicon()
    d = ImageDraw.Draw(c)
    d.rounded_rectangle(ebox(10, 8, 3, 5), radius=3 * SS, fill=WHITE)   # capsule body
    d.arc(ebox(10, 9, 5, 6), start=20, end=160, fill=WHITE, width=2 * SS)  # holder
    d.line(pts([(10, 15), (10, 17)]), fill=WHITE, width=2 * SS)         # stem
    d.line(pts([(7, 17), (13, 17)]), fill=WHITE, width=2 * SS)          # base
    return c

def make_icon_ask():
    """Question mark -- UP asks the AI."""
    c = aicon()
    d = ImageDraw.Draw(c)
    f = ImageFont.truetype(FONT_BOLD, 16 * SS)
    bbox = d.textbbox((0, 0), '?', font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((AICON * SS - tw) / 2 - bbox[0], (AICON * SS - th) / 2 - bbox[1]), '?', font=f, fill=WHITE)
    return c

def make_icon_phrases():
    """A short list -- DOWN opens the quick-phrase menu."""
    c = aicon()
    d = ImageDraw.Draw(c)
    for y in (6, 10, 14):
        d.rounded_rectangle(ebox(10, y, 6, 1.1), radius=1 * SS, fill=WHITE)
    return c

def make_menu_icon():
    isize = (ICON * SS, ICON * SS)
    c = layer(isize)
    c.alpha_composite(glossy(isize, lambda d, **kw: d.ellipse(ebox(13, 14, 10, 10), **kw),
                             RED, RED_LIGHT, outline=RED_DARK, outline_width=SS))
    d = ImageDraw.Draw(c)
    d.ellipse(ebox(9, 11, 3, 3), fill=WHITE)
    d.ellipse(ebox(8, 11, 1.1, 1.1), fill=BLACK)
    c.alpha_composite(glossy(isize, lambda d, **kw: d.polygon(pts([(18, 13), (24.5, 15), (18, 18)]), **kw),
                             ORANGE, ORANGE_LT, hl_radius_rel=0.5))
    c.alpha_composite(glossy(isize, lambda d, **kw: d.polygon(pts([(10, 5), (13, 0.5), (15.5, 6)]), **kw),
                             YELLOW, YELLOW_LT, hl_radius_rel=0.5))
    return c

# --- Render & save -----------------------------------------------------------

def save(img, name, scale=1.0):
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    small = img.resize((img.width // SS, img.height // SS), Image.LANCZOS)
    if scale != 1.0:
        small = small.resize((round(small.width * scale), round(small.height * scale)), Image.LANCZOS)
    small.save(out)
    print('wrote', out, small.size)

if __name__ == '__main__':
    save(make_idle(), 'parrot_idle.png', PARROT_SCALE)
    save(make_blink(), 'parrot_blink.png', PARROT_SCALE)
    save(make_tilt(), 'parrot_tilt.png', PARROT_SCALE)
    save(make_speak(2), 'parrot_speak_1.png', PARROT_SCALE)
    save(make_speak(9), 'parrot_speak_2.png', PARROT_SCALE)
    save(make_flap(), 'parrot_flap.png', PARROT_SCALE)
    save(make_menu_icon(), 'menu_icon.png')
    save(make_icon_talk(), 'action_talk.png')
    save(make_icon_ask(), 'action_ask.png')
    save(make_icon_phrases(), 'action_phrases.png')
