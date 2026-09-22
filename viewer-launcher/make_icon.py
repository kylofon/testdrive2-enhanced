"""Writes app.ico: the road drawn in icon.cpp, for Explorer. Needs Pillow.

    python make_icon.py
"""
from pathlib import Path

from PIL import Image

tile = Image.new("RGBA", (16, 16))


def put(x, y, rgb):
    tile.putpixel((x, y), (rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF, 255))


for y in range(16):
    for x in range(16):
        put(x, y, 0x55FFFF if y < 6 else 0x00AA00)
for y in range(6, 16):  # the road: two pixels wide at the horizon, the whole width at the bottom
    half = 1 + (y - 6) * 7 // 9
    for x in range(8 - half, 8 + half):
        put(x, y, 0x555555 if x in (8 - half, 7 + half) else 0xAAAAAA)
for y in (8, 11, 12, 15):  # the centre line's dashes, longer nearer
    for x in (7, 8):
        put(x, y, 0xFFFF55)

sizes = [16, 20, 24, 32, 48, 64, 256]
big = tile.resize((256, 256), Image.NEAREST)
big.save(Path(__file__).with_name("app.ico"), sizes=[(s, s) for s in sizes])
