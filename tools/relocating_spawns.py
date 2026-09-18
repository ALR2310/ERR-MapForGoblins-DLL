"""Relocating-boss flee-spawns, as an INPUT to the marker generators.

A few vanilla bosses flee their first arena and are killed at a second one (Lansseax today). The
drop is only obtainable at the kill-spawn, but the flee-spawn MSB entity references the same lot, so
the loot generator - which knows nothing about relocating bosses - would place duplicate,
un-collectable loot and rune markers there, and the boss marker at the flee-spawn would clear on the
wrong flag.

This used to be repaired AFTERWARDS: generate_relocating_boss_fix.py read every generated file back,
deleted rows and rewrote lines. That pass is why the pipeline had to keep an editable text
intermediate at all. The detection never needed those files - it comes from boss_list.json plus
EMEVD, both of which exist before any marker is generated - so the list is simply handed to the
generators and the wrong rows are never created.

The detection itself still lives in generate_relocating_boss_fix.py (it needs the game files and
caches its result); this module is the read side.
"""
import json

import config

CACHE = config.PROJECT_DIR / "data" / "relocating_flee_spawns.json"
RADIUS = 50.0   # loot within this of the flee-spawn boss is its duplicate drop

_cache = None


def load():
    """[{tile: (a, gx, gz), flag, death_flag, x, y, z, enemy_id, model}], empty if not detected yet.

    Read once per process: every generator that filters against it would otherwise re-read the same
    file for every row.
    """
    global _cache
    if _cache is None:
        try:
            data = json.loads(CACHE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            data = []
        _cache = [{**d, "tile": tuple(d["tile"])} for d in data]
    return _cache


def _num(fields, name, default=0):
    v = fields.get(name, default)
    try:
        return float(v)
    except (TypeError, ValueError):
        return float(default)


def at_tile(fields, tile):
    return (int(_num(fields, "areaNo", -1)) == tile[0]
            and int(_num(fields, "gridXNo", -1)) == tile[1]
            and int(_num(fields, "gridZNo", -1)) == tile[2])


def near(fields, x, z, radius=RADIUS):
    dx = _num(fields, "posX", 1e9) - x
    dz = _num(fields, "posZ", 1e9) - z
    return dx * dx + dz * dz <= radius * radius


def has_enemy(fields, enemy_id):
    """The marker names that enemy in any of its text slots."""
    if not enemy_id:
        return False
    return any(int(_num(fields, f"textId{i}", 0)) == enemy_id for i in range(1, 9))


def duplicate_drop(fields):
    """True for a loot/rune marker sitting on a flee-spawn: the item is not obtainable there.

    Tile, position AND the enemy's name. A drop marker carries the item in slot 1 and the enemy in
    slot 2, so the enemy test is what separates "this boss's drop" from any unrelated marker that
    happens to stand in the same clearing - dropping it would delete innocent markers.
    """
    for s in load():
        if (at_tile(fields, s["tile"]) and has_enemy(fields, s["enemy_id"])
                and near(fields, s["x"], s["z"])):
            return True
    return False


def boss_marker_for(fields):
    """The flee-spawn this BOSS marker stands on, or None. Matched on the enemy id as well, so a
    different boss that happens to stand nearby is left alone."""
    for s in load():
        if (at_tile(fields, s["tile"]) and has_enemy(fields, s["enemy_id"])
                and near(fields, s["x"], s["z"])):
            return s
    return None
