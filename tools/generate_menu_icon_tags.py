"""Category icons for the IN-GAME MENU rows (movie 02_160_KeyConfiguration).

The menu row clip has no image child at all, and an image registered into another movie is not
reachable from this one - so the icons have to become part of THIS movie. They are handed to the
game's own parser as an EXTENDED copy of the movie bytes when the movie LOADS
(goblin_own_movie.cpp), which is the one route that does not disturb a live movie: the icon ends
up an ordinary timeline child that the engine creates and destroys itself, and the DLL only
resolves it by name and picks its frame. Creating display objects in a live movie was tried and
crashed three times over; see the note in goblin_stall_probe.cpp.

Nothing here reads a .gfx. Everything that depends on the movie that actually loads is decided by
the DLL at load time, from that movie's own tags:
  * the character ids - the DLL takes one past the highest id the loaded movie defines;
  * the row clip - the DLL inserts ROW_PLACES at the start of frame 1 of the row clip it finds.
What this generator emits (src/generated_shared/goblin_menu_icon_tags.hpp) is only our own content,
with LOCAL character ids (0 .. CID_COUNT-1) and the byte offsets of every id so the DLL can add its
base:
  ICON_BLOB[]    = N x DefineBitsLossless2 (one per category icon, from our PNG art)
                   + a DefineShape mask + ONE single-frame DefineSprite strip of all icons
                   + the SLIDER strip (see below)
  ROW_PLACES[]   = the four PlaceObject2 tags that go into the row clip: the icon mask, the strip
                   as the named child "MfgIcon", the slider mask, the slider strip as "MfgSlider"
  *_CID_RELOCS[] = offsets of the u16 local ids inside those two arrays
  ICON_FRAME_OF_KEY[] = ini key -> strip cell (1-based; cell 0 is empty)

The SLIDER strip is the native settings slider rebuilt for this movie. In the game's own
widget (02_042_PC_GraphicSetting, sprite 78 "Slider") the whole visual is ONE external
texture, MENU_FL_Slider (396x40, drawn 1:1), sliding to the right under a 400x36 mask -
sprite 76 gives it 1000 frames, one per position. A frame-driven copy is out (a multi-frame
sprite ANIMATES in every instance we do not drive - the game's own key-binding screen uses
this very row clip), so the same look is authored the way the icons are: a strip of
SLIDER_CELLS static cells, cell k = the texture at the position for fraction k/(cells-1),
behind one row-level mask window; showing a value is a horizontal shift of the strip, and
cell 0 is empty so untouched instances show nothing. The texture is referenced BY NAME
(GFX_DefineExternalImage2: the engine loads it from the global menu image sets), so its define
is a constant here. Geometry measured once from 02_042 (scratch/recon_042_slider_geom.py):
image x -591.45px (0%) .. -202px (100%), mask window x -196.5..203.5, y -36..0, image y -37.15.

Run by the shared stage of the build; standalone: py tools/generate_menu_icon_tags.py
"""
import importlib.util as u
import struct
import sys

sys.path.insert(0, 'tools')

import config  # noqa: F401  (tools/config.py - keeps path handling consistent)

OUT = 'src/generated_shared/goblin_menu_icon_tags.hpp'
ICON_PX = 32           # icons are drawn into a 40px-tall row
ICON_X = 14.0          # px. Was 2.0 - flush with the separator caption, which ate the indent
                       # ordinary rows have; 14 leaves that indent visible and still clears the label
ICON_Y = 11.0         # rows are 63.7 apart; 4 sat too high and 18 too low, measured in game

# ── the slider strip (native look, shift-driven) ──────────────────────────────────────
# MENU_FL_Slider's GFX_DefineExternalImage2 (tag 1009) as 02_042 defines it (its cid 28 there):
# u32 character id, u16 bitmap format 13, u16 width 396, u16 height 40, then two length-prefixed
# names - the export name and the texture file.
EXT_IMAGE = 1009
SLIDER_IMG_FORMAT, SLIDER_IMG_W, SLIDER_IMG_H = 13, 396, 40
SLIDER_IMG_NAME, SLIDER_IMG_FILE = b'MENU_FL_Slider', b'MENU_FL_Slider.tga'
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

# Depths our placements take inside the row clip: icon mask 16 (clips 17), icon strip 17, slider
# mask 18 (clips 19), slider strip 19. The row's own children use 1..15; the DLL refuses a row clip
# that already uses any of these.
ROW_DEPTH_LO, ROW_DEPTH_HI = 16, 19

PLACE2, SHOWFRAME, END, DEFSPRITE, LOSSLESS2 = 26, 1, 0, 39, 36


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


def external_image(cid):
    """GFX_DefineExternalImage2 for MENU_FL_Slider (see EXT_IMAGE above)."""
    body = struct.pack('<IHHH', cid, SLIDER_IMG_FORMAT, SLIDER_IMG_W, SLIDER_IMG_H)
    body += bytes([len(SLIDER_IMG_NAME)]) + SLIDER_IMG_NAME
    body += bytes([len(SLIDER_IMG_FILE)]) + SLIDER_IMG_FILE
    return build_tag(EXT_IMAGE, body)


def build(base, bitmaps):
    """ICON_BLOB and ROW_PLACES with every character id = base + its local index.

    Local ids: the bitmaps 0..N-1, then mask N, strip N+1, then N+2 and N+3 left unused (the ids the
    removed strip-in-a-sprite variant took - keeping the gap keeps every id, and the logo's one past
    the last, exactly where the shipped builds had them), then the slider image N+4, mask N+5 and
    strip N+6. CID_COUNT = N+7."""
    n = len(bitmaps)
    mask_cid, strip_cid = base + n, base + n + 1
    slider_img_cid, slider_mask_cid, slider_strip_cid = base + n + 4, base + n + 5, base + n + 6

    blob = bytearray()
    for i, (body, _size) in enumerate(bitmaps):
        b = bytearray(body)
        b[0:2] = struct.pack('<H', base + i)          # the lossless body leads with its charId
        blob += build_tag(LOSSLESS2, bytes(b))
    blob += define_shape_rect(mask_cid, ICON_PX, ICON_PX)
    # ONE named child - the strip of all icons - and the mask that limits it to one cell is its
    # SIBLING, not its parent: exactly one thing ever resolved by name, a single named child one
    # level down in the row clip itself (children inside a container were unreachable, 0 of 64, and
    # 64 siblings were unreachable too). Picking an icon is a horizontal shift of that one child.
    # The strip carries the row offset ITSELF (ICON_X/ICON_Y folded into every icon), because its
    # placement must go in WITHOUT a matrix - see ROW_PLACES below.
    strip = bytearray()
    for i, (_body, size) in enumerate(bitmaps):
        scale = ICON_PX / float(max(size))
        # CENTRE each icon in its cell. The scale fits the longer side to the cell, so a tall narrow
        # icon came out only a few pixels wide and sat flush against the cell's left edge, leaving a
        # visible gap before the label while a round icon had none. Half the leftover on each side
        # makes every icon read as being in the same column.
        dx = (ICON_PX - size[0] * scale) / 2.0
        dy = (ICON_PX - size[1] * scale) / 2.0
        strip += place2(base + i, i + 1,
                        matrix_bytes(scale, ICON_X + (i + 1) * ICON_PX + dx, ICON_Y + dy))
    strip += build_tag(SHOWFRAME, b'')
    strip += build_tag(END, b'')
    blob += build_tag(DEFSPRITE, struct.pack('<HH', strip_cid, 1) + bytes(strip))

    blob += external_image(slider_img_cid)
    win_w = (SL_WIN_R - SL_WIN_L) * SL_SCALE
    win_h = (SL_WIN_B - SL_WIN_T) * SL_SCALE
    blob += define_shape_rect(slider_mask_cid, win_w, win_h)
    strip_s = bytearray()
    for k in range(1, SLIDER_CELLS + 1):
        f = (k - 1) / float(SLIDER_CELLS - 1)
        ix = (SLIDER_X + k * SLIDER_PITCH
              + (SL_IMG_X0 + (SL_IMG_X1 - SL_IMG_X0) * f - SL_WIN_L) * SL_SCALE)
        iy = SLIDER_Y + (SL_IMG_Y - SL_WIN_T) * SL_SCALE
        strip_s += place2(slider_img_cid, k, matrix_bytes(SL_SCALE, ix, iy))
    strip_s += build_tag(SHOWFRAME, b'')
    strip_s += build_tag(END, b'')
    blob += build_tag(DEFSPRITE, struct.pack('<HH', slider_strip_cid, 1) + bytes(strip_s))

    # NO MATRIX on either strip's placement, and that is the whole point. DisplayList::
    # MoveDisplayObject (GFx SDK, Src/GFx/GFx_DisplayList.cpp:363) re-applies the tag's matrix every
    # time the timeline places an object again, and a script-set transform only survives if the object
    # rejects anim moves - which the movie-wide continueAnimation flag undoes for it
    # (GFx_DisplayObject.cpp:1175). With a matrix in the tag, every row re-place snapped our strip
    # back to cell 0 (the empty one), which in game read as "the icons disappeared after toggling a
    # row" and came back only when the list was scrolled. `if (pos.HasMatrix())` is the escape: a
    # placement with no matrix is never reset, so the shift we set stays set. An untouched instance
    # (the right column, the player's own key-binding screen) sits at identity, and since the icons
    # start at ICON_X + ICON_PX, the mask window shows the empty cell there.
    places = (place2(mask_cid, 16, matrix_bytes(1.0, ICON_X, ICON_Y), clip_depth=17)
              + place2(strip_cid, 17, b'', 'MfgIcon')
              + place2(slider_mask_cid, 18, matrix_bytes(1.0, SLIDER_X, SLIDER_Y), clip_depth=19)
              + place2(slider_strip_cid, 19, b'', 'MfgSlider'))
    return bytes(blob), bytes(places), n + 7


def relocations(at_a, at_b, delta, cid_count):
    """Offsets of every u16 character id in a buffer built at two bases `delta` apart. The ids are the
    only bytes that depend on the base, and with both bases' low bytes zero only an id's HIGH byte
    differs - so each differing byte is one id, whose low byte sits just before it."""
    if len(at_a) != len(at_b):
        raise SystemExit('relocation: the two builds differ in length - an id changed a tag size')
    relocs = []
    for p, (x, y) in enumerate(zip(at_a, at_b)):
        if x == y:
            continue
        if y - x != delta >> 8 or p == 0:
            raise SystemExit(f'relocation: byte {p} differs by {y - x}, not by one id base step')
        local = struct.unpack_from('<H', at_a, p - 1)[0] - BASE_A
        if not 0 <= local < cid_count:
            raise SystemExit(f'relocation: id at {p - 1} decodes to local {local} of {cid_count}')
        relocs.append(p - 1)
    return relocs


BASE_A, BASE_B = 0x1000, 0x2000   # two probe bases; low bytes zero, see relocations()


def main():
    gm = u.spec_from_file_location('gmi', 'tools/generate_map_icons.py')
    icons_mod = u.module_from_spec(gm)
    gm.loader.exec_module(icons_mod)          # normalize() + lossless_body()
    import generate_overlay_icons as art      # icon_image(iconId) -> RGBA PNG art

    # --- which icons do we need? one per ini key that has an atlas cell ---
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

    # --- bitmaps (charId placeholder patched per build) ---
    bitmaps = []          # (lossless body, (w, h))
    frame_of_key = []
    for key, icon_id in keys_icons:
        img = art.icon_image(icon_id)
        if img is None:
            print(f'  WARN no art for iconId {icon_id} (key {key}) - skipped')
            continue
        img = icons_mod.normalize(img, ICON_PX)
        bitmaps.append((icons_mod.lossless_body(img), img.size))
        frame_of_key.append((key, len(bitmaps)))  # strip cell; cell 0 stays empty

    blob_a, places_a, cid_count = build(BASE_A, bitmaps)
    blob_b, places_b, _ = build(BASE_B, bitmaps)
    blob_relocs = relocations(blob_a, blob_b, BASE_B - BASE_A, cid_count)
    place_relocs = relocations(places_a, places_b, BASE_B - BASE_A, cid_count)
    # Every id we wrote: N bitmap defines + N strip places, mask, strip, slider image, slider mask,
    # slider strip + SLIDER_CELLS slider places; and the four row placements.
    n = len(bitmaps)
    want_blob, want_places = 2 * n + 5 + SLIDER_CELLS, 4
    if len(blob_relocs) != want_blob or len(place_relocs) != want_places:
        raise SystemExit(f'relocation: found {len(blob_relocs)}+{len(place_relocs)} ids, '
                         f'expected {want_blob}+{want_places}')
    # Emit at local ids (base 0): the DLL adds the loaded movie's base at each relocation.
    blob, places = bytearray(blob_a), bytearray(places_a)
    for buf, relocs in ((blob, blob_relocs), (places, place_relocs)):
        for o in relocs:
            struct.pack_into('<H', buf, o, struct.unpack_from('<H', buf, o)[0] - BASE_A)
    print(f'{n} icons, {cid_count} local ids, blob {len(blob)} B ({len(blob_relocs)} ids), '
          f'row places {len(places)} B ({len(place_relocs)} ids)')

    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#pragma once\n')
        f.write('// GENERATED by tools/generate_menu_icon_tags.py - do not edit.\n')
        f.write('// Category icons and the value slider for the in-game menu rows, with LOCAL character\n')
        f.write('// ids: goblin_own_movie adds one past the loaded movie\'s highest id at every *_CID_RELOCS\n')
        f.write('// offset, and inserts ROW_PLACES at the start of frame 1 of the row clip it loaded.\n')
        f.write('#include <cstdint>\n#include <cstddef>\n\n')
        f.write('namespace goblin::menu_icon_tags\n{\n')
        f.write(f'    constexpr uint16_t CID_COUNT = {cid_count};  // local ids 0..CID_COUNT-1\n')
        f.write(f'    constexpr int ICON_COUNT = {n};\n')
        f.write('    // Depths ROW_PLACES takes inside the row clip (the row\'s own children use 1..15).\n')
        f.write(f'    constexpr uint16_t ROW_DEPTH_LO = {ROW_DEPTH_LO}, ROW_DEPTH_HI = {ROW_DEPTH_HI};\n')
        f.write('    // The slider strip: cell k (1-based) shows the native bar at fraction\n')
        f.write('    // (k-1)/(SLIDER_CELLS-1); cell 0 is empty. Show a value by shifting the\n')
        f.write('    // "MfgSlider" child to x = -cell * SLIDER_CELL_PITCH_PX (same mechanism\n')
        f.write('    // as the icon strip; the placement carries no matrix).\n')
        f.write(f'    constexpr int SLIDER_CELLS = {SLIDER_CELLS};\n')
        f.write(f'    constexpr int SLIDER_CELL_PITCH_PX = {SLIDER_PITCH};\n')
        for name, data in (('ICON_BLOB', blob), ('ROW_PLACES', places)):
            f.write(f'    constexpr size_t {name}_LEN = {len(data)};\n')
            f.write(f'    inline const unsigned char {name}[] = {{\n')
            for i in range(0, len(data), 20):
                f.write('        ' + ','.join(str(b) for b in data[i:i + 20]) + ',\n')
            f.write('    };\n')
        for name, relocs in (('ICON_BLOB_CID_RELOCS', blob_relocs),
                             ('ROW_PLACES_CID_RELOCS', place_relocs)):
            f.write(f'    inline const uint32_t {name}[] = {{\n')
            for i in range(0, len(relocs), 16):
                f.write('        ' + ','.join(str(o) for o in relocs[i:i + 16]) + ',\n')
            f.write('    };\n')
        f.write(f'    constexpr int ICON_CELL_PX = {ICON_PX};\n')
        f.write('    // The strip is placed with NO matrix in the tag (so the timeline can never\n')
        f.write('    // reset it), and the row offset lives inside the strip - so the shift to show\n')
        f.write('    // a cell is (-cell * ICON_CELL_PX, 0) from the origin.\n')
        f.write('    // Which strip cell an ini key uses: shift the strip by -cell*ICON_CELL_PX\n')
        f.write('    // to show it. Cell 0 is empty, so cell 0 means "no icon".\n')
        f.write('    struct KeyFrame { const char *key; int frame; };\n')
        f.write('    inline const KeyFrame ICON_FRAME_OF_KEY[] = {\n')
        for key, frame in frame_of_key:
            f.write(f'        {{"{key}", {frame}}},\n')
        f.write('    };\n')
        f.write('}\n')
    print(f'wrote {OUT}')


if __name__ == '__main__':
    main()
