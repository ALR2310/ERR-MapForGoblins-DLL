#!/usr/bin/env python3
"""Generate src/generated_shared/goblin_logo.{hpp,cpp}: the MapForGoblins logo as a runtime-injectable
DefineBitsLossless2 bitmap tag + the SWF placement matrix, so the on-map logo (over the decorative
plaque sprite 246) can be injected at runtime WITHOUT shipping/modifying the gfx.

Mirrors what tools/build_vanilla_gfx.py baked into the gfx (clone bitmap-fill shape 1097 into sprite
246, scale 0.38, translate -243, on a 256px square). Here instead: one DefineBitsLossless2 bitmap of
the logo fit to 256x256, registered at runtime at a live charId, and a baked SWF matrix the DLL writes
into a cloned copy of sprite 246's char-10 PlaceObject (re-pointed to our logo image).

DefineBitsLossless2 body: [charId u16=0 placeholder][fmt=5][w u16][h u16][zlib(premult ARGB rows)].
SWF MATRIX (PlaceObject), bit-packed MSB-first: HasScale UB[1]=1, NScaleBits UB[5], ScaleX/Y FB[n]
(signed 16.16), HasRotate UB[1]=0, NTranslateBits UB[5], TranslateX/Y SB[m] (signed twips); byte-padded.
"""
import os, sys, zlib, struct
from pathlib import Path
sys.path.insert(0, os.path.dirname(__file__))
import config
from PIL import Image

PROJ = config.PROJECT_DIR
OUT_DIR = PROJ / "src" / "generated_shared"
LOGO_SRC = PROJ / "assets" / "map_icons" / "MapForGoblins_new.png"
SQ = 256              # 256x256 square (matches the gfx clone's shape-181 bitmap-fill bounds = 256px)
LOGO_SCALE = 0.38     # same transform build_vanilla_gfx.py used for the gfx-baked clone
LOGO_TRANS = -243     # twips; re-centres the 256px square on char-10's centre
N_SCALE_BITS = 17
N_TRANS_BITS = 12

# GLOBAL on-map icon scale. generate_map_icons.icon_matrix() imports this and builds a per-icon SWF
# placement matrix (scale + centering for that icon's cropped W x H). Bump to enlarge ALL custom icons at
# once; per-icon relative size is set by how big each glyph is drawn in its source canvas.
ICON_SCALE_CUSTOM = 0.50


def fit_square(img, size=SQ):
    """Logo scaled to full height, centered horizontally in a transparent size x size square."""
    img = img.convert("RGBA")
    s = size / img.height
    w = max(1, round(img.width * s))
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(img.resize((w, size), Image.LANCZOS), ((size - w) // 2, 0))
    return out


def lossless_body(img):
    """DefineBitsLossless2 body (charId placeholder 0) from an RGBA image (premultiplied ARGB)."""
    w, h = img.size
    px = img.load()
    raw = bytearray(w * h * 4)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            raw[i] = a
            raw[i + 1] = (r * a) // 255
            raw[i + 2] = (g * a) // 255
            raw[i + 3] = (b * a) // 255
            i += 4
    z = zlib.compress(bytes(raw), 9)
    return struct.pack('<HBHH', 0, 5, w, h) + z


class BitW:
    def __init__(self): self.acc = 0; self.n = 0; self.out = bytearray()
    def write(self, val, nbits):
        for i in range(nbits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((val >> i) & 1); self.n += 1
            if self.n == 8:
                self.out.append(self.acc); self.acc = 0; self.n = 0
    def bytes(self):
        if self.n:
            self.out.append(self.acc << (8 - self.n))
        return bytes(self.out)


def _fb_bits(*values):
    """Bit count for a SIGNED SWF fixed-point field holding all `values` (SB[n], two's complement)."""
    return max(2, max(v.bit_length() for v in values) + 1)


def swf_matrix(sx, sy, tx, ty, nS=None, nT=None):
    """Build a SWF MATRIX. Scale and translate fields are SIGNED (SB[n]), so the field must be WIDE
    ENOUGH for the value including its sign bit - masking into a fixed width silently flips the sign.
    With the old fixed nS=17, a scale of 1.0 encodes 0x10000, whose bit 16 IS the sign bit, and the
    engine read it back as -1.0: a mirrored icon. Same for nT=12, which holds only +-2048 twips
    (+-102.4 px) before wrapping. Sizes are now derived from the values and asserted.
    Audited 2026-07-28."""
    fsx, fsy = round(sx * 65536), round(sy * 65536)
    itx, ity = int(tx), int(ty)
    nS = nS or _fb_bits(fsx, fsy)
    nT = nT or _fb_bits(itx, ity)
    if nS > 31 or nT > 31:
        raise ValueError(f"SWF MATRIX field too wide: nS={nS} nT={nT} (scale {sx},{sy} trans {tx},{ty})")
    bw = BitW()
    bw.write(1, 1); bw.write(nS, 5)
    bw.write(fsx & ((1 << nS) - 1), nS)
    bw.write(fsy & ((1 << nS) - 1), nS)
    bw.write(0, 1); bw.write(nT, 5)
    bw.write(itx & ((1 << nT) - 1), nT)
    bw.write(ity & ((1 << nT) - 1), nT)
    return bw.bytes()


def carr(name, b):
    return f"    const unsigned char {name}[] = {{{','.join(str(x) for x in b)}}};\n"


def main():
    if not LOGO_SRC.exists():
        sys.exit(f"[logo] missing {LOGO_SRC}")
    img = fit_square(Image.open(LOGO_SRC))
    tag = lossless_body(img)
    mat = swf_matrix(LOGO_SCALE, LOGO_SCALE, LOGO_TRANS, LOGO_TRANS)
    print(f"[logo] {LOGO_SRC.name} -> lossless {len(tag)} B")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "goblin_logo.hpp").write_text(
        "#pragma once\n#include <cstdint>\n\n"
        "// AUTO-GENERATED by tools/generate_logo.py. The MapForGoblins logo as a runtime-injectable\n"
        "// DefineBitsLossless2 bitmap + the SWF placement matrix. The DLL registers LOGO_TAG at a live\n"
        "// charId (the last id of the icon window, chosen at runtime at sprite-171 load), then re-points\n"
        "// sprite 246's char-10 PlaceObject to it and overwrites its matrix with LOGO_MATRIX. So the\n"
        "// on-map logo needs no gfx edit. See goblin_gfx_probe.\n"
        "// (Map-ICON placement matrices are per-icon and live in goblin_map_icons, not here.)\n"
        "namespace goblin::generated\n{\n"
        "    extern const unsigned char LOGO_TAG[];      // DefineBitsLossless2 body (charId u16 @+0 = placeholder)\n"
        "    extern const unsigned LOGO_TAG_LEN;\n"
        "    extern const unsigned char LOGO_MATRIX[];   // SWF MATRIX bytes, written at PO3 body +0xe\n"
        "    extern const unsigned LOGO_MATRIX_LEN;\n"
        "}\n", encoding="utf-8")

    (OUT_DIR / "goblin_logo.cpp").write_text(
        '#include "goblin_logo.hpp"\n'
        "namespace goblin::generated\n{\n"
        f"{carr('LOGO_TAG', tag)}"
        f"    const unsigned LOGO_TAG_LEN = {len(tag)}u;\n"
        f"{carr('LOGO_MATRIX', mat)}"
        f"    const unsigned LOGO_MATRIX_LEN = {len(mat)}u;\n"
        "}\n", encoding="utf-8")
    print(f"[logo] wrote goblin_logo.cpp/.hpp")


if __name__ == "__main__":
    main()
