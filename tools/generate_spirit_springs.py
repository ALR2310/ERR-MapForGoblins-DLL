#!/usr/bin/env python3
"""Generate World - Spirit Springs.rows and World - Spiritspring Hawks.rows
from MSB MountJumps/LockedMountJumps regions and c4210 hawk enemies."""

import sys
import io
import os
import tempfile

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
import icon_registry
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


def write_rows(entries, out_name, icon_id, text_id, start_row, zoom=1):
    """Write a row file from a list of entry dicts."""
    sink = rowsink.RowSink()
    row_id = start_row
    for e in entries:
        area = e['area']
        gx = e['gx']
        gz = e['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        fields = {'iconId': icon_id, disp: 1, 'areaNo': area}
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            fields['gridXNo'] = gx
            fields['gridZNo'] = gz
        fields['posX'] = e['x']
        if e['y'] != 0:
            fields['posY'] = e['y']
        fields['posZ'] = e['z']
        fields['textId1'] = text_id
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id_at(map_code, e["x"], e.get("y", 0.0), e["z"])
        if loc_id > 0:
            fields['textId2'] = loc_id
        flag = e.get('flag', 0)
        if flag > 0:
            fields['clearedEventFlagId'] = flag
        fields['selectMinZoomStep'] = zoom
        sink.add(row_id, fields)
        row_id += 1

    sink.write(OUT_DIR / out_name)
    return len(entries)


def main():
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'

    springs = []
    hawks = []
    seen_springs = set()
    seen_hawks = set()

    print("Scanning MSBs...")
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

        # Spirit Springs: launch points only (MountJumps / LockedMountJumps).
        # MountJumpFalls / LockedMountJumpFalls are LANDING points - they
        # mirror every launch and produce duplicate icons (every launch has
        # a matching fall within 50u; verified 0 orphans across all maps).
        # We only want one icon per spring at the interaction point.
        for rtype in ['MountJumps', 'LockedMountJumps']:
            coll = getattr(msb.Regions, rtype, None)
            if not coll:
                continue
            for r in coll:
                x = round(float(r.Position.X), 3)
                y = round(float(r.Position.Y), 3)
                z = round(float(r.Position.Z), 3)
                # Skip mount jumps that ERR moved DOWN below vanilla into
                # unreachable terrain (no jump triggers in-game).
                if is_unreachable_in_err(map_name, str(r.Name), y):
                    continue
                key = (area, round(x, 0), round(z, 0))
                if key in seen_springs:
                    continue
                seen_springs.add(key)
                springs.append({
                    'area': area, 'gx': gx, 'gz': gz,
                    'x': x, 'y': y, 'z': z,
                })

        # ERR FakeSpiritSpring in Others regions
        for r in msb.Regions.Others:
            rname = str(r.Name) if hasattr(r, 'Name') else ''
            if 'FakeSpiritSpringJump' not in rname:
                continue
            x = round(float(r.Position.X), 3)
            y = round(float(r.Position.Y), 3)
            z = round(float(r.Position.Z), 3)
            key = (area, round(x, 0), round(z, 0))
            if key in seen_springs:
                continue
            seen_springs.add(key)
            springs.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': x, 'y': y, 'z': z,
            })

        # DLC AEG463_200 assets (spirit springs without MountJumps region)
        for p in msb.Parts.Assets:
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
            if str(p.ModelName) != 'AEG463_200':
                continue
            x = round(float(p.Position.X), 3)
            y = round(float(p.Position.Y), 3)
            z = round(float(p.Position.Z), 3)
            key = (area, round(x, 0), round(z, 0))
            if key in seen_springs:
                continue
            seen_springs.add(key)
            springs.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': x, 'y': y, 'z': z,
            })

        # Spiritspring Hawks: c4210 enemies with EntityID ending in 0980 or 0971
        for p in msb.Parts.Enemies:
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
            eid = int(p.EntityID)
            model = str(p.ModelName)
            if eid <= 0 or model != 'c4210':
                continue
            suffix = eid % 10000
            if suffix not in (980, 971):
                continue
            key = (area, eid)
            if key in seen_hawks:
                continue
            seen_hawks.add(key)
            hawks.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': round(float(p.Position.X), 3),
                'y': round(float(p.Position.Y), 3),
                'z': round(float(p.Position.Z), 3),
                'flag': eid,  # EntityID = kill flag = spring unlock flag
            })

    springs.sort(key=lambda s: (s['area'], s['gx'], s['gz']))
    hawks.sort(key=lambda h: (h['area'], h['gx'], h['gz']))

    print(f"  {len(springs)} spirit springs, {len(hawks)} spiritspring hawks")

    # Write Spirit Springs (icon 404 = MENU_MAP_Range, tutorialId 301620 = "Spiritspring Jumping")
    n = write_rows(springs, 'World - Spirit Springs.rows',
                       icon_id=icon_registry.iconid("spirit_springs"), text_id=900301620,
                       start_row=__import__("row_id_registry").base("World - Spirit Springs"))
    print(f"Written {n} springs to World - Spirit Springs.rows")

    # Write Hawks (icon 439 - custom image MENU_ItemIcon_03273; textId
    # 904210304 = "167. Spiritspring Stormhawk")
    # clearedEventFlagId = hawk EntityID (set when hawk killed = spring unlocked)
    n = write_rows(hawks, 'World - Spiritspring Hawks.rows',
                       icon_id=icon_registry.iconid("spiritspring_hawks"), text_id=904210304,
                       start_row=__import__("row_id_registry").base("World - Spiritspring Hawks"))
    print(f"Written {n} hawks to World - Spiritspring Hawks.rows")


if __name__ == '__main__':
    main()
