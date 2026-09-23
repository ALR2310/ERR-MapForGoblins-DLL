#!/usr/bin/env python3
"""Generate src/generated_shared/goblin_map_icons.{hpp,cpp}: one DefineBitsLossless2 tag per
custom map icon, built from the SAME rendered per-iconId PNGs the in-game overlay atlas uses
(tools/generate_overlay_icons.py). Single source -> both the map (runtime Scaleform frame
injection) and the menu overlay (ImGui atlas) draw the identical art; update an icon once and
it propagates to both.

The DLL adds each tag to the live worldmap movie while it parses (DefineBitsLossless2 loader), gives
each a character id chosen at runtime from THAT movie (one past the highest id its tag stream and its
registry hold, plus a margin - goblin_gfx_probe.cpp choose_charid_window; no id is decided here),
appends a frame, and remaps every marker whose baked iconId is one of these to the resulting iconId.
So no custom gfx needs to ship; the base gfx's vanilla icons (1-348) stay as-is.

DefineBitsLossless2 body layout, kind 3 (the default: a 64-colour palette from pngquant):
  [charId u16][kind=3 u8][width u16][height u16][colours-1 u8][zlib( table of [R*,G*,B*,A] x colours,
  then one index byte per pixel, rows padded to 4 bytes )]
and kind 5 (MFG_ICON_PALETTE=0: 32-bit premultiplied ARGB):
  [charId u16][kind=5 u8][width u16][height u16][zlib( rows of [A,R*,G*,B*] )]
charId is emitted as 0 (placeholder); the DLL patches it to the live charId at inject time.
"""
import os, sys, re, zlib, struct, subprocess, tempfile
from pathlib import Path
sys.path.insert(0, os.path.dirname(__file__))
import generate_overlay_icons as g
import generate_logo  # swf_matrix + ICON_SCALE_CUSTOM (per-icon centering matrix for tight crops)
from PIL import Image, ImageChops

PROJ = g.PROJ
OUT_DIR = PROJ / "src" / "generated_shared"
SIZE = 96   # normalize every icon to SIZE x SIZE. Placed with the grace matrix, on-map size is
            # proportional to SIZE (grace matrix was tuned for the 160px anon "?"); 96 ~= 60% of that,
            # to match the gfx's smaller per-icon scaling. Bump if icons read too small.

# (FALLBACK_CHARID_BASE = 13764 stood here and was emitted as MAP_ICON_CHARID_BASE. Removed 2026-09-23:
# the character ids are chosen only at runtime, per world-map parse, from the movie that actually
# loaded. Nothing in the build decides or checks them. NO gfx is read here.)

def icon_set():
    """Every iconId we build a tag for = the FULL registry icon set (profile-independent superset).
    Every marker's baked iconId is a registry slug's id (no hardcoded iconIds remain), so this covers
    all markers on every profile; non-ERR builds simply use a subset (extra tags are harmless). Reading
    the static registry instead of the per-profile baked map data is what makes generated_shared the
    same complete set for all four builds."""
    import icon_registry
    return sorted({icon_registry.iconid(s) for s in icon_registry.all_slugs()})


ALPHA_GAMMA = 0.9  # <1 lifts semi-transparent pixels toward opaque (applied BEFORE premultiply, so it
                   # stays consistent; fully-opaque pixels unchanged = no over-bright). Gentle nudge to
                   # close the small residual vs the overlay (Scaleform premult-blend over the dark map
                   # reads a touch fainter than ImGui). Dial toward 0.8 for more punch, 1.0 = off.
                   # (source art + downscale leave many semi-alpha pixels -> faint over the dark bg). 0->0,
                   # 255->255 preserved; lower = more opaque/punchy. Tune to taste.


def normalize(img, size=SIZE):
    """Scale the WHOLE source canvas to fit `size` px (longest side) FIRST, THEN crop to the alpha bbox.
    Returns a PREMULTIPLIED RGBA image cropped TIGHT to the glyph (variable W x H <= SIZE); icon_matrix()
    builds the per-icon matrix that centers it on the marker anchor. lossless_body must NOT premultiply
    again.

    Why scale-before-crop: it preserves the size you DREW the glyph at within the (consistent-size) source
    canvas - a glyph drawn smaller stays smaller on the map, larger stays larger (direct artist control),
    instead of every icon being blown up to fill SIZE (which lost relative sizing and made size wobble
    with each icon's bbox/aspect). The crop then keeps the tag TIGHT to the real texture (no empty-alpha
    border) so the marker's hit/label region is the glyph, not padding.

    Why premultiply BEFORE the resize: it's the fix for detailed/AA icons (e.g. memory) rendering far MORE
    transparent on the map than in the source - a straight (non-premultiplied) downscale spreads/loses
    alpha at thin & AA features (chunky icons like nodes barely change). Mirrors the Icon Preview's
    alpha-aware (stbir RGBA) resize, so map == preview == source."""
    img = img.convert("RGBA")
    r, gg, b, al = img.split()
    if ALPHA_GAMMA != 1.0:  # solidify: gamma-lift alpha BEFORE premultiply (keeps premult consistent)
        al = al.point(lambda v: min(255, round(255 * (v / 255.0) ** ALPHA_GAMMA)))
    premul = Image.merge("RGBA", (ImageChops.multiply(r, al), ImageChops.multiply(gg, al),
                                  ImageChops.multiply(b, al), al))  # r*a/255 ... premultiplied
    s = min(size / premul.width, size / premul.height)              # fit the WHOLE canvas (no crop yet)
    w, h = max(1, round(premul.width * s)), max(1, round(premul.height * s))
    premul = premul.resize((w, h), Image.LANCZOS)                   # alpha-weighted (data is premultiplied)
    bb = premul.split()[3].point(lambda v: 255 if v > 8 else 0).getbbox()  # crop AFTER scaling
    if bb:
        premul = premul.crop(bb)
    return premul


def icon_matrix(w, h):
    """Per-icon SWF placement matrix: scale ICON_SCALE_CUSTOM, translated to CENTER this W x H bitmap on
    the marker anchor (centering twips = -(dim*scale/2)*20 = -dim*scale*10). Per-icon because the crop is
    tight, so each bitmap carries its own size (a shared matrix can only center one fixed size)."""
    sc = generate_logo.ICON_SCALE_CUSTOM
    return generate_logo.swf_matrix(sc, sc, -round(w * sc * 10), -round(h * sc * 10))


# Every icon ships as a 64-colour palette tag (DefineBitsLossless2 kind 3), quantised by pngquant -
# a real RGBA quantiser, alpha is part of the palette. The 32-bit ARGB tags (kind 5) were 1.27 MB of
# a 5 MB DLL and zlib barely dented them; the palette costs 216 KB and was judged on the map on
# 2026-09-22: fine, a little soft up close. MFG_ICON_PALETTE=N picks another size, 0 = 32-bit.
PALETTE_COLOURS = int(os.environ.get("MFG_ICON_PALETTE", "64") or 0)


def palette_body(img, colours):
    """DefineBitsLossless2 body, kind 3: a colour table of premultiplied R,G,B,A entries plus one
    index byte per pixel, rows padded to 4 bytes, zlib. `img` is the premultiplied crop normalize()
    returns; pngquant wants straight alpha, so the premultiply is undone for it and redone on the
    palette entries it picks."""
    import config
    pq = getattr(config, "PNGQUANT", None)
    if not pq or not Path(pq).is_file():
        raise SystemExit(f"pngquant.exe not found at {pq} (tools/lib/pngquant, or [paths] pngquant "
                         "in tools/config.ini)")
    w, h = img.size
    px = img.load()
    straight = Image.new("RGBA", (w, h))
    sp = straight.load()
    for y in range(h):
        for x in range(w):
            r, gg, b, a = px[x, y]
            sp[x, y] = ((min(255, (r * 255 + a // 2) // a), min(255, (gg * 255 + a // 2) // a),
                         min(255, (b * 255 + a // 2) // a), a) if a else (0, 0, 0, 0))
    with tempfile.TemporaryDirectory() as td:
        src, dst = Path(td) / "in.png", Path(td) / "out.png"
        straight.save(src, "PNG")
        r = subprocess.run([str(pq), "--force", "--speed", "1", "--quality", "0-100",
                            "--output", str(dst), str(colours), "--", str(src)],
                           capture_output=True, text=True)
        if r.returncode not in (0, 98, 99) or not dst.is_file():
            raise SystemExit(f"pngquant failed ({r.returncode}): {r.stderr.strip()}")
        q = Image.open(dst)
        q.load()
    if q.mode != "P":
        raise SystemExit(f"pngquant returned a {q.mode} image, expected a palette")
    # The alpha of a PNG8 palette lives in the tRNS chunk, which Pillow keeps in info["transparency"]
    # (one byte per entry, shorter than the palette = the rest opaque) and does NOT fold into
    # getpalette("RGBA") - that call answers 255 for every entry, which shipped a build whose icon
    # edges were solid on the map while the same PNGs looked right in a browser.
    pal = q.getpalette("RGB") or []
    n = len(pal) // 3
    if not 1 <= n <= 256:
        raise SystemExit(f"palette of {n} entries")
    trns = q.info.get("transparency", b"")
    if isinstance(trns, int):
        trns = bytes(255 if i != trns else 0 for i in range(n))
    alphas = [trns[i] if i < len(trns) else 255 for i in range(n)]
    if all(a == 255 for a in alphas) and any(px[x, y][3] < 255 for y in range(h) for x in range(w)):
        raise SystemExit("palette came back fully opaque for an icon that has transparent pixels")
    table = bytearray()
    for i in range(n):
        r, gg, b = pal[i * 3:i * 3 + 3]
        a = alphas[i]
        table += bytes(((r * a + 127) // 255, (gg * a + 127) // 255, (b * a + 127) // 255, a))
    stride = (w + 3) & ~3
    idx = q.tobytes()
    rows = bytearray()
    pad = bytes(stride - w)
    for y in range(h):
        rows += idx[y * w:(y + 1) * w] + pad
    z = zlib.compress(bytes(table + rows), 9)
    return struct.pack("<HBHHB", 0, 3, w, h, n - 1) + z


def lossless_body(img):
    """DefineBitsLossless2 body (charId placeholder 0) from an ALREADY-normalized (premultiplied, cropped)
    RGBA image - do NOT normalize or premultiply again here."""
    if PALETTE_COLOURS:
        return palette_body(img, PALETTE_COLOURS)
    w, h = img.size
    px = img.load()
    raw = bytearray(w * h * 4)
    i = 0
    for y in range(h):
        for x in range(w):
            r, gg, b, a = px[x, y]
            # normalize() already returns PREMULTIPLIED RGBA (premult done before the alpha-weighted
            # resize), so write the channels straight here - do NOT premultiply again.
            raw[i] = a
            raw[i + 1] = r
            raw[i + 2] = gg
            raw[i + 3] = b
            i += 4
    z = zlib.compress(bytes(raw), 9)
    return struct.pack('<HBHH', 0, 5, w, h) + z


def main():
    # The bitmaps' character ids are chosen at RUNTIME, per world-map parse (choose_charid_window);
    # every tag is emitted with a 0 placeholder id. NO gfx is scanned (pure-DLL build).
    print("[map-icons] " + (f"{PALETTE_COLOURS}-colour palette tags (kind 3) via pngquant"
                            if PALETTE_COLOURS else "32-bit ARGB tags (kind 5)"))
    icons = icon_set()
    g.render_icons(icons)  # no-op stub (warns on any iconId lacking committed PNG art); NO gfx render

    entries = []  # (iconId, body, matrix)
    for icon in icons:
        img = g.icon_image(icon)
        if img is None:
            print(f"[map-icons] WARN no image for iconId {icon}; skipped")
            continue
        norm = normalize(img)                 # premultiplied, cropped TIGHT to the glyph (variable WxH)
        body = lossless_body(norm)
        mat = icon_matrix(norm.width, norm.height)  # per-icon centering matrix (depends on the crop size)
        entries.append((icon, body, mat))
        print(f"  iconId {icon:4} -> {norm.width:2}x{norm.height:2}px, tag {len(body)} bytes")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    hpp = OUT_DIR / "goblin_map_icons.hpp"
    cpp = OUT_DIR / "goblin_map_icons.cpp"

    hpp.write_text(
        "#pragma once\n#include <cstdint>\n\n"
        "// AUTO-GENERATED by tools/generate_map_icons.py. One DefineBitsLossless2 tag per custom map\n"
        "// icon, from the same source renders as the overlay atlas. The DLL adds these while the worldmap\n"
        "// parses (character ids chosen at runtime from that movie; there is no build-time id), appends a\n"
        "// frame per icon, and remaps markers baked at srcIconId to the resulting iconId.\n"
        "// See reference_scaleform_displaylist.\n"
        "namespace goblin::generated\n{\n"
        "    // Each icon ships its TIGHT (alpha-cropped) bitmap tag PLUS its own placement matrix: the crop\n"
        "    // is tight so centering depends on each bitmap's W x H (no shared matrix). The DLL appends a\n"
        "    // frame placed with this matrix; the overlay atlas letterboxes the same tag into a square cell.\n"
        "    struct MapIconTag { int srcIconId; const unsigned char *tag; unsigned tagLen; "
        "const unsigned char *matrix; unsigned matrixLen; };\n"
        "    extern const MapIconTag MAP_ICON_TAGS[];\n"
        "    extern const int MAP_ICON_TAG_COUNT;\n"
        "}\n", encoding="utf-8")

    out = ['#include "goblin_map_icons.hpp"\n', "namespace goblin::generated\n{\n"]
    for icon, body, mat in entries:
        out.append(f"    static const unsigned char TAG_{icon}[] = {{{','.join(str(b) for b in body)}}};\n")
        out.append(f"    static const unsigned char MAT_{icon}[] = {{{','.join(str(b) for b in mat)}}};\n")
    out.append("    const MapIconTag MAP_ICON_TAGS[] = {\n")
    for icon, body, mat in entries:
        out.append(f"        {{{icon}, TAG_{icon}, {len(body)}u, MAT_{icon}, {len(mat)}u}},\n")
    out.append("    };\n")
    out.append(f"    const int MAP_ICON_TAG_COUNT = {len(entries)};\n")
    out.append("}\n")
    cpp.write_text("".join(out), encoding="utf-8")
    print(f"[map-icons] wrote {len(entries)} icons -> {cpp.name} ({cpp.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
