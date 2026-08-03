"""Category icons for the IN-GAME MENU rows (movie 02_160_KeyConfiguration).

The menu row clip has no image child at all, and an image registered into another movie is not
reachable from this one - so the icons have to become part of THIS movie. They are handed to the
game's own parser as an EXTENDED copy of the movie bytes when the movie LOADS
(goblin_own_movie.cpp), which is the one route that does not disturb a live movie: the icon ends
up an ordinary timeline child that the engine creates and destroys itself, and the DLL only
resolves it by name and picks its frame. Creating display objects in a live movie was tried and
crashed three times over; see the note in goblin_stall_probe.cpp.

What this generator emits (src/generated_shared/goblin_menu_icon_tags.hpp):
  ICON_BLOB[] = N x DefineBitsLossless2 (one per category icon, fresh charIds)
                + ONE DefineSprite with N frames, frame i placing bitmap i
                + the SLIDER strip (see below)
  ROW_TAG[]   = a REPLACEMENT DefineSprite tag for cid 189 whose frame 1 additionally places
                that icon sprite as a named child "MfgIcon" and the slider strip as "MfgSlider"
  ICON_FRAME_OF_KEY[] = ini key -> frame number inside the icon sprite (1-based)

The SLIDER strip is the native settings slider rebuilt for this movie. In the game's own
widget (02_042_PC_GraphicSetting, sprite 78 "Slider") the whole visual is ONE external
texture, MENU_FL_Slider (396x40, drawn 1:1), sliding to the right under a 400x36 mask -
sprite 76 gives it 1000 frames, one per position. A frame-driven copy is out (a multi-frame
sprite ANIMATES in every instance we do not drive - the game's own key-binding screen uses
this very row clip), so the same look is authored the way the icons are: a strip of
SLIDER_CELLS static cells, cell k = the texture at the position for fraction k/(cells-1),
behind one row-level mask window; showing a value is a horizontal shift of the strip, and
cell 0 is empty so untouched instances show nothing. Geometry measured from 02_042
(scratch/recon_042_slider_geom.py): image x -591.45px (0%) .. -202px (100%), mask window
x -196.5..203.5, y -36..0, image y -37.15.

The DLL then splices:  orig[0..tag189start) + ICON_BLOB + ROW_TAG + orig[tag189end..)
so our defs are parsed immediately before the row clip that uses them (both are defines, so
this lands before the movie's first frame either way), and the row keeps its original charId.

Run by the shared stage of the build; standalone: py tools/generate_menu_icon_tags.py
"""
import importlib.util as u
from PIL import Image
import struct
import sys
import os

sys.path.insert(0, 'tools')
sys.path.insert(0, 'scratch')

import config

GFX = str(config.GAME_DIR / 'menu' / '02_160_keyconfiguration.gfx')
GFX_042 = str(config.GAME_DIR / 'menu' / 'win' / '02_042_pc_graphicsetting.gfx')
OUT = 'src/generated_shared/goblin_menu_icon_tags.hpp'
ROW_CID = 189          # the row clip we extend
ICON_PX = 32           # icons are drawn into a 40px-tall row
ICON_X = 14.0          # px. Was 2.0 - flush with the separator caption, which ate the indent
                       # ordinary rows have; 14 leaves that indent visible and still clears the label
ICON_Y = 11.0         # rows are 63.7 apart; 4 sat too high and 18 too low, measured in game

# ── the slider strip (native look, shift-driven) ──────────────────────────────────────
SLIDER_IMG_CID_042 = 28      # MENU_FL_Slider's GFX_DefineExternalImage2 in 02_042
# Native geometry, px, in the widget's own space (recon_042_slider_geom.py):
SL_IMG_X0, SL_IMG_X1 = -591.45, -202.0   # sliding image x at 0% and at 100%
SL_IMG_Y = -37.15
SL_WIN_L, SL_WIN_R = -196.5, 203.5       # mask window (shape 75 under scale 1.0 x 1.286)
SL_WIN_T, SL_WIN_B = -36.0, 0.0
SL_SCALE = 0.45              # 400x36 native window -> 180x16.2 in our value column
SLIDER_CELLS = 100           # cells 1..100 = 0..100%; cell 0 is empty on purpose
# Strip pitch must exceed window width (180) + scaled image width (396*0.45 = 178.2) so a
# neighbouring cell's texture can never reach into the visible window.
SLIDER_PITCH = 384           # px between cells
SLIDER_X = 586.0             # window position in the row: the value column starts at 582.6
SLIDER_Y = 19.0              # bar 16.2px tall, centred on the icons' 11..43 band

PLACE2, PLACE3, SHOWFRAME, END, DEFSPRITE, REMOVE2, LOSSLESS2 = 26, 70, 1, 0, 39, 28, 36


def build_tag(tt, body):
    n = len(body)
    if n >= 0x3f:
        return struct.pack('<HI', (tt << 6) | 0x3f, n) + bytes(body)
    return struct.pack('<H', (tt << 6) | n) + bytes(body)


def matrix_bytes(scale, tx_px, ty_px):
    """Minimal SWF MATRIX: HasScale=1 (16.16), no rotate, translate in twips.

    Audited 2026-07-28 and correct as written: the scale field is 20 bits, which in signed 16.16 spans
    about +-8.0, so scale 1.0 (0x10000, 18 bits with its sign) fits without tripping the sign bit, and
    the translate width is already derived from the values. generate_logo.swf_matrix had the opposite
    problem with a fixed 17-bit scale field and was changed to size its fields the same way."""
    bits = []

    def put(v, n):
        for i in range(n - 1, -1, -1):
            bits.append((v >> i) & 1)

    def put_signed(v, n):
        put(v & ((1 << n) - 1), n)

    s = int(round(scale * 65536))
    put(1, 1)          # HasScale
    put(20, 5)         # NScaleBits
    put_signed(s, 20)
    put_signed(s, 20)
    put(0, 1)          # HasRotate
    tx = int(round(tx_px * 20))
    ty = int(round(ty_px * 20))
    nbits = max(tx.bit_length(), ty.bit_length()) + 1
    nbits = max(nbits, 1)
    put(nbits, 5)
    put_signed(tx, nbits)
    put_signed(ty, nbits)
    while len(bits) % 8:
        bits.append(0)
    out = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for bit in bits[i:i + 8]:
            b = (b << 1) | bit
        out.append(b)
    return bytes(out)


def place2(cid, depth, matrix=b'', name=None, clip_depth=None):
    flags = 0x02 | (0x04 if matrix else 0)   # HasCharacter | HasMatrix
    if name:
        flags |= 0x20                        # HasName
    if clip_depth is not None:
        flags |= 0x40                        # HasClipDepth -> this object MASKS depths <= it
    body = bytearray()
    body.append(flags)
    body += struct.pack('<H', depth)
    body += struct.pack('<H', cid)
    body += matrix
    if name:
        body += name.encode('utf-8') + b'\x00'
    if clip_depth is not None:
        body += struct.pack('<H', clip_depth)
    return build_tag(PLACE2, body)


def define_shape_rect(cid, w_px, h_px):
    """A minimal DefineShape: one solid fill, a w x h rectangle. Written out by hand because the
    masking path wants a genuine shape - a placed image character renders as artwork but is not
    something the engine can clip with."""
    w, h = int(round(w_px * 20)), int(round(h_px * 20))
    bits = []

    def put(v, n):
        for i in range(n - 1, -1, -1):
            bits.append((v >> i) & 1)

    def put_signed(v, n):
        put(v & ((1 << n) - 1), n)

    def need(*vals):
        return max(max(abs(v).bit_length() for v in vals) + 1, 2)

    # SHAPEWITHSTYLE: one solid white fill, no lines
    styles = bytearray()
    styles.append(1)            # FillStyleCount
    styles.append(0x00)         # solid
    styles += bytes((255, 255, 255))  # RGB (tag 2 = DefineShape, no alpha)
    styles.append(0)            # LineStyleCount
    styles.append(0x10)         # NumFillBits = 1, NumLineBits = 0

    put(0, 1)                   # StyleChangeRecord
    put(0, 1)                   # StateNewStyles
    put(0, 1)                   # StateLineStyle
    put(1, 1)                   # StateFillStyle1
    put(0, 1)                   # StateFillStyle0
    put(1, 1)                   # StateMoveTo
    nb = need(0, 0)
    put(nb, 5)
    put_signed(0, nb)
    put_signed(0, nb)
    put(1, 1)                   # FillStyle1 = index 1 (NumFillBits = 1)
    for dx, dy in ((w, 0), (0, h), (-w, 0), (0, -h)):
        put(1, 1)               # EdgeRecord
        put(1, 1)               # StraightEdge
        n = need(dx, dy)
        put(n - 2, 4)
        put(1, 1)               # GeneralLineFlag
        put_signed(dx, n)
        put_signed(dy, n)
    put(0, 6)                   # EndShapeRecord
    while len(bits) % 8:
        bits.append(0)
    recs = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for bit in bits[i:i + 8]:
            b = (b << 1) | bit
        recs.append(b)

    # ShapeBounds RECT (5-bit count + 4 signed fields)
    rb = []

    def rput(v, n):
        for i in range(n - 1, -1, -1):
            rb.append((v >> i) & 1)
    nbits = max(w.bit_length(), h.bit_length()) + 1
    rput(nbits, 5)
    for v in (0, w, 0, h):
        rput(v & ((1 << nbits) - 1), nbits)
    while len(rb) % 8:
        rb.append(0)
    rect = bytearray()
    for i in range(0, len(rb), 8):
        b = 0
        for bit in rb[i:i + 8]:
            b = (b << 1) | bit
        rect.append(b)
    return build_tag(2, struct.pack('<H', cid) + bytes(rect) + bytes(styles) + bytes(recs))


def remove2(depth):
    return build_tag(REMOVE2, struct.pack('<H', depth))


class _BitWriter:
    """Bit writer for rebuilding a CXFORMWITHALPHA in place (same bit length in, same out)."""

    def __init__(self):
        self.bits = []

    def u(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def s(self, v, n):
        self.u(v & ((1 << n) - 1), n)

    def bytes_out(self):
        b = list(self.bits)
        while len(b) % 8:
            b.append(0)
        out = bytearray()
        for i in range(0, len(b), 8):
            byte = 0
            for bit in b[i:i + 8]:
                byte = (byte << 1) | bit
            out.append(byte)
        return bytes(out)


class _BitReader:
    def __init__(self, buf, pos):
        self.buf, self.p, self.b = buf, pos, 0

    def bit(self):
        v = (self.buf[self.p] >> (7 - self.b)) & 1
        self.b += 1
        if self.b == 8:
            self.b = 0
            self.p += 1
        return v

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def s(self, n):
        if n == 0:
            return 0
        v = self.u(n)
        return v - (1 << n) if (v >> (n - 1)) else v

    def align(self):
        if self.b:
            self.b = 0
            self.p += 1


def patch_depth7_cxform(body, mul, add):
    """The row's depth-7 placements carry a FLAT-TINT colour transform (mul RGB = 0, add =
    120/130/110), which would turn our icon into a grey-green silhouette. Rewrite those
    transforms with the given mul/add. The rebuilt block uses the SAME nbits, so it occupies
    exactly as many bytes as before and nothing after it shifts."""
    import struct as _st
    out = bytearray(body)
    p = 4
    n = len(out)
    patched = 0
    while p + 2 <= n:
        rh = out[p] | (out[p + 1] << 8)
        t, ln, hdr = rh >> 6, rh & 0x3f, 2
        if ln == 0x3f:
            ln = _st.unpack('<I', out[p + 2:p + 6])[0]
            hdr = 6
        if t == 0:
            break
        if t in (PLACE2, PLACE3):
            q = p + hdr
            f0 = out[q]
            q += 1
            if t == PLACE3:
                q += 1
            depth = _st.unpack('<H', out[q:q + 2])[0]
            q += 2
            if f0 & 0x02:
                q += 2
            if depth == 7:
                if f0 & 0x04:                     # skip MATRIX
                    br = _BitReader(out, q)
                    if br.bit():
                        nb = br.u(5); br.s(nb); br.s(nb)
                    if br.bit():
                        nb = br.u(5); br.s(nb); br.s(nb)
                    nb = br.u(5); br.s(nb); br.s(nb); br.align()
                    q = br.p
                if f0 & 0x08:                     # CXFORMWITHALPHA
                    br = _BitReader(out, q)
                    ha, hm, nb = br.bit(), br.bit(), br.u(4)
                    if hm:
                        [br.s(nb) for _ in range(4 if ha else 3)]
                    if ha:
                        [br.s(nb) for _ in range(4 if ha else 3)]
                    br.align()
                    span = br.p - q
                    w = _BitWriter()
                    w.u(1, 1); w.u(1, 1); w.u(nb, 4)
                    for v in mul:
                        w.s(v, nb)
                    for v in add:
                        w.s(v, nb)
                    blk = w.bytes_out()
                    if len(blk) != span:
                        raise SystemExit(f'cxform rebuild changed size ({len(blk)} vs {span})')
                    out[q:q + span] = blk
                    patched += 1
        p += hdr + ln
    return bytes(out), patched


def main():
    import config  # noqa: F401  (tools/config.py - keeps path handling consistent)
    gm = u.spec_from_file_location('gmi', 'tools/generate_map_icons.py')
    icons_mod = u.module_from_spec(gm)
    gm.loader.exec_module(icons_mod)          # normalize() + lossless_body()
    import generate_overlay_icons as art      # icon_image(iconId) -> RGBA PNG art

    rm = u.spec_from_file_location('r', 'scratch/re_gfx_remap.py')
    M = u.module_from_spec(rm)
    rm.loader.exec_module(M)

    mv = M.Movie(GFX)
    if ROW_CID not in mv.defs:
        raise SystemExit(f'row clip cid {ROW_CID} not found in {GFX}')
    used = set(mv.defs.keys())
    base = max(used) + 1
    print(f'movie defs: {len(used)}, max cid {max(used)}, our ids start at {base}')

    # --- which icons do we need? one per ini key that has an atlas cell ---
    import icon_registry  # noqa: F401  (same registry the map icons come from)
    keys_icons = []       # (ini_key, iconId)
    hdr = open('src/generated_shared/goblin_overlay_icons.cpp', encoding='utf-8').read()
    import re as _re
    cells = dict()
    for m in _re.finditer(r'\{"([^"]+)",\s*(\d+),\s*(\d+)\}', hdr):
        cells[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    src = _re.search(r'CELL_SRC_ICON\[\] = \{([^}]*)\}', hdr).group(1)
    cell_src = [int(x) for x in src.split(',') if x.strip()]
    atlas_w = int(_re.search(r'ATLAS_W = (\d+)', hdr).group(1))
    cell_px = int(_re.search(r'CELL = (\d+)', hdr).group(1))
    per_row = atlas_w // cell_px
    for key, (col, row) in sorted(cells.items()):
        idx = row * per_row + col
        if 0 <= idx < len(cell_src):
            keys_icons.append((key, cell_src[idx]))
    print(f'keys with an icon: {len(keys_icons)}')

    # --- bitmaps ---
    blob = bytearray()
    frame_of_key = []
    bitmap_cids = []
    for i, (key, icon_id) in enumerate(keys_icons):
        img = art.icon_image(icon_id)
        if img is None:
            print(f'  WARN no art for iconId {icon_id} (key {key}) - skipped')
            continue
        img = icons_mod.normalize(img, ICON_PX)
        body = bytearray(icons_mod.lossless_body(img))
        cid = base + i
        body[0:2] = struct.pack('<H', cid)      # patch the charId placeholder
        blob += build_tag(LOSSLESS2, bytes(body))
        bitmap_cids.append((cid, img.size))
        frame_of_key.append((key, len(bitmap_cids)))  # strip cell; cell 0 stays empty

    # --- two candidate sprites, ONE frame each -----------------------------------------
    # Neither uses frames: nothing in a tag stream stops a timeline, so a multi-frame sprite
    # animates in every instance we do not reach (the right column, and the player's own
    # key-binding screen).
    #
    # A: all icons side by side inside a child clip "Strip", behind a real DefineShape mask.
    #    Picking an icon is a horizontal shift; cell 0 is empty, so an untouched instance shows
    #    nothing. The mask is a genuine shape this time - an image character renders as artwork
    #    but faulted the engine when asked to clip (0xC0000005 in Scaleform).
    # B: one named child per icon, all baked at scale 0 (invisible), and the chosen one switched
    #    to 100%. No new byte construction at all, which is the point: it cannot bring a new
    #    parser fault.
    mask_cid = base + len(bitmap_cids)
    strip_cid = mask_cid + 1
    sprite_a_cid = strip_cid + 1
    sprite_b_cid = sprite_a_cid + 1

    blob_a = bytearray(blob)
    blob_a += define_shape_rect(mask_cid, ICON_PX, ICON_PX)
    strip = bytearray()
    for i, (cid, size) in enumerate(bitmap_cids):
        scale = ICON_PX / float(max(size))
        strip += place2(cid, i + 1, matrix_bytes(scale, (i + 1) * ICON_PX, 0.0))
    strip += build_tag(SHOWFRAME, b'')
    strip += build_tag(END, b'')
    blob_a += build_tag(DEFSPRITE, struct.pack('<HH', strip_cid, 1) + bytes(strip))
    inner = bytearray()
    inner += place2(mask_cid, 1, matrix_bytes(1.0, 0.0, 0.0), clip_depth=2)
    inner += place2(strip_cid, 2, matrix_bytes(1.0, 0.0, 0.0), 'Strip')
    inner += build_tag(SHOWFRAME, b'')
    inner += build_tag(END, b'')
    blob_a += build_tag(DEFSPRITE, struct.pack('<HH', sprite_a_cid, 1) + bytes(inner))

    # Variant B, built ONLY from what has been measured to work. Exactly one thing ever
    # resolved by name: a single named child, one level down, in the row clip itself (the old
    # container at depth 6). Children inside it were unreachable (0 of 64), and 64 siblings were
    # unreachable too, on low and high depths alike. So there is one named child again - the
    # strip of all icons - and the mask that limits it to one cell is its SIBLING, not its
    # parent. Picking an icon stays a horizontal shift of that one child.
    mask_b_cid = base + len(bitmap_cids)
    strip_b_cid = mask_b_cid + 1
    blob_b = bytearray(blob)
    blob_b += define_shape_rect(mask_b_cid, ICON_PX, ICON_PX)
    # The strip carries the row offset ITSELF (ICON_X/ICON_Y folded into every icon), because the
    # placement of the strip must go in WITHOUT a matrix - see below.
    strip_b = bytearray()
    for i, (cid, size) in enumerate(bitmap_cids):
        scale = ICON_PX / float(max(size))
        # CENTRE each icon in its cell. The scale fits the longer side to the cell, so a tall narrow
        # icon came out only a few pixels wide and sat flush against the cell's left edge, leaving a
        # visible gap before the label while a round icon had none. Half the leftover on each side
        # makes every icon read as being in the same column.
        dx = (ICON_PX - size[0] * scale) / 2.0
        dy = (ICON_PX - size[1] * scale) / 2.0
        strip_b += place2(cid, i + 1,
                          matrix_bytes(scale, ICON_X + (i + 1) * ICON_PX + dx, ICON_Y + dy))
    strip_b += build_tag(SHOWFRAME, b'')
    strip_b += build_tag(END, b'')
    blob_b += build_tag(DEFSPRITE, struct.pack('<HH', strip_b_cid, 1) + bytes(strip_b))
    # Mask on 16 clips depth 17; the strip sits on 17. The row clip itself uses 1..15.
    #
    # NO MATRIX on the strip's placement, and that is the whole point. DisplayList::MoveDisplayObject
    # (GFx SDK, Src/GFx/GFx_DisplayList.cpp:363) re-applies the tag's matrix every time the timeline
    # places an object again, and a script-set transform only survives if the object rejects anim
    # moves - which the movie-wide continueAnimation flag undoes for it (GFx_DisplayObject.cpp:1175).
    # With a matrix in the tag, every row re-place snapped our strip back to cell 0 (the empty one),
    # which in game read as "the icons disappeared after toggling a row" and came back only when the
    # list was scrolled. `if (pos.HasMatrix())` is the escape: a placement with no matrix is never
    # reset, so the shift we set stays set. An untouched instance (the right column, the player's own
    # key-binding screen) sits at identity, and since the icons now start at ICON_X + ICON_PX, the
    # mask window shows the empty cell there exactly as before.
    # ── the slider strip ────────────────────────────────────────────────────────────
    # Its ids come AFTER both icon variants' ids (A and B share the same range - only one
    # is ever spliced - so the slider must clear the higher of the two, sprite_b_cid).
    slider_img_cid = sprite_b_cid + 1
    slider_mask_cid = sprite_b_cid + 2
    slider_strip_cid = sprite_b_cid + 3
    mv042 = M.Movie(GFX_042)
    if SLIDER_IMG_CID_042 not in mv042.defs:
        raise SystemExit(f'MENU_FL_Slider (cid {SLIDER_IMG_CID_042}) not found in {GFX_042}')
    t42, s42, e42 = mv042.defs[SLIDER_IMG_CID_042]
    if t42 != 1009:
        raise SystemExit(f'cid {SLIDER_IMG_CID_042} in 02_042 is tag {t42}, expected 1009 '
                         '(GFX_DefineExternalImage2)')
    # The def carries only {cid, format, size, resource name}; the engine loads the texture
    # by NAME from the global menu image sets, so the copied tag works in any movie.
    img_body = bytearray(mv042.d[s42:e42])
    img_body[0:2] = struct.pack('<H', slider_img_cid)
    slider_blob = build_tag(1009, bytes(img_body))
    win_w = (SL_WIN_R - SL_WIN_L) * SL_SCALE
    win_h = (SL_WIN_B - SL_WIN_T) * SL_SCALE
    slider_blob += define_shape_rect(slider_mask_cid, win_w, win_h)
    strip_s = bytearray()
    for k in range(1, SLIDER_CELLS + 1):
        f = (k - 1) / float(SLIDER_CELLS - 1)
        ix = (SLIDER_X + k * SLIDER_PITCH
              + (SL_IMG_X0 + (SL_IMG_X1 - SL_IMG_X0) * f - SL_WIN_L) * SL_SCALE)
        iy = SLIDER_Y + (SL_IMG_Y - SL_WIN_T) * SL_SCALE
        strip_s += place2(slider_img_cid, k, matrix_bytes(SL_SCALE, ix, iy))
    strip_s += build_tag(SHOWFRAME, b'')
    strip_s += build_tag(END, b'')
    slider_blob += build_tag(DEFSPRITE, struct.pack('<HH', slider_strip_cid, 1) + bytes(strip_s))
    blob_b += slider_blob

    # Mask pairs: icons on 16(clip 17)/17, slider on 18(clip 19)/19 - the row's own children
    # stay on 1..15. Same no-matrix rule for the strip placement as for MfgIcon (see above);
    # the row-level offset SLIDER_X/SLIDER_Y is baked into every cell.
    icon_places = (place2(mask_b_cid, 16, matrix_bytes(1.0, ICON_X, ICON_Y), clip_depth=17)
                   + place2(strip_b_cid, 17, b'', 'MfgIcon')
                   + place2(slider_mask_cid, 18, matrix_bytes(1.0, SLIDER_X, SLIDER_Y),
                            clip_depth=19)
                   + place2(slider_strip_cid, 19, b'', 'MfgSlider'))

    tt, b0, b1 = mv.defs[ROW_CID]
    if tt != DEFSPRITE:
        raise SystemExit(f'cid {ROW_CID} is tag {tt}, expected DefineSprite')
    body = bytes(mv.d[b0:b1])
    # Both variants insert their placements at the START of frame 1 (right after cid +
    # frameCount), where they persist across the row's style frames.
    row_tag_a = build_tag(DEFSPRITE,
                          body[:4]
                          + place2(sprite_a_cid, 6, matrix_bytes(1.0, ICON_X, ICON_Y), 'MfgIcon')
                          + body[4:])
    row_tag_b = build_tag(DEFSPRITE, body[:4] + bytes(icon_places) + body[4:])
    # --- SELF-CHECK: assemble exactly what the DLL will splice, for BOTH variants ---
    # Any icon build must survive a parse-back before it is emitted, and every original charId
    # reference inside the row clip must survive untouched (an early attempt repurposed a clip
    # other code resolves paths inside, and the game crashed).
    for label, blb, rtag, sprite in (('A', blob_a, row_tag_a, sprite_a_cid),
                                     ('B', blob_b, row_tag_b, strip_b_cid)):
        ext = bytearray(mv.d[:b0 - 6]) + bytearray(blb) + bytearray(rtag) + bytearray(mv.d[b1:])
        struct.pack_into('<I', ext, 4, len(ext))
        check_path = 'scratch/ext_02_160_%s.gfx' % label
        with open(check_path, 'wb') as f:
            f.write(ext)
        mv2 = M.Movie(check_path)
        if ROW_CID not in mv2.defs or sprite not in mv2.defs:
            raise SystemExit('SELF-CHECK %s: row clip or icon sprite missing' % label)
        t2, y0, y1 = mv2.defs[ROW_CID]
        orig_refs = sorted(o for pos, o in mv.id_positions.items() if b0 <= pos < b1)
        new_refs = sorted(o for pos, o in mv2.id_positions.items() if y0 <= pos < y1)
        missing = [c for c in orig_refs if c not in new_refs]
        if missing:
            raise SystemExit('SELF-CHECK %s: row clip lost char refs %s' % (label, missing))
        if label == 'B':
            for c, what in ((slider_img_cid, 'slider image'),
                            (slider_mask_cid, 'slider mask'),
                            (slider_strip_cid, 'slider strip')):
                if c not in mv2.defs:
                    raise SystemExit(f'SELF-CHECK B: {what} (cid {c}) missing after parse-back')
        print('self-check %s OK: %d defs, sprite %d, %d B blob + %d B row tag'
              % (label, len(mv2.defs), sprite, len(blb), len(rtag)))

    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#pragma once\n')
        f.write('// GENERATED by tools/generate_menu_icon_tags.py - do not edit.\n')
        f.write('// Category icons for the in-game menu rows: bitmaps + an icon sprite, plus a\n')
        f.write('// replacement DefineSprite tag for the row clip that places the sprite as "MfgIcon".\n')
        f.write('#include <cstdint>\n#include <cstddef>\n\n')
        f.write('namespace goblin::menu_icon_tags\n{\n')
        f.write(f'    constexpr uint16_t ROW_CID = {ROW_CID};\n')
        f.write('    // The runtime splice refuses to patch a movie these bytes were not built\n')
        f.write('    // against: the row tag body must be exactly this long, and every character\n')
        f.write('    // id we add must still be unused in the movie that actually loaded.\n')
        f.write(f'    constexpr size_t ORIG_ROW_BODY_LEN = {len(body)};\n')
        f.write(f'    constexpr uint16_t FIRST_CID = {base};\n')
        f.write(f'    constexpr uint16_t LAST_CID = {slider_strip_cid};\n')
        f.write(f'    constexpr int ICON_COUNT = {len(bitmap_cids)};\n')
        f.write('    // The slider strip: cell k (1-based) shows the native bar at fraction\n')
        f.write('    // (k-1)/(SLIDER_CELLS-1); cell 0 is empty. Show a value by shifting the\n')
        f.write('    // "MfgSlider" child to x = -cell * SLIDER_CELL_PITCH_PX (same mechanism\n')
        f.write('    // as the icon strip; the placement carries no matrix).\n')
        f.write(f'    constexpr int SLIDER_CELLS = {SLIDER_CELLS};\n')
        f.write(f'    constexpr int SLIDER_CELL_PITCH_PX = {SLIDER_PITCH};\n')
        # Only variant B is emitted since 2026-07-29: the strip-behind-a-mask variant (A) was
        # removed from the DLL, and its blob was ~160 KB of data nothing read. blob_a/row_tag_a are
        # still BUILT above so the layout arithmetic stays honest and reviving A stays a one-line
        # change here.
        for name, data in (('ICON_BLOB_B', blob_b), ('ROW_TAG_B', row_tag_b)):
            f.write(f'    constexpr size_t {name}_LEN = {len(data)};\n')
            f.write(f'    inline const unsigned char {name}[] = {{\n')
            for i in range(0, len(data), 20):
                f.write('        ' + ','.join(str(b) for b in data[i:i + 20]) + ',\n')
            f.write('    };\n')
        f.write(f'    constexpr int ICON_CELL_PX = {ICON_PX};\n')
        f.write('    // Variant B places the strip with NO matrix in the tag (so the timeline can\n')
        f.write('    // never reset it), and the row offset lives inside the strip - so the shift\n')
        f.write('    // to show a cell is (-cell * ICON_CELL_PX, 0) from the origin, not\n')
        f.write(f'    // (ICON_X - cell * ICON_CELL_PX, ICON_Y).\n')
        f.write('    // Which strip cell an ini key uses: shift the strip by -cell*ICON_CELL_PX\n')
        f.write('    // to show it. Cell 0 is empty, so cell 0 means "no icon".\n')
        f.write('    struct KeyFrame { const char *key; int frame; };\n')
        f.write('    inline const KeyFrame ICON_FRAME_OF_KEY[] = {\n')
        for key, frame in frame_of_key:
            f.write(f'        {{"{key}", {frame}}},\n')
        f.write('    };\n')
        f.write('}\n')
    print('wrote %s: variant A %d B, variant B %d B, %d icons'
          % (OUT, len(blob_a), len(blob_b), len(bitmap_cids)))


if __name__ == '__main__':
    main()
