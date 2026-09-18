#!/usr/bin/env python3
"""Generate World - Stakes of Marika.rows from MSB AEG099_060 assets."""

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
                             resolve_location_id, resolve_location_id_at, convert_legacy_coords,
                             is_dlc_plane, remap_placeholder_xz)

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


def main():
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'

    print("Scanning MSBs for Stakes of Marika (AEG099_060)...")
    stakes = []
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
                continue  # disabled placement (e.g. preview/cView MSB rooms)
            if str(p.ModelName) != 'AEG099_060':
                continue
            x = round(float(p.Position.X), 3)
            y = round(float(p.Position.Y), 3)
            z = round(float(p.Position.Z), 3)
            # More than half of the stakes (vanilla: 244 of 453 placements) are stored in a
            # supertile MSB and named after their fine owner tile; without the remap they baked
            # the supertile's grid and local coords and landed far from the stake.
            sgx, sgz = gx, gz
            moved = remap_placeholder_xz(str(p.Name), map_name, x, z)
            if moved:
                sgx, sgz, x, z = moved
            # Legacy maps: one key per area, so the variants of one map (m11_00 / m11_05,
            # m21_00 / m21_02) share their stakes. Overworld: every tile has its own local frame.
            key = ((area, sgx, sgz, round(x, 0), round(z, 0)) if area in OVERWORLD_AREAS
                   else (area, round(x, 0), round(z, 0)))
            if key in seen:
                continue
            seen.add(key)
            stakes.append({
                'area': area, 'gx': sgx, 'gz': sgz,
                'x': x, 'y': y, 'z': z,
            })

    stakes.sort(key=lambda s: (s['area'], s['gx'], s['gz']))
    print(f"  {len(stakes)} unique stakes")

    # Generate rows
    sink = rowsink.RowSink()
    row_id = __import__("row_id_registry").base("World - Stakes of Marika")  # z-order slot; see row_id_registry
    for s in stakes:
        area = s['area']
        gx = s['gx']
        gz = s['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif is_dlc_plane(area, gx):
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        fields = {'iconId': __import__("icon_registry").iconid("stakes_of_marika"), disp: 1, 'areaNo': area}
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            fields['gridXNo'] = gx
            fields['gridZNo'] = gz
        fields['posX'] = s['x']
        if s['y'] != 0:
            fields['posY'] = s['y']
        fields['posZ'] = s['z']
        # Tutorial text 301540 = "Stakes of Marika"
        fields['textId1'] = 900301540
        # Location name for dungeons - nearest-grace lookup
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id_at(map_code, s["x"], s.get("y", 0.0), s["z"])
        if loc_id > 0:
            fields['textId2'] = loc_id
        fields['selectMinZoomStep'] = 1
        sink.add(row_id, fields)
        row_id += 1

    out_path = sink.write(OUT_DIR / 'World - Stakes of Marika.rows')
    print(f"Written {len(stakes)} entries to {out_path.name}")


if __name__ == '__main__':
    main()
