#!/usr/bin/env python3
"""Generate World - Maps.rows from items_database.json Map: items."""

import json
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

from marker_common import (DATA_DIR, OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS,
                             resolve_location_id, resolve_location_id_at)
import rowsink


def main():
    with open(DATA_DIR / 'items_database.json', encoding='utf-8') as f:
        db = json.load(f)

    print("Extracting Map items...")
    maps = []
    seen_flags = set()
    for item in db:
        flag = item.get('eventFlag', 0)
        if flag <= 0 or flag in seen_flags:
            continue
        for it in item.get('items', []):
            name = it.get('name', '')
            if not name.startswith('Map:'):
                continue
            seen_flags.add(flag)
            maps.append({
                'name': name,
                'goods_id': it.get('id', 0),
                'flag': flag,
                'area': item.get('areaNo', 0),
                'gx': item.get('gridX', 0),
                'gz': item.get('gridZ', 0),
                'x': round(item.get('x', 0), 3),
                'y': round(item.get('y', 0), 3),
                'z': round(item.get('z', 0), 3),
                'map': item.get('map', ''),
            })
            break

    maps.sort(key=lambda m: m['goods_id'])
    print(f"  {len(maps)} unique maps")

    sink = rowsink.RowSink()
    row_id = __import__("row_id_registry").base("World - Maps")  # z-order slot; see row_id_registry
    for m in maps:
        area = m['area']
        gx = m['gx']
        gz = m['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        fields = {'iconId': __import__("icon_registry").iconid("world_maps"), disp: 1, 'areaNo': area}
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            fields['gridXNo'] = gx
            fields['gridZNo'] = gz
        fields['posX'] = m['x']
        if m['y'] != 0:
            fields['posY'] = m['y']
        fields['posZ'] = m['z']
        # Map name from GoodsName
        fields['textId1'] = 500000000 + m['goods_id']
        # Hide when collected
        fields['textDisableFlagId1'] = m['flag']
        # Location for dungeons - nearest-grace lookup
        loc_id = resolve_location_id_at(m['map'], m.get('x', 0.0), m.get('y', 0.0), m.get('z', 0.0))
        if loc_id > 0:
            fields['textId2'] = loc_id
            fields['textDisableFlagId2'] = m['flag']
        fields['selectMinZoomStep'] = 1
        sink.add(row_id, fields)
        row_id += 1

    out_path = sink.write(OUT_DIR / 'World - Maps.rows')
    print(f"Written {len(maps)} entries to {out_path.name}")


if __name__ == '__main__':
    main()
