#!/usr/bin/env python3
"""
Bake the generated marker rows into the DLL's own data.

Reads every category's .rows file (tools/rowsink.py wrote them) and packs the whole map table into
one deflated blob. It used to emit the same table as 3.34 MB of C++ brace initialisers per profile,
which MSVC then parsed on every build of all nine and shipped as 2.14 MB of .rdata.

Output:
  - src/generated/goblin_map_blob_data.cpp  (the packed table as a byte array; src/goblin_map_blob.cpp
                                             expands it at startup into the MapEntry array declared
                                             in the hand-maintained src/goblin_map_data.hpp)
  - src/generated/goblin_legacy_conv.hpp    (dungeon coord conversion)

Localization is handled by the DLL at runtime via FMG offset-encoding
(textId = real_id + category_offset), so no text compilation step is needed.
"""

import os
import re
import json
import sys
from collections import defaultdict
from pathlib import Path

import mapblob

# Category mapping: row-file name -> Category enum
CATEGORY_MAP = {
    "Equipment - Armaments": "EquipArmaments",
    "Equipment - Armour": "EquipArmour",
    "Equipment - Ashes of War": "EquipAshesOfWar",
    "Equipment - Spirits": "EquipSpirits",
    "Equipment - Talismans": "EquipTalismans",
    "Key - Celestial Dew": "KeyCelestialDew",
    "Key - Cookbooks": "KeyCookbooks",
    "Key - Crystal Tears": "KeyCrystalTears",
    "Key - Great Runes": "KeyGreatRunes",
    "Key - Imbued Sword Keys": "KeyImbuedSwordKeys",
    "Key - Larval Tears": "KeyLarvalTears",
    "Key - Lost Ashes": "KeyLostAshes",
    "Key - Pots n Perfumes": "KeyPotsNPerfumes",
    "Key - Scadutree Fragments": "KeyScadutreeFragments",
    "Key - Revered Spirit Ashes": "KeyReveredSpiritAshes",
    "Key - Seeds Tears Ashes": "KeySeedsTears",
    "Key - Spectral Steed Regalia": "KeySpectralSteedRegalia",
    "Key - Whetblades": "KeyWhetblades",
    "Loot - Ammo": "LootAmmo",
    "Loot - Bell-Bearings": "LootBellBearings",
    "Loot - Consumables": "LootConsumables",
    "Loot - Crafting Materials": "LootCraftingMaterials",
    "Loot - Gestures": "LootGestures",
    "Loot - Gloveworts": "LootGloveworts",
    "Loot - Golden Runes": "LootGoldenRunes",
    "Loot - Golden Runes (Low)": "LootGoldenRunesLow",
    "Loot - Great Gloveworts": "LootGreatGloveworts",
    "Loot - Greases": "LootGreases",
    "Loot - Material Nodes": "LootMaterialNodes",
    "Loot - Merchant Bell-Bearings": "LootMerchantBellBearings",
    "Loot - MP-Fingers": "LootMPFingers",
    "Loot - Prattling Pates": "LootPrattlingPates",
    "Loot - Reusables": "LootReusables",
    "Loot - Smithing Stones": "LootSmithingStones",
    "Loot - Smithing Stones (Low)": "LootSmithingStonesLow",
    "Loot - Smithing Stones (Rare)": "LootSmithingStonesRare",
    "Loot - Stat Boosts": "LootStatBoosts",
    "Loot - Stonesword Keys": "LootStoneswordKeys",
    "Loot - Throwables": "LootThrowables",
    "Loot - Rune Arcs": "LootRuneArcs",
    "Loot - Dragon Hearts": "LootDragonHearts",
    "Loot - Utilities": "LootUtilities",
    "Magic - Incantations": "MagicIncantations",
    "Magic - Memory Stones": "MagicMemoryStones",
    "Magic - Prayerbooks": "MagicPrayerbooks",
    "Magic - Sorceries": "MagicSorceries",
    "Quest - Deathroot": "QuestDeathroot",
    "Quest - Progression": "QuestProgression",
    "Quest - Seedbed Curses": "QuestSeedbedCurses",
    "Reforged - Ember Pieces": "ReforgedEmberPieces",
    "Reforged - Fortunes": "ReforgedFortunes",
    "Reforged - Items": "ReforgedItemsAndChanges",
    "Reforged - Rune Pieces": "ReforgedRunePieces",
    "Reforged - Sealed Curios": "ReforgedItemsAndChanges",
    "World - Bosses": "WorldBosses",
    "World - Graces": "WorldGraces",
    "World - Hostile NPC": "WorldHostileNPC",
    "World - Strong Enemies": "WorldStrongEnemies",
    "World - Imp Statues": "WorldImpStatues",
    "World - Maps": "WorldMaps",
    "World - Paintings": "WorldPaintings",
    "World - Spirit Springs": "WorldSpiritSprings",
    "World - Spiritspring Hawks": "WorldSpiritspringHawks",
    "World - Stakes of Marika": "WorldStakesOfMarika",
    "World - Summoning Pools": "WorldSummoningPools",
    "World - Kindling Spirits": "WorldKindlingSpirits",
    "World - Seal Puzzles": "WorldInteractables",
    "World - Hero's Tomb Statues": "WorldInteractables",
}

# Fields to skip
SKIP_FIELDS = {"Name", "pad4"}


# ERR-only categories: never bake these in the vanilla profile, even
# if a stale file is present in the (gitignored) vanilla output dir.
ERR_ONLY_FILES = {
    "Reforged - Rune Pieces", "Reforged - Ember Pieces",
    "Reforged - Items", "Reforged - Fortunes", "Reforged - Sealed Curios",
    "World - Kindling Spirits",
}


def parse_row_files(rows_dir):
    """Read every category's .rows file -> {row_id: {field: value_str, ..., '_category': str}}.

    Values stay strings: the interior coord fix below re-formats with :.3f, and build_map_records
    converts once at the end. Floats are rendered at 3 decimals, which is what every generator wrote
    when this was a text format, so the baked value is unchanged by the move off it.
    """
    import config
    import rowsink
    entries = defaultdict(dict)

    for filepath in sorted(Path(rows_dir).glob("*.rows")):
        filename = filepath.stem
        if config.PROFILE != 'err' and filename in ERR_ONLY_FILES:
            print(f"SKIP (ERR-only): {filename}")
            continue
        category = CATEGORY_MAP.get(filename)
        if category is None:
            # Auto-generate category name from filename
            category = re.sub(r'[^A-Za-z0-9]', '', filename.replace(' - ', '_').replace(' ', '_'))
            print(f"INFO: Auto-category for '{filename}' -> {category}")

        for row_id, fields in rowsink.read(filepath):
            for field, value in fields.items():
                if field in SKIP_FIELDS:
                    continue
                if field not in mapblob.FIELD_ORDER:
                    # rowsink refuses an unknown name at the call site, so reaching this means the
                    # packer's field list has drifted from rowsink's - worth saying loudly rather
                    # than dropping a field the generator meant to write.
                    print(f"WARNING: '{field}' in {filename} has no slot in the packed record, "
                          f"skipping")
                    continue
                entries[row_id][field] = (f"{value:.3f}" if isinstance(value, float)
                                          else str(value))
            entries[row_id]["_category"] = category

    # Fix interior areas that don't display correctly on overworld
    # e.g. m11_10 (Roundtable Hold) - MSB coords land in ocean
    # Shift all coords to match grace display positions (area stays unchanged)
    # m11_10: MSB ~(-305,-298) → grace display ~(-2500,-650) = offset (-2195,-352)
    COORD_SHIFTS = {(11, 10): (-2195.0, -352.0)}
    shifted = 0
    for row_id, fields in entries.items():
        area = int(fields.get("areaNo", "0"))
        gx = int(fields.get("gridXNo", "0"))
        shift = COORD_SHIFTS.get((area, gx))
        if shift:
            dx, dz = shift
            fields["posX"] = f"{float(fields.get('posX', '0')) + dx:.3f}"
            fields["posZ"] = f"{float(fields.get('posZ', '0')) + dz:.3f}"
            shifted += 1
    if shifted > 0:
        print(f"  Shifted {shifted} entries (interior coord fix)")

    return entries


def load_piece_metadata(rows_dir):
    """Load geom_slot and name_suffix from *_slots.json files.
    Returns dict: row_id (int) -> {geom_slot: int, name_suffix: int}."""
    meta = {}
    for path in Path(rows_dir).glob("*_slots.json"):
        with open(path) as f:
            data = json.load(f)
        for row_id_str, val in data.items():
            if isinstance(val, dict):
                meta[int(row_id_str)] = val
            else:
                # Legacy format: just geom_slot as int
                meta[int(row_id_str)] = {'geom_slot': val, 'name_suffix': -1}
    return meta


def _load_lot_linkage():
    """row_id(int) -> (lotId, lotType, aggregate) from generate_loot's side file.
    Older side files carry two numbers; those markers are not aggregates."""
    import config
    p = config.DATA_DIR / 'loot_lot_linkage.json'
    if not p.exists():
        return {}
    with open(p, encoding='utf-8') as f:
        raw = json.load(f)
    return {int(k): (int(v[0]), int(v[1]), int(v[2]) if len(v) > 2 else 0)
            for k, v in raw.items()}


def build_map_records(entries, geom_slots=None):
    """The map table as plain records, row_id order - the input to tools/mapblob.pack().

    This used to write 3.34 MB of C++ brace initialisers straight out; the records exist as their
    own step because the packer and the verification both read them.
    """
    if geom_slots is None:
        geom_slots = {}

    lot_linkage = _load_lot_linkage()
    records = []

    for row_id in sorted(entries.keys()):
        fields = entries[row_id]
        category = fields.get("_category", "World")

        # Values arrive as strings (the row files render positions at 3 decimals). int(float(v))
        # rather than int(v) because that is what the old text emitter did, and an integer field
        # that arrived as "5.0" has to land on 5, not raise.
        param = {}
        for row_field, raw_value in fields.items():
            if row_field.startswith("_") or row_field in SKIP_FIELDS:
                continue
            param[row_field] = (float(raw_value) if row_field in mapblob.FLOAT_FIELDS
                                else int(float(raw_value)))

        meta = geom_slots.get(row_id, {})
        slot = meta.get('geom_slot', -1) if isinstance(meta, dict) else meta
        suffix = meta.get('name_suffix', -1) if isinstance(meta, dict) else -1
        obj_name = meta.get('object_name', '') if isinstance(meta, dict) else ''
        lot_id, lot_type, lot_aggregate = lot_linkage.get(row_id, (0, 0, 0))
        # real_posX/real_posZ: where collected tracking has to look. Normally the marker's
        # own position, but a piece whose DISPLAY position was moved onto its pickup target
        # carries the MSB position of the asset itself in the slots side file - that is where
        # the live CSWorldGeomIns sits, so that is what tracking must match. Emitted separately
        # for exactly that case: feeding the MSB position back into the row would move the marker.
        rx, rz = fields.get("posX", "0"), fields.get("posZ", "0")
        if isinstance(meta, dict) and 'msb_x' in meta and 'msb_z' in meta:
            rx, rz = meta['msb_x'], meta['msb_z']

        records.append({
            "row_id": row_id, "category": category, "fields": param,
            "geom_slot": int(slot), "name_suffix": int(suffix), "object_name": obj_name,
            "lotId": lot_id, "lotType": lot_type, "lotAggregate": lot_aggregate,
            "real_posX": float(rx), "real_posZ": float(rz),
        })
    return records


def generate_map_blob_cpp(records, output_path, header_path):
    """Pack the records and emit them as the deflated byte array the DLL expands at startup."""
    categories = mapblob.category_index(header_path)
    raw = mapblob.pack(records, categories)
    raw_len, packed_len = mapblob.write_cpp(raw, output_path)
    print(f"Generated {output_path}: {len(records)} entries, "
          f"{raw_len / 1024:.0f} KB packed -> {packed_len / 1024:.0f} KB deflated")


def generate_item_icons_cpp(output_path):
    """Generate goblin_item_icons.cpp: encoded-item-key -> (iconId, Category).

    Source = item_icon_table.json from generate_loot (the same ordered
    LOOT_CATEGORIES classifier, applied per item). Lets the DLL re-icon and
    re-gate a lot-backed marker from the LIVE randomized item. Sorted by key
    for binary search."""
    import config
    p = config.DATA_DIR / 'item_icon_table.json'
    table = {}  # key(int) -> (iconId, Category enum name)
    if p.exists():
        with open(p, encoding='utf-8') as f:
            raw = json.load(f)
        for k, v in raw.items():
            enum = CATEGORY_MAP.get(v[1])
            if enum:
                table[int(k)] = (int(v[0]), enum)

    # Spoiler-free "?" map-icon iconId. Just another registry-assigned icon id; the
    # DLL injects its frame and remaps markers to it at runtime like every other icon.
    anon_icon_id = __import__("icon_registry").iconid("anon")
    # Green "cleared" check badge (native-marker twin child for defeated rows).
    cleared_icon_id = __import__("icon_registry").iconid("cleared")
    # Focus ring: an extra marker row over the isolated category (no icon swapping).
    highlight_icon_id = __import__("icon_registry").iconid("highlight")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from item_icon_table.json\n\n")
        f.write('#include "../goblin_item_icons.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write(f"const uint16_t ANON_ICON_ID = {anon_icon_id}u;\n\n")
        f.write(f"const uint16_t CLEARED_ICON_ID = {cleared_icon_id}u;\n\n")
        f.write(f"const uint16_t HIGHLIGHT_ICON_ID = {highlight_icon_id}u;\n\n")
        f.write(f"const size_t ITEM_ICON_COUNT = {len(table)};\n\n")
        f.write("const ItemIcon ITEM_ICONS[] = {\n")
        for key in sorted(table.keys()):
            icon, enum = table[key]
            f.write(f"    {{{key}, {icon}u, Category::{enum}}},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {output_path} with {len(table)} item-icon entries")


def generate_item_fallback_cpp(output_path, entries=None):
    """Generate goblin_item_fallback.cpp: marker-textId -> English string. Injected by
    setup_messages as the lowest-priority PlaceName layer so a marker whose text has no
    string in the player's LANGUAGE falls back to English instead of resolving to nothing
    (the engine draws no icon for a fully text-less marker). Overhauls (e.g. The Convergence)
    localize only part of their custom content, so a non-English player otherwise gets blank
    boss names / locations / item names. Covers three id spaces, all keyed the way the marker
    references them (so remap_textid lines them up):
      - items:     encoded-item-key -> English name   (item_icon_table.json, 3rd field)
      - bosses:    npcNameId+700M    -> English name   (boss_list.json vanillaPlaceName)
      - locations: raw PlaceName id  -> English name   (PlaceName_engus.json), only ids a
                   marker actually references (collected from `entries`)."""
    import config
    p = config.DATA_DIR / 'item_icon_table.json'
    table = {}  # key(int) -> English name
    if p.exists():
        with open(p, encoding='utf-8') as f:
            raw = json.load(f)
        for k, v in raw.items():
            name = (v[2] if len(v) > 2 else '').strip()
            if name:
                table[int(k)] = name

    # English fallback for EVERY other band a marker references (NpcName boss/NPC
    # names +700M, ActionButtonText prompts +800M, raw PlaceName locations), from
    # english_fallback.json (extract_english_fallback.py dumps it from the engus
    # msgbnd, keyed by the same offset-encoded id the markers use). Only ids a marker
    # actually references are emitted. setdefault => a real localized string (copied
    # from the player's language at runtime) still wins; this only fills the gaps an
    # overhaul left untranslated in that language. Items are already covered above.
    if entries:
        ef = config.DATA_DIR / 'english_fallback.json'
        english = {}
        if ef.exists():
            with open(ef, encoding='utf-8') as f:
                english = json.load(f)
        else:
            print("  WARNING: english_fallback.json not found - run extract_english_fallback.py")
        referenced = set()
        for fields in entries.values():
            for fld, val in fields.items():
                if not fld.startswith('textId'):
                    continue
                try:
                    tid = int(float(val))
                except (TypeError, ValueError):
                    continue
                if tid > 0:
                    referenced.add(tid)
        for tid in referenced:
            nm = english.get(str(tid))
            if nm:
                table.setdefault(tid, nm.strip())

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from item_icon_table.json\n\n")
        f.write('#include "../goblin_item_fallback.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write(f"const size_t ITEM_NAME_FALLBACK_COUNT = {len(table)};\n\n")
        f.write("const ItemNameFallback ITEM_NAME_FALLBACK[] = {\n")
        for key in sorted(table.keys()):
            f.write(f"    {{{key}, {_wlit(table[key])}}},\n")
        if not table:
            f.write("    {0, nullptr},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"Generated {output_path} with {len(table)} item-name fallback entries")


# Fixed language order for the embedded enemy-name table (msgbnd codes). engus
# first so it can serve as the fallback. Must match ENEMY_NAME_LANGS in the DLL.
ENEMY_NAME_LANGS = ["engus", "jpnjp", "deude", "frafr", "itait", "korkr", "polpl",
                    "porbr", "rusru", "spaes", "spaar", "thath", "zhocn", "zhotw", "araae"]


def _wlit(s):
    """C++ wide-string literal with every non-ASCII unit as \\uXXXX (encoding-safe)."""
    units = s.encode("utf-16-le")
    out = []
    for i in range(0, len(units), 2):
        cu = units[i] | (units[i + 1] << 8)
        if 0x20 <= cu < 0x7f and chr(cu) not in '"\\':
            out.append(chr(cu))
        else:
            out.append("\\u%04x" % cu)
    return 'L"' + "".join(out) + '"'


def generate_enemy_names_cpp(output_path):
    """Generate goblin_enemy_names.cpp: localized enemy names for the non-ERR
    builds (marker textId = id + 900000000). Empty for the ERR build (it uses
    its own runtime name table). Strings are FromSoft / community-wiki enemy
    names; no reference to where the source data was read from."""
    import config
    is_err = (config.PROFILE == "err")
    table = {}
    if not is_err:
        p = config.INPUTS_DIR / "enemy_names_i18n.json"
        if p.exists():
            with open(p, encoding="utf-8") as f:
                table = json.load(f)

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from enemy_names_i18n.json\n\n")
        f.write('#include "../goblin_enemy_names.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write("const char *const ENEMY_NAME_LANGS[ENEMY_NAME_LANG_COUNT] = {\n    ")
        f.write(", ".join(f'"{c}"' for c in ENEMY_NAME_LANGS))
        f.write("\n};\n\n")
        f.write(f"const size_t ENEMY_NAME_COUNT = {len(table)};\n\n")
        f.write("const EnemyName ENEMY_NAMES[] = {\n")
        for k in sorted(table, key=lambda x: int(x)):
            langs = table[k]
            en = langs.get("engus", "")
            cells = [_wlit(langs.get(code, en)) for code in ENEMY_NAME_LANGS]
            f.write(f"    {{{int(k)}, {{{', '.join(cells)}}}}},\n")
        if not table:
            f.write("    {0, {" + ", ".join(["nullptr"] * len(ENEMY_NAME_LANGS)) + "}},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"Generated {output_path} with {len(table)} enemy-name entries"
          + (" (ERR: empty, uses runtime table)" if is_err else ""))


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows-dir", type=str, default=None,
                        help="Path to the generated rows directory (default: data/rows_generated; "
                             "the pipeline passes the active profile's rows_generated)")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    project_dir = script_dir.parent

    if args.rows_dir:
        rows_dir = Path(args.rows_dir)
    else:
        rows_dir = project_dir / "data" / "rows_generated"
    import config
    output_dir = config.GENERATED_DIR  # src/generated or src/generated_vanilla

    output_dir.mkdir(parents=True, exist_ok=True)

    # The headers these .cpp files implement (goblin_map_data.hpp = the Category enum + struct,
    # goblin_item_icons/enemy_names/item_fallback.hpp) are hand-maintained and live in src/; the
    # generated files include them as "../<name>.hpp". They used to sit in src/generated/ and be
    # mirrored into each bake dir - a copy that went stale whenever this stage was cached, and a
    # source that left git with src/generated/. Older bake dirs may still hold those copies; the
    # "../" include never reads them.

    print("=== Reading generated rows ===")
    entries = parse_row_files(rows_dir)
    print(f"Total unique entries: {len(entries)}")

    # Tiles the game's world-map converter refuses (custom overhaul maps with no
    # WorldMapLegacyConvParam route to the overworld, and a few vanilla tiles the engine
    # itself declines). A marker there is never drawn - the engine answers "cannot convert"
    # and the DLL skips it, 111 rows on Golden Age 3.6.8 - so it is dropped here instead of
    # being baked, counted as "failed" on every map open and hiding real failures.
    # Patterns per profile live in inputs/unprojectable_tiles.json: "m32_68" is one tile
    # (gz 00 implied), "m32_68_01" a specific one, "m32_*" a whole area. Rows dropped here
    # are listed once per bake.
    def _load_unprojectable():
        p = config.INPUTS_DIR / "unprojectable_tiles.json"
        if not p.exists():
            return []
        with open(p, encoding="utf-8") as f:
            table = json.load(f)
        return [str(x) for x in list(table.get(config.PROFILE, [])) + list(table.get("*", []))]

    def _tile_blocked(tile, patterns):
        # tile = "mAA_GX_GZ"
        for pat in patterns:
            if pat.endswith("_*"):
                if tile.startswith(pat[:-1]):
                    return True
            elif pat.count("_") == 1:
                if tile == pat + "_00":
                    return True
            elif tile == pat:
                return True
        return False

    _unproj = _load_unprojectable()
    if _unproj:
        _dropped = {}
        for rid in list(entries.keys()):
            f = entries[rid]
            if f.get("areaNo", "0") in ("60", "61", "99"):
                continue
            tile = "m%02d_%02d_%02d" % (int(f.get("areaNo", "0")), int(f.get("gridXNo", "0")), int(f.get("gridZNo", "0")))
            if _tile_blocked(tile, _unproj):
                _dropped[tile] = _dropped.get(tile, 0) + 1
                entries.pop(rid)
        if _dropped:
            print(f"Dropped {sum(_dropped.values())} rows on tiles the engine cannot project: "
                  + ", ".join(f"{t}={n}" for t, n in sorted(_dropped.items())))

    print("\n=== Loading piece metadata ===")
    geom_slots = load_piece_metadata(rows_dir)
    print(f"Loaded {len(geom_slots)} piece metadata entries")

    print("\n=== Generating map data C++ ===")
    records = build_map_records(entries, geom_slots)
    generate_map_blob_cpp(records, output_dir / "goblin_map_blob_data.cpp",
                          project_dir / "src" / "goblin_map_data.hpp")

    print("\n=== Generating item-icon table C++ ===")
    generate_item_icons_cpp(output_dir / "goblin_item_icons.cpp")

    print("\n=== Generating enemy-name table C++ ===")
    generate_enemy_names_cpp(output_dir / "goblin_enemy_names.cpp")
    generate_item_fallback_cpp(output_dir / "goblin_item_fallback.cpp", entries)

    print("\n=== Generating legacy-conv C++ ===")
    generate_legacy_conv_cpp(config.DATA_DIR / "WorldMapLegacyConvParam.json",
                             output_dir / "goblin_legacy_conv.hpp")

    print("\nDone.")


def generate_legacy_conv_cpp(conv_json, output_path):
    """Emit a C++ header with the dungeon→overworld conversion table.
    Used by goblin_markers to place dungeon MAP_ENTRIES on overworld coords."""
    if not conv_json.exists():
        print(f"  WARNING: {conv_json} missing, skipping")
        return
    # Chain resolution lives in tools/legacy_conv.py - the offline marker tool reads the same param and
    # had the same single-hop bug, and two copies of this is how they diverge.
    import legacy_conv
    resolved, chains, unresolved = legacy_conv.resolve(conv_json)
    legacy_conv.report(chains, unresolved)
    entries = sorted(resolved.values(), key=lambda e: (e['src_area'], e['src_gx']))
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("#pragma once\n// AUTO-GENERATED - do not edit.\n")
        f.write("// Dungeon-area → overworld-tile conversion table (first base-point per src key).\n\n")
        f.write("#include <cstdint>\n#include <cstddef>\n\n")
        f.write("namespace goblin::generated {\n\n")
        f.write("struct LegacyConvEntry {\n")
        f.write("    uint8_t src_area;\n    uint8_t src_gx;\n")
        f.write("    float src_pos_x;\n    float src_pos_z;\n")
        f.write("    uint8_t dst_area;\n    uint8_t dst_gx;\n    uint8_t dst_gz;\n")
        f.write("    float dst_pos_x;\n    float dst_pos_z;\n")
        f.write("};\n\n")
        f.write(f"constexpr LegacyConvEntry LEGACY_CONV[] = {{\n")
        for e in entries:
            f.write(f"    {{ {e['src_area']}, {e['src_gx']}, {e['src_pos_x']:.3f}f, {e['src_pos_z']:.3f}f, ")
            f.write(f"{e['dst_area']}, {e['dst_gx']}, {e['dst_gz']}, ")
            f.write(f"{e['dst_pos_x']:.3f}f, {e['dst_pos_z']:.3f}f }},\n")
        f.write("};\n\n")
        f.write(f"constexpr size_t LEGACY_CONV_COUNT = {len(entries)};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"  Generated {output_path.name} with {len(entries)} conv entries")


if __name__ == "__main__":
    main()
