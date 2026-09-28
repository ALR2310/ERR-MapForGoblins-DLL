#!/usr/bin/env python3
"""Downscale the spoiler-free "?" art to a 160x160 map-icon overlay asset.

Source: assets/badges/gray_question_mark.png (user-provided).
Output: assets/badges/anon_qmark.png (160x160 RGBA, centered, transparent pad).

160 matches the vanilla item-overlay texture size (MENU_ItemIcon_* = 160), so
build_vanilla_gfx can embed it and place it at the standard overlay scale."""
from pathlib import Path
import config
from PIL import Image

SRC = config.PROJECT_DIR / "assets" / "badges" / "gray_question_mark.png"
DST = config.PROJECT_DIR / "assets" / "badges" / "anon_qmark.png"
SIZE = 160
PAD = 0.86  # glyph fills 86% of the tile, small transparent margin

def main():
    im = Image.open(SRC).convert("RGBA")
    # trim to the non-transparent bounding box so scaling is consistent
    bbox = im.getbbox()
    if bbox:
        im = im.crop(bbox)
    target = int(SIZE * PAD)
    im.thumbnail((target, target), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    canvas.paste(im, ((SIZE - im.width) // 2, (SIZE - im.height) // 2), im)
    canvas.save(DST)
    print(f"wrote {DST} ({canvas.size})")

if __name__ == "__main__":
    main()
