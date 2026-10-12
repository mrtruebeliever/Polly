#!/usr/bin/env python3
"""Generates Polly's launcher menu icon: the scarlet macaw on its perch.

The launcher draws menu icons in four greys (black, dark grey, light grey,
white) on white, so the icon is crisp pixel art in Pebble palette colours
chosen for their grey level, with every part outlined in black; pale colours
without an outline would vanish.

Output:
  resources/images/menu_icon.png   25x25 on transparent

Run from the project root (needs Pillow):  python3 tools/gen_icon.py
"""
import os

from PIL import Image

N = 25
PAL = {
    "K": (0x00, 0x00, 0x00), "W": (0xFF, 0xFF, 0xFF), "L": (0xAA, 0xAA, 0xAA),
    "D": (0x55, 0x55, 0x55), "R": (0xFF, 0x00, 0x00), "r": (0xAA, 0x00, 0x00),
    "o": (0xFF, 0x55, 0x55), "Y": (0xFF, 0xFF, 0x00), "y": (0xFF, 0xAA, 0x00),
    "i": (0xFF, 0xFF, 0x55), "A": (0xAA, 0xAA, 0x55), "a": (0x55, 0x55, 0x00),
    "S": (0xFF, 0xAA, 0x55), "s": (0xFF, 0x55, 0x00), "N": (0xAA, 0x55, 0x00),
    "n": (0x55, 0x00, 0x00), "b": (0x00, 0x00, 0x55), "B": (0x00, 0x55, 0xAA),
    "c": (0x00, 0x55, 0xFF), "C": (0xAA, 0xFF, 0xFF), "E": (0x00, 0xFF, 0xAA),
    "e": (0x00, 0xAA, 0x55), "p": (0x00, 0x55, 0x00), "M": (0x55, 0xAA, 0x00),
    "Z": (0xAA, 0xFF, 0x55), "T": (0xAA, 0xFF, 0xAA), "m": (0xAA, 0x00, 0xAA),
    "P": (0x00, 0xAA, 0x00), "G": (0x55, 0xAA, 0xAA), "g": (0x00, 0x55, 0x55),
}


def disc(cx, cy, r):
    return {(x, y) for y in range(N) for x in range(N) if (x - cx) ** 2 + (y - cy) ** 2 <= r * r}


def ell(cx, cy, rx, ry):
    return {(x, y) for y in range(N) for x in range(N)
            if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1}


def rect(x0, y0, x1, y1):
    return {(x, y) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)}


def part(g, cells, shade, diag=False):
    nb = [(1, 0), (-1, 0), (0, 1), (0, -1)] + ([(1, 1), (-1, -1), (1, -1), (-1, 1)] if diag else [])
    for (x, y) in cells:
        for dx, dy in nb:
            p = (x + dx, y + dy)
            if p not in cells and 0 <= p[0] < N and 0 <= p[1] < N:
                g[p[1]][p[0]] = "K"
    for (x, y) in cells:
        if 0 <= x < N and 0 <= y < N:
            g[y][x] = shade(x, y) if callable(shade) else shade


def new():
    return [["."] * N for _ in range(N)]


def to_img(g):
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    for y in range(N):
        for x in range(N):
            if g[y][x] != ".":
                img.putpixel((x, y), PAL[g[y][x]] + (255,))
    return img



def art():
    g = new()
    # tail
    tail = {(x, y) for y in range(17, 25) for x in range(N) if 2 + (y - 17) // 2 <= x <= 6 + (y - 17) // 3}
    part(g, tail, lambda x, y: "c" if x < 4 + (y - 17) // 2 else "B")
    # body with wing
    body = ell(9, 16, 6, 6.5)
    part(g, body, "R")
    wing = ell(7.5, 17, 4, 5)
    part(g, wing, lambda x, y: "y" if y < 15 else ("c" if y < 19 else "B"))
    # head
    head = disc(13, 8, 6.4)
    part(g, head, lambda x, y: "o" if (x - 10) ** 2 + (y - 4) ** 2 < 5 else "R")
    # white face patch + eye
    patch = ell(15.5, 8, 3, 3.2)
    for (x, y) in patch:
        g[y][x] = "W"
    for (x, y) in ((15, 7), (16, 7), (15, 8), (16, 8)):
        g[y][x] = "K"
    g[7][15] = "W"
    # beak: hooked upper, dark lower
    up = {(19, 6), (20, 6), (21, 6), (19, 7), (20, 7), (21, 7), (22, 7), (19, 8), (20, 8),
          (21, 8), (22, 8), (23, 8), (19, 9), (20, 9), (21, 9), (22, 9), (23, 9), (23, 10), (22, 10)}
    part(g, up, lambda x, y: "W" if y < 8 and x < 21 else "L")
    low = {(19, 11), (20, 11), (21, 11), (19, 12), (20, 12)}
    part(g, low, "D")
    # crest
    for (x, y) in ((9, 1), (10, 1), (8, 2), (9, 2)):
        g[y][x] = "R"
    for (x, y) in ((8, 0), (9, 0), (10, 0), (11, 0), (7, 1), (11, 1), (7, 2)):
        if g[y][x] == ".":
            g[y][x] = "K"
    # feet on a perch
    part(g, rect(8, 23, 23, 23), "N")
    for x in (10, 13):
        g[22][x] = "D"
    return g


def main():
    os.makedirs("resources/images", exist_ok=True)
    to_img(art()).save("resources/images/menu_icon.png")
    print("wrote resources/images/menu_icon.png (25x25)")


if __name__ == "__main__":
    main()
