#!/usr/bin/env python3
"""Generate World - Imp Statues.rows from MSB assets with seal entity IDs (suffix 570/575/565/611)."""

import sys
import io
import os
import tempfile

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
import rowsink
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats


def _safe_unlink(path):
    try:
        os.unlink(path)
    except PermissionError:
        pass


from marker_common import (OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS,
                             resolve_location_id, resolve_location_id_at)
from unreachable import is_unreachable_in_err

ERR_MOD_DIR = config.require_err_mod_dir()
_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def rfb(rm, data, suf='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_tmp' + suf)
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    r = rm.Invoke(None, Array[Object]([tmp]))
    _safe_unlink(tmp)
    return r


# Seal types by entity ID suffix
SEAL_SUFFIXES = {570, 575, 565, 611}

# Actual imp statue seal models (the stone imp face you use keys on).
# AEG027_078/079 = the Stonesword-Key imp seals. AEG099_295 = the Four Belfries
# "gargoyle statues" (3 in Liurnia + 1 in the DLC), opened with an IMBUED Sword Key
# (Goods 8186) to warp - confirmed in m60_33_46 ev1033462611 (DisplayGenericDialog
# 108186 + RemoveItemFromPlayer Goods 8186). Their entity suffix is 611 and the
# used-flag is tile_base+611 (= the same activation_flag this script already computes).
SEAL_MODELS = {'AEG027_078', 'AEG027_079', 'AEG099_295'}

# Models opened with an Imbued Sword Key rather than a Stonesword Key.
IMBUED_KEY_MODELS = {'AEG099_295'}


def main():
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'

    print("Scanning MSBs for Imp Statue seals...")
    seals = []
    seen = set()

    for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
        map_name = msb_path.name.replace('.msb.dcx', '')
        parts = map_name.split('_')
        if len(parts) < 4:
            continue
        area = int(parts[0][1:])
        gx = int(parts[1])
        gz = int(parts[2])
        try:
            msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except:
            continue

        for p in msb.Parts.Assets:
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue  # disabled placement - engine doesn't spawn it
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else 0
            if eid <= 0:
                continue
            model = str(p.ModelName)
            if model not in SEAL_MODELS:
                continue
            suffix = eid % 1000
            if suffix not in SEAL_SUFFIXES:
                continue

            x = round(float(p.Position.X), 3)
            y = round(float(p.Position.Y), 3)
            z = round(float(p.Position.Z), 3)
            # Skip seals that ERR moved DOWN below vanilla into unreachable
            # terrain. Conditional on actual vs vanilla Y - self-disarms if
            # a future ERR update fixes the position.
            if is_unreachable_in_err(map_name, str(p.Name), y):
                continue
            key = (area, eid)
            if key in seen:
                continue
            seen.add(key)
            # Activation flag = tile_base + seal suffix (NOT entity ID)
            if area in (60, 61):
                prefix = 10 if area == 60 else 20
                tile_base = prefix * 100000000 + gx * 1000000 + gz * 10000
            else:
                tile_base = area * 1000000 + gx * 10000
            activation_flag = tile_base + suffix

            seals.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': x, 'y': y, 'z': z,
                'eid': eid, 'suffix': suffix, 'model': model,
                'flag': activation_flag,
            })

    seals.sort(key=lambda s: (s['area'], s['gx'], s['gz']))
    print(f"  {len(seals)} unique imp statue seals")

    # Deduplicate by position (different assets at same seal location)
    deduped = []
    seen_pos = set()
    for s in seals:
        key = (s['area'], round(s['x'], 0), round(s['z'], 0))
        if key in seen_pos:
            continue
        seen_pos.add(key)
        deduped.append(s)

    print(f"  {len(deduped)} after position dedup")

    # Generate rows
    sink = rowsink.RowSink()
    row_id = __import__("row_id_registry").base("World - Imp Statues")  # z-order slot; see row_id_registry
    for s in deduped:
        area = s['area']
        gx = s['gx']
        gz = s['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        fields = {'iconId': __import__("icon_registry").iconid("imp_statues"), disp: 1, 'areaNo': area}
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            fields['gridXNo'] = gx
            fields['gridZNo'] = gz
        fields['posX'] = s['x']
        if s['y'] != 0:
            fields['posY'] = s['y']
        fields['posZ'] = s['z']
        # Key type as first line: Stonesword Key or Imbued Sword Key
        imbued = s['suffix'] == 565 or s['model'] in IMBUED_KEY_MODELS
        if imbued:
            fields['textId1'] = 500008186  # Imbued Sword Key
        else:
            fields['textId1'] = 500008000  # Stonesword Key
        # Seal unlock flag = entity ID
        fields['textDisableFlagId1'] = s['flag']
        # Location name for dungeons (second line) - nearest-grace lookup
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id_at(map_code, s["x"], s.get("y", 0.0), s["z"])
        if loc_id > 0:
            fields['textId2'] = loc_id
            fields['textDisableFlagId2'] = s['flag']
        fields['selectMinZoomStep'] = 1
        sink.add(row_id, fields)
        row_id += 1

    out_path = sink.write(OUT_DIR / 'World - Imp Statues.rows')
    print(f"Written {len(deduped)} entries to {out_path.name}")


if __name__ == '__main__':
    main()
