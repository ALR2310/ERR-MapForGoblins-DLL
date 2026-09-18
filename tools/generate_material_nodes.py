#!/usr/bin/env python3
"""
Generate Loot - Material Nodes.rows from MSB gathering nodes.

Scans all_gathering_nodes_final.json for one-time pickup nodes
(isEnableRepick=True AND isHiddenOnRepick=True) and generates
WorldMapPointParam marker rows.

Output: data/rows_generated/Loot - Material Nodes.rows
"""

import json
import os
import sys
from pathlib import Path
import config
import rowsink
from marker_common import (UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS,
                             resolve_location_id, resolve_location_id_at, is_dlc_plane)
from unreachable import is_unreachable_in_err

def main():
    data_dir = config.DATA_DIR          # data/ or data/vanilla/ per profile
    out_dir = data_dir / "rows_generated"
    out_dir.mkdir(parents=True, exist_ok=True)

    # Load mappings
    with open(data_dir / "aeg099_item_mapping.json") as f:
        aeg099 = json.load(f)
    with open(data_dir / "aeg463_item_mapping.json") as f:
        aeg463 = json.load(f)
    with open(data_dir / "all_gathering_nodes_final.json") as f:
        all_nodes = json.load(f)

    # ERR-specific per-instance collection flags extracted from EMEVD.
    # Only the `by_tile_entity` path is trusted: it's derived from actual
    # EMEVD instructions that wire a tile+EntityID pair to a specific
    # collection flag. The old `by_name_suffix` fallback was a naive
    # heuristic that reused any tile's flag for every node with a matching
    # name suffix - producing phantom flags pointing at the wrong tile.
    # Nodes with entity_id=0 (no EMEVD binding) fall back to runtime-only
    # hiding via collected::refresh(), same as Rune/Ember Pieces.
    gn_flags_path = data_dir / "gathering_node_flags.json"
    if gn_flags_path.exists():
        with open(gn_flags_path) as f:
            gn_flags_data = json.load(f)
        gn_flags_by_tile = gn_flags_data.get("by_tile_entity", {})
    else:
        gn_flags_by_tile = {}

    # One-time models: isEnableRepick=True AND isHiddenOnRepick=True.
    # AEG099_821 (Rune Piece) and AEG099_822 (Ember Piece) also match but are
    # handled by generate_pieces.py - don't double-generate markers.
    PIECES_MODELS = {"AEG099_821", "AEG099_822"}
    onetime_models = {}
    for e in aeg099 + aeg463:
        if e["model"] in PIECES_MODELS:
            continue
        if e.get("isEnableRepick") and e.get("isHiddenOnRepick"):
            onetime_models[e["model"]] = e

    print(f"One-time models: {len(onetime_models)}")

    # Filter nodes
    onetime_nodes = [n for n in all_nodes if n["model"] in onetime_models]
    print(f"One-time nodes in MSBs: {len(onetime_nodes)}")

    # Deduplicate: same model + same coordinates = same physical object
    # (appears in multiple MSB variants like _00 and _10)
    seen_coords = set()
    unique_nodes = []
    dupes = 0
    for n in onetime_nodes:
        key = (n["model"], round(n["x"], 3), round(n["y"], 3), round(n["z"], 3))
        if key in seen_coords:
            dupes += 1
            continue
        seen_coords.add(key)
        unique_nodes.append(n)
    print(f"Deduplicated: {dupes} duplicates removed, {len(unique_nodes)} unique nodes")
    onetime_nodes = unique_nodes

    START_ID = __import__("row_id_registry").base("Loot - Material Nodes")  # z-order slot; see row_id_registry
    entries = []
    emitted_nodes = []  # node behind entries[i] - same-loop capture for slots.json
    excluded_unreachable = 0
    for n in onetime_nodes:
        # Skip nodes that sit out of reach in ERR (see unreachable.py).
        if is_unreachable_in_err(n.get("map", ""), n.get("name", ""), n.get("y", 0.0)):
            excluded_unreachable += 1
            continue
        area = n["area"]
        info = onetime_models[n["model"]]
        goods_id = info.get("primaryGoodsId", info.get("goodsId", 0))
        if not goods_id or goods_id == 17000:
            continue

        entry = {
            "id": START_ID + len(entries),
            "iconId": __import__("icon_registry").iconid("material_nodes"),
            "areaNo": area,
            "posX": round(n["x"], 3),
            "posY": round(n["y"], 3),
            "posZ": round(n["z"], 3),
            "textId1": goods_id + 500000000,  # offset-encoded to avoid PlaceName collision
            "selectMinZoomStep": 1,
        }
        # Location subtitle for non-overworld maps - nearest-grace lookup
        # (disambiguates stacked dungeon regions like Nokron / Siofra)
        if area not in OVERWORLD_AREAS:
            loc_id = resolve_location_id_at(
                n.get("map", ""), n.get("x", 0.0), n.get("y", 0.0), n.get("z", 0.0))
            if loc_id > 0:
                entry["textId2"] = loc_id

        # ERR per-instance collection flag - hides the marker once the node
        # has been picked even on tiles that are currently unloaded. Only
        # emit the flag when we have a genuine EMEVD-derived mapping for
        # this specific (tile, entity_id); no heuristic fallback. Nodes
        # without a mapping rely on runtime collected::refresh() to hide.
        entity_id = n.get("entity_id", 0)
        if entity_id:
            tile_flags = gn_flags_by_tile.get(n.get("map", ""), {})
            flag = tile_flags.get(str(entity_id))
            if flag:
                entry["textDisableFlagId1"] = flag
                if "textId2" in entry:
                    entry["textDisableFlagId2"] = flag
        if area in (60, 61):
            entry["gridXNo"] = n["p1"]
            entry["gridZNo"] = n["p2"]
            if area == 61:
                entry["pad2_0"] = 1
            else:
                entry["dispMask00"] = 1
        elif area in UNDERGROUND_AREAS:
            entry["gridXNo"] = n["p1"]
            entry["dispMask01"] = 1
        elif is_dlc_plane(area, n["p1"]):
            entry["gridXNo"] = n["p1"]
            entry["pad2_0"] = 1
        else:
            entry["gridXNo"] = n["p1"]
            entry["dispMask00"] = 1

        entries.append(entry)
        emitted_nodes.append(n)

    print(f"Generated {len(entries)} marker rows "
          f"({excluded_unreachable} skipped as unreachable)")

    # Write slots.json for geom tracking. Pair each entry with the node captured in
    # the SAME loop iteration - never reconstruct the list by re-filtering: the loop
    # also skips unreachable nodes, and a re-filter that misses one condition shifts
    # every name/geom_slot after the first exclusion (mislabeled markers + collected-
    # tracking hiding the wrong icon).
    slots = {}
    for entry, node in zip(entries, emitted_nodes):
        # Strip the SoulsFormats duplicate-name decoration: ERR copy-pastes parts keeping
        # the name, the reader disambiguates as "AEG099_931_9006 {2}". In-game the part is
        # plain "AEG099_931_9006" - bake THAT (decorated names parse to no slot/no name and
        # the row becomes invisible to collected-tracking).
        name = node["name"].split(" {")[0]  # e.g. "AEG099_651_9000"
        parts = name.rsplit("_", 1)
        if len(parts) == 2 and parts[1].isdigit():
            suffix = int(parts[1])
            slot = suffix - 9000
            # full_name e.g. "AEG099_651_9000"
            slots[str(entry["id"])] = {"geom_slot": slot, "name_suffix": suffix, "object_name": name}

    slots_path = out_dir / "Loot - Material Nodes_slots.json"
    with open(slots_path, "w") as f:
        json.dump(slots, f, indent=2)
    print(f"Written {len(slots)} slot entries to {slots_path}")

    # Write row file
    out_path = out_dir / "Loot - Material Nodes.rows"
    # The record is rebuilt here rather than handed over as-is: `entries` carries its fields in the
    # order the builder above happened to set them, while the file's order is this loop's. The two
    # differ (id and selectMinZoomStep go in early there, the masks and grid last), so the sink is
    # fed in THIS order - the one every consumer has ever seen.
    sink = rowsink.RowSink()
    for e in entries:
        fields = {"iconId": e["iconId"]}
        for mask in ("dispMask00", "dispMask01", "pad2_0"):
            if mask in e:
                fields[mask] = e[mask]
        fields["areaNo"] = e["areaNo"]
        for grid in ("gridXNo", "gridZNo"):
            if grid in e:
                fields[grid] = e[grid]
        fields["posX"] = e["posX"]
        fields["posY"] = e["posY"]
        fields["posZ"] = e["posZ"]
        fields["textId1"] = e["textId1"]
        if "textDisableFlagId1" in e:
            fields["textDisableFlagId1"] = e["textDisableFlagId1"]
        if "textId2" in e:
            fields["textId2"] = e["textId2"]
        if "textDisableFlagId2" in e:
            fields["textDisableFlagId2"] = e["textDisableFlagId2"]
        fields["selectMinZoomStep"] = e["selectMinZoomStep"]
        sink.add(e["id"], fields)
    sink.write(out_path)

    print(f"Written to {out_path}")

    # Summary
    from collections import Counter
    items = Counter()
    model_to_item = {e["model"]: e.get("primaryItem", "?") for e in aeg099 + aeg463}
    for n in onetime_nodes:
        if n["model"] in onetime_models:
            items[model_to_item.get(n["model"], n["model"])] += 1

    areas = Counter(e["areaNo"] for e in entries)
    print(f"\nBy area: {dict(sorted(areas.items()))}")
    print(f"Top items:")
    for item, cnt in items.most_common(10):
        print(f"  {item}: {cnt}")


if __name__ == "__main__":
    main()
