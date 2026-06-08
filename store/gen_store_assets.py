#!/usr/bin/env python3
"""Generate Polly store/README assets from the real app resources.

Screenshots reproduce parrot_window.c's layout faithfully at the PT2 (emery)
resolution of 200x228: state label band (22px, Gothic-18, white), parrot sprite
centered below it, and the speech bubble (rounded rect, white fill / dark-gray
border, radius 10) pinned to the bottom. Icons are composed from the idle parrot
over a jungle-green gradient.

Run:  .art-venv/bin/python store/gen_store_assets.py
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "resources", "images")
OUT = os.path.join(ROOT, "store")
os.makedirs(OUT, exist_ok=True)

# --- PT2 / parrot_window.c geometry -----------------------------------------
W, H = 200, 228
STATE_LABEL_H = 22
BUBBLE_H, BUBBLE_MARGIN = 64, 8
BUBBLE = (BUBBLE_MARGIN, H - BUBBLE_H - BUBBLE_MARGIN, W - BUBBLE_MARGIN, H - BUBBLE_MARGIN)
RADIUS = 10
WHITE, BLACK, DARKGRAY = (255, 255, 255), (0, 0, 0), (85, 85, 85)  # GColorDarkGray = #555

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def fit_font(draw, text, max_w, start=15, bold=False, min_size=10):
    path = FONT_BOLD if bold else FONT
    size = start
    while size > min_size:
        f = ImageFont.truetype(path, size)
        if draw.textlength(text, font=f) <= max_w:
            return f
        size -= 1
    return ImageFont.truetype(path, min_size)


def wrap(draw, text, font, max_w):
    lines, cur = [], ""
    for word in text.split():
        trial = (cur + " " + word).strip()
        if draw.textlength(trial, font=font) <= max_w:
            cur = trial
        else:
            if cur:
                lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def draw_label(img, text):
    d = ImageDraw.Draw(img)
    f = fit_font(d, text, W - 8, start=15)
    tw = d.textlength(text, font=f)
    asc, desc = f.getmetrics()
    x = (W - tw) / 2
    y = (STATE_LABEL_H - (asc + desc)) / 2
    # soft shadow for legibility over the backdrop (watch relies on darkened top)
    d.text((x + 1, y + 1), text, font=f, fill=(0, 0, 0, 130))
    d.text((x, y), text, font=f, fill=WHITE)


def draw_bubble(img, text, color):
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = BUBBLE
    d.rounded_rectangle([x0, y0, x1, y1], radius=RADIUS, fill=WHITE, outline=DARKGRAY, width=1)
    inner_w = (x1 - x0) - 12
    f = ImageFont.truetype(FONT, 15)
    lines = wrap(d, text, f, inner_w)
    line_h = sum(f.getmetrics())
    total_h = line_h * len(lines)
    ty = y0 + ((y1 - y0) - total_h) / 2
    for ln in lines:
        lw = d.textlength(ln, font=f)
        d.text((x0 + ((x1 - x0) - lw) / 2, ty), ln, font=f, fill=color)
        ty += line_h


def screenshot(name, pose, label, bubble_text=None, bubble_color=BLACK):
    bg = Image.open(os.path.join(IMG, "jungle_bg.png")).convert("RGBA")
    parrot = Image.open(os.path.join(IMG, pose)).convert("RGBA")
    pw, ph = parrot.size
    px = (W - pw) // 2
    py = STATE_LABEL_H + (H - STATE_LABEL_H - ph) // 2
    bg.alpha_composite(parrot, (px, py))
    if bubble_text:
        draw_bubble(bg, bubble_text, bubble_color)
    draw_label(bg, label)
    out = bg.convert("RGB")
    out.save(os.path.join(OUT, name + ".png"))
    # 3x nearest-neighbour for crisp README display
    out.resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(OUT, name + "@3x.png"))
    print("screenshot:", name)


# --- screenshots -------------------------------------------------------------
DEMO = "Hello! How are you today?"
screenshot("screenshot_idle", "parrot_idle.png", "Press SELECT to talk to Polly")
screenshot("screenshot_thinking", "parrot_idle.png", "Thinking...", DEMO)
screenshot("screenshot_speaking", "parrot_speak_1.png", "Speaking...", DEMO)


# --- icons -------------------------------------------------------------------
def vgrad(size, top, bottom):
    g = Image.new("RGB", (1, size), 0)
    for y in range(size):
        t = y / (size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return g.resize((size, size))


def make_icon(px):
    # jungle-green gradient backdrop, lighter at top like the app's sky/foliage
    icon = vgrad(px, (74, 142, 78), (28, 74, 40)).convert("RGBA")
    parrot = Image.open(os.path.join(IMG, "parrot_idle.png")).convert("RGBA")
    target_h = int(px * 0.82)
    scale = target_h / parrot.height
    parrot = parrot.resize((int(parrot.width * scale), target_h), Image.LANCZOS)
    ox = (px - parrot.width) // 2
    oy = px - parrot.height - int(px * 0.04)  # sit near the bottom
    # soft contact shadow
    shadow = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sw = int(parrot.width * 0.7)
    sd.ellipse([px // 2 - sw // 2, px - int(px * 0.12), px // 2 + sw // 2, px - int(px * 0.02)],
               fill=(0, 0, 0, 90))
    icon.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(px * 0.02)))
    icon.alpha_composite(parrot, (ox, oy))
    icon.convert("RGB").save(os.path.join(OUT, f"icon_{px}.png"))
    print("icon:", px)


make_icon(80)
make_icon(144)
print("done ->", OUT)
