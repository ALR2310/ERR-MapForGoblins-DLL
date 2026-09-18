#!/usr/bin/env python3
"""Generate World - Paintings.rows from EMEVD painting template events + MSB positions."""

import sys
import io
import os
import tempfile
import struct

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

ERR_MOD_DIR = config.require_err_mod_dir()
_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
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

    # Step 1: Find painting events in EMEVD (flags 580000-580199)
    # Every painting is initialised by TWO events, in two different maps:
    #   PICKUP - where the painting itself lies. Common template 90005632, or a per-map DLC
    #            template (2046402550, 2047422550, 21002600): args [0, tmpl, flag, entity, textId]
    #            with textId = flag - 500000 (the painting's own text, 80000..80120).
    #   REWARD - the painted vista, where the reward appears once the painting is owned.
    #            Common template 90005633 or a per-map DLC one (2045432550 ...): args
    #            [0, tmpl, rewardFlag 5803xx/5804xx, flag, entity (the reward's enemy), ...].
    # The marker names the painting and hides when it is picked up, so it belongs at the PICKUP.
    # (Taking the first event in file order put 6 of vanilla's 10 on the vista instead - all three
    # DLC paintings among them - where it showed before the painting was found and vanished on
    # pickup.) The reward event is only a fallback for a painting with no pickup event.
    print("Scanning EMEVD for painting events...")
    pickups = {}
    rewards = {}

    for emevd_path in sorted((ERR_MOD_DIR / 'event').glob('*.emevd.dcx')):
        fname = emevd_path.name.replace('.emevd.dcx', '')
        try:
            emevd = rfb(emevd_read, SoulsFormats.DCX.Decompress(str(emevd_path)), '.emevd')
        except:
            continue
        for ev in emevd.Events:
            if int(ev.ID) != 0:
                continue
            for instr in ev.Instructions:
                bank = int(instr.Bank)
                if bank != 2000:
                    continue
                raw = bytes(instr.ArgData)
                ints = []
                for off in range(0, len(raw) - 3, 4):
                    ints.append(struct.unpack_from('<i', raw, off)[0])
                if len(ints) < 5:
                    continue
                if 580000 <= ints[2] <= 580199 and ints[4] == ints[2] - 500000 and ints[3] > 0:
                    pickups.setdefault(ints[2], {'flag': ints[2], 'entity_id': ints[3], 'map_file': fname})
                elif 580200 <= ints[2] <= 580999 and 580000 <= ints[3] <= 580199 and ints[4] > 0:
                    rewards.setdefault(ints[3], {'flag': ints[3], 'entity_id': ints[4], 'map_file': fname})

    paintings = list(pickups.values())
    for flag, r in rewards.items():
        if flag not in pickups:
            print(f"  WARNING: painting flag {flag} has no pickup event - placed at its reward ({r['map_file']})")
            paintings.append(r)
    print(f"  {len(paintings)} paintings ({len(pickups)} at their pickup, "
          f"{len(paintings) - len(pickups)} at the reward only)")

    # Step 2: Build MSB entity index for positions
    print("Building MSB entity index...")
    entity_pos = {}
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
        for ptype in ['Assets', 'DummyAssets', 'Enemies', 'DummyEnemies']:
            parts = getattr(msb.Parts, ptype, None)
            if not parts:
                continue
            for p in parts:
                eid = int(p.EntityID) if hasattr(p, 'EntityID') else 0
                if eid > 0 and eid not in entity_pos:
                    entity_pos[eid] = (
                        area, gx, gz,
                        round(float(p.Position.X), 3),
                        round(float(p.Position.Y), 3),
                        round(float(p.Position.Z), 3),
                    )

    # Step 3: Resolve positions and generate rows
    sink = rowsink.RowSink()
    row_id = __import__("row_id_registry").base("World - Paintings")  # z-order slot; see row_id_registry
    count = 0
    for p in sorted(paintings, key=lambda p: p['flag']):
        eid = p['entity_id']
        if eid not in entity_pos:
            print(f"  WARNING: entity {eid} not found for flag {p['flag']}")
            continue

        area, gx, gz, x, y, z = entity_pos[eid]

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        fields = {'iconId': __import__("icon_registry").iconid("paintings"), disp: 1, 'areaNo': area}
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            fields['gridXNo'] = gx
            fields['gridZNo'] = gz
        fields['posX'] = x
        if y != 0:
            fields['posY'] = y
        fields['posZ'] = z
        # Painting name from GoodsName FMG
        flag = p['flag']
        if flag >= 580100:
            goods_id = 2008200 + (flag - 580100) // 10
        else:
            goods_id = 8200 + (flag - 580000) // 10
        fields['textId1'] = 500000000 + goods_id
        fields['textDisableFlagId1'] = p['flag']
        # Location text for dungeons - nearest-grace lookup
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id_at(map_code, x, y, z)
        if loc_id > 0:
            fields['textId2'] = loc_id
            fields['textDisableFlagId2'] = p['flag']
        fields['selectMinZoomStep'] = 1
        sink.add(row_id, fields)
        row_id += 1
        count += 1

    out_path = sink.write(OUT_DIR / 'World - Paintings.rows')
    print(f"Written {count} entries to {out_path.name}")


if __name__ == '__main__':
    main()
