#!/usr/bin/env python3
"""Generate World - Graces.rows from BonfireWarpParam."""

import json
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
import rowsink
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

from extract_all_items import load_paramdefs, read_param, param_to_dict
from marker_common import OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS, resolve_location_id_at
from unreachable import is_unreachable_grace

ERR_MOD_DIR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()

# Set of map tiles that actually exist as an MSB. Some BonfireWarpParam rows
# point at cut-content maps with no MSB (e.g. area 12 / grid 6 = "m12_06",
# which exists in neither ERR nor vanilla) - their graces are unreachable and
# their textId1 (e.g. 120600/120601) has no PlaceName entry, so a marker for
# them would be a nameless/null icon. Skip any grace whose tile has no MSB.
_MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
EXISTING_TILES = {p.name[:12] for p in _MSB_DIR.glob('m*.msb.dcx')}  # 'm12_02_00_00'

# PlaceName ids this profile's FMG actually carries. A grace whose BonfireWarpParam textId1 has
# no string (Throne ER: grace rid with textId1 60200000 in Stormveil) would be a text-less
# marker - the DLL's sanitizer clears the id and the engine draws no icon - so such a grace
# is labelled with its location instead, and skipped only when even that cannot be resolved.
def _placename_ids():
    p = config.DATA_DIR / 'PlaceName_engus.json'
    if not p.exists():
        return set()
    with open(p, encoding='utf-8') as f:
        return {int(k) for k in json.load(f)}
PLACENAME_IDS = _placename_ids()


def main():
    print("Loading BonfireWarpParam...")
    bwp = read_param(bnd, 'BonfireWarpParam', paramdefs)
    fields = {'areaNo', 'gridXNo', 'gridZNo', 'posX', 'posY', 'posZ',
              'eventflagId', 'textId1', 'dispMask00', 'dispMask01',
              'dispMask02', 'bonfireEntityId'}
    data = param_to_dict(bwp, fields)

    sink = rowsink.RowSink()
    # Row-ID base = this category's z-order slot (lower base -> drawn on top).
    # Single source: tools/row_id_registry.py (reorder LAYER_ORDER to relayer).
    row_id = __import__("row_id_registry").base("World - Graces")
    count = 0
    for rid, row in sorted(data.items()):
        area = row.get('areaNo', 0)
        if area == 0:
            continue
        flag = row.get('eventflagId', 0)
        tid1 = row.get('textId1', 0)
        if flag <= 0 or tid1 <= 0:
            continue

        # Skip graces in cut-content maps with no MSB (unreachable; their
        # textId1 has no PlaceName text → would be a nameless/null icon).
        gxc = row.get('gridXNo', 0)
        gzc = row.get('gridZNo', 0)
        tile = f'm{area:02d}_{gxc:02d}_{gzc:02d}_00'
        if tile not in EXISTING_TILES:
            print(f"  skip grace {rid}: tile {tile} has no MSB (textId1={tid1})")
            continue

        # Respect ERR's intentional hides: if ALL dispMaskXX = 0 in the
        # source BonfireWarpParam row, ERR has explicitly hidden this grace
        # (e.g. spoiler-hides for Inner Aeonia / Primeval Sorcerer Azur /
        # Fortified Manor 1F - soft-disabled with rebound bonfireEntityId
        # pointing to non-existent MSB asset). Don't override the hide.
        if (not row.get('dispMask00') and not row.get('dispMask01')
                and not row.get('dispMask02')):
            continue

        # Skip graces whose physical bonfire MSB asset ERR moved out of
        # reach (e.g. Midra's Library raised onto a ledge, Fissure Cross
        # dropped into terrain). Self-disarms if ERR moves it back.
        if is_unreachable_grace(row.get('areaNo', 0),
                                row.get('bonfireEntityId', 0)):
            continue

        gx = row.get('gridXNo', 0)
        gz = row.get('gridZNo', 0)

        if row.get('dispMask01'):
            disp = 'dispMask01'
        elif row.get('dispMask02'):
            disp = 'pad2_0'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        elif area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        else:
            disp = 'dispMask00'

        fields = {'iconId': __import__("icon_registry").iconid("graces"), disp: 1, 'areaNo': area}
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            fields['gridXNo'] = gx
            fields['gridZNo'] = gz
        x = round(float(row.get('posX', 0)), 3)
        y = round(float(row.get('posY', 0)), 3)
        z = round(float(row.get('posZ', 0)), 3)
        fields['posX'] = x
        if y != 0:
            fields['posY'] = y
        fields['posZ'] = z
        # Reeling Shack (custom ERR grace, rid=62354601): override the
        # default lit-flag with 1035469430. Event 923 in common.emevd sets
        # 1035469430 ON after Potent Dreambrew (SpEffect 502170) and never
        # resets it, so this hides the marker permanently once the player
        # progresses past it. Can't OR with 76253 (lit flag) because the
        # engine shows the icon as long as ANY text slot is visible, and
        # the two flags never fire simultaneously (Event 923 resets 76253
        # OFF in the same step it sets 1035469430 ON). Trade-off: between
        # lighting and Dreambrew, our marker overlaps with vanilla icon -
        # small cosmetic price for correct post-Dreambrew hiding.
        disable_flag = 1035469430 if rid == 62354601 else flag
        if PLACENAME_IDS and tid1 not in PLACENAME_IDS:
            loc = resolve_location_id_at(tile, float(row.get('posX', 0.0)), float(row.get('posY', 0.0)),
                                         float(row.get('posZ', 0.0)))
            if loc <= 0:
                # Overworld tile with no interior location: the BloodMsg vocabulary word
                # "grace" (32032, +950M encoding), which every language has.
                print(f"  grace {rid}: textId1={tid1} has no PlaceName string and no location at {tile}, labelled 'grace'")
                tid1 = 950000000 + 32032
            else:
                print(f"  grace {rid}: textId1={tid1} has no PlaceName string, labelled with location {loc}")
                tid1 = loc
        fields['textId1'] = tid1
        fields['textDisableFlagId1'] = disable_flag
        fields['selectMinZoomStep'] = 2
        sink.add(row_id, fields)
        row_id += 1
        count += 1

    out_path = sink.write(OUT_DIR / 'World - Graces.rows')
    print(f"Written {count} graces to {out_path.name}")


if __name__ == '__main__':
    main()
