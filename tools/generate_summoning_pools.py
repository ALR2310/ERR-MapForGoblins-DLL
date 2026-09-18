#!/usr/bin/env python3
"""Generate World - Summoning Pools.rows from SignPuddleParam + MSB position matching."""

import sys
import io
import os
import tempfile
import math

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


from extract_all_items import load_paramdefs, read_param, param_to_dict
from marker_common import (OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS,
                             resolve_location_id, resolve_location_id_at)

ERR_MOD_DIR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()

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
    # Step 1: Read SignPuddleParam for positions and flags
    print("Loading SignPuddleParam...")
    sp = read_param(bnd, 'SignPuddleParam', paramdefs)
    sp_fields = {'unknown_0x24', 'unknown_0x28',
                 'unknown_0x2c', 'unknown_0x30', 'unknown_0x34',
                 'unknown_0x3c'}
    sp_data = param_to_dict(sp, sp_fields)

    pools = []
    for rid, row in sorted(sp_data.items()):
        if rid == 0:
            continue
        flag = int(row.get('unknown_0x3c', 0))
        map_ref = int(row.get('unknown_0x28', 0))
        x = round(float(row.get('unknown_0x2c', 0)), 3)
        y = round(float(row.get('unknown_0x30', 0)), 3)
        z = round(float(row.get('unknown_0x34', 0)), 3)

        # Decode dungeon map_ref: area + block*256
        if map_ref < 100000:
            area = map_ref % 256
            block = map_ref // 256
        else:
            area = 0  # overworld - will resolve via MSB
            block = 0

        pools.append({
            'rid': rid, 'flag': flag, 'map_ref': map_ref,
            'x': x, 'y': y, 'z': z,
            'area': area, 'block': block,
            'gx': 0, 'gz': 0, 'resolved': area > 0,
        })

    print(f"  {len(pools)} pools from SignPuddleParam")

    # Step 2: Scan MSB for AEG099_015 to resolve overworld tiles
    print("Scanning MSBs for AEG099_015 positions...")
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
    msb_pools = []  # (area, gx, gz, x, y, z, map_name)
    seen_pos = set()

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
            if str(p.ModelName) != 'AEG099_015':
                continue
            mx = round(float(p.Position.X), 3)
            mz = round(float(p.Position.Z), 3)
            key = (area, round(mx, 0), round(mz, 0))
            if key in seen_pos:
                continue
            seen_pos.add(key)
            msb_pools.append((area, gx, gz, mx, float(p.Position.Y), mz, map_name))

    print(f"  {len(msb_pools)} unique MSB positions")

    # Step 3: Match unresolved pools (overworld) to MSB by position.
    #
    # IN 3D, INCLUDING Y. Both coordinate sets are TILE-LOCAL, so local X/Z repeat from tile to
    # tile and an X/Z-only distance can hand a pool to the wrong map by a fraction of a unit while
    # missing the height by a kilometre. That is not hypothetical: SignPuddleParam row 670800 (the
    # DLC pool in m61_46_41 at local y=329.7) beat m60_53_57's own pool by 0.257u in X/Z and was
    # planted in the Mountaintops, 1292.564u below the real one - so that tile showed two pools,
    # the phantom being the visible one, and the DLC tile showed none. Y separates them cleanly:
    # the true match is exact (dY=0.000 for m60_53_57's own pool, dY=0.233 for the DLC one).
    # Measured over the vanilla data: 3D changes the answer for that one pool and agrees with the
    # old metric for the other 64.
    unresolved = [p for p in pools if not p['resolved']]
    resolved_count = 0
    for pool in unresolved:
        best_dist = 999
        best_match = None
        for area, gx, gz, mx, my, mz, map_name in msb_pools:
            dx = pool['x'] - mx
            dy = pool['y'] - my
            dz = pool['z'] - mz
            dist = math.sqrt(dx*dx + dy*dy + dz*dz)
            if dist < best_dist:
                best_dist = dist
                best_match = (area, gx, gz)
        if best_match and best_dist < 10:
            pool['area'] = best_match[0]
            pool['gx'] = best_match[1]
            pool['gz'] = best_match[2]
            pool['resolved'] = True
            resolved_count += 1

    # For dungeon pools, set gx from block
    for pool in pools:
        if pool['area'] > 0 and pool['area'] not in (60, 61) and pool['block'] > 0:
            pool['gx'] = pool['block']

    still_unresolved = [p for p in pools if not p['resolved']]
    print(f"  Resolved {resolved_count} overworld pools via MSB, {len(still_unresolved)} unresolved")
    for p in still_unresolved:
        print(f"    row={p['rid']} map_ref={p['map_ref']} pos=({p['x']},{p['z']})")

    # Step 4: Deduplicate pools at near-identical positions.
    # SignPuddleParam has separate rows per matchAreaId (e.g. 670105 + 670410
    # for the same physical pool in different game contexts). Keep the first.
    resolved = [p for p in pools if p['resolved']]
    deduped = []
    for pool in resolved:
        is_dupe = any(
            pool['area'] == q['area'] and pool['gx'] == q['gx'] and pool['gz'] == q['gz']
            and abs(pool['x'] - q['x']) < 3 and abs(pool['z'] - q['z']) < 3
            for q in deduped
        )
        if is_dupe: continue
        deduped.append(pool)
    dropped = len(resolved) - len(deduped)
    if dropped:
        print(f"  Deduplicated {dropped} overlapping pool(s)")

    # Step 5: Generate rows
    sink = rowsink.RowSink()
    row_id = __import__("row_id_registry").base("World - Summoning Pools")  # z-order slot; see row_id_registry
    count = 0
    for pool in deduped:
        area = pool['area']
        gx = pool['gx']
        gz = pool['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        fields = {'iconId': __import__("icon_registry").iconid("summoning_pools"), disp: 1, 'areaNo': area}
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            fields['gridXNo'] = gx
            fields['gridZNo'] = gz
        fields['posX'] = pool['x']
        if pool['y'] != 0:
            fields['posY'] = pool['y']
        fields['posZ'] = pool['z']
        # Use SignPuddleParam row ID (670XXX) as activation flag - EMEVD sets these
        # textDisableFlagId1 hides text when pool activated → engine hides icon without text
        if pool['rid'] > 0:
            fields['textDisableFlagId1'] = pool['rid']
        fields['textId1'] = 900301690
        map_code = f'm{pool["area"]:02d}_{pool["gx"]:02d}_{pool["gz"]:02d}_00'
        loc_id = resolve_location_id_at(map_code, pool["x"], pool.get("y", 0.0), pool["z"])
        if loc_id > 0:
            fields['textId2'] = loc_id
            if pool['rid'] > 0:
                fields['textDisableFlagId2'] = pool['rid']
        fields['selectMinZoomStep'] = 1
        sink.add(row_id, fields)
        row_id += 1
        count += 1

    out_path = sink.write(OUT_DIR / 'World - Summoning Pools.rows')
    print(f"Written {count} entries to {out_path.name}")


if __name__ == '__main__':
    main()
