#!/usr/bin/env python3
"""Detect relocating-boss flee-spawns (currently: Ancient Dragon Lansseax).

A few vanilla bosses flee their first arena and become killable at a SECOND location (Lansseax:
flies off at ~20% HP, then fought elsewhere). The drop is only obtainable at the kill-spawn, but the
flee-spawn MSB entity references the same lot, so a naive generator would place duplicate,
un-collectable loot there and clear the boss marker on the wrong flag.

This script only FINDS those spawns and caches them to data/relocating_flee_spawns.json. The
generators read that list (tools/relocating_spawns.py) and never create the wrong rows in the first
place - until 2026-09-18 this file instead rewrote the generated output afterwards, which is what
forced the pipeline to keep an editable text intermediate.

Detection is data-derived, no hardcoded boss list:
  relocating boss = ONE npcParamID placed at 2 tiles (from boss_list.json)
  AND one tile's boss defeat flag is referenced as a CONDITION in the OTHER
  tile's EMEVD (the relocation gate). That intersection matches ONLY Lansseax
  today (Night's Cavalry / Burial Watchdog reuse an npc but have no cross-link;
  Ghostflame Dragon / Fallingstar Beast are distinct npcs). Vanilla content, so
  it applies to every profile.
"""
import sys, os, re, struct, tempfile, shutil, json, collections
sys.path.insert(0, os.path.dirname(__file__))
import config


def _safe_unlink(path):
    try:
        os.unlink(path)
    except PermissionError:
        pass


def _sf():
    from pythonnet import load; load("coreclr")
    import clr
    from System.Reflection import Assembly
    asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
    import SoulsFormats
    src = config.GAME_DIR / "oo2core_6_win64.dll"
    for d in (config.LIB_DIR, tempfile.gettempdir(), os.getcwd()):
        p = os.path.join(str(d), "oo2core_6_win64.dll")
        if src.exists() and not os.path.exists(p):
            shutil.copy2(str(src), p)
    return asm, SoulsFormats


def _emevd_flag_refs(asm, SoulsFormats, event_dir, tile):
    """Set of event flags referenced as conditions (bank 1003) in a tile's EMEVD."""
    from System import Array, Object
    from System import Type as SysType
    from System.IO import File as SysFile
    from System.Reflection import BindingFlags
    _s = SysType.GetType("System.String")
    read = asm.GetType("SoulsFormats.EMEVD").GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_s]), None)
    p = event_dir / f"{tile}.emevd.dcx"
    if not p.exists():
        return set()
    tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_rbf.tmp")
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(p)).ToArray())
    em = read.Invoke(None, Array[Object]([tmp])); _safe_unlink(tmp)
    refs = set()
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank) != 1003:
                continue
            a = bytes(ins.ArgData)
            for o in range(0, len(a) - 3, 4):
                v = struct.unpack_from("<i", a, o)[0]
                if v > 10000000:
                    refs.add(v)
    return refs


CACHE = config.PROJECT_DIR / "data" / "relocating_flee_spawns.json"


def _scan_game_for_flee_spawns():
    """Derive relocating-boss flee-spawns from VANILLA game files (build-agnostic;
    the dragon is base-game content). Signature: a boss-death flag (= entity id)
    set by 90005860/70/80 in tile A and referenced as a condition in tile B,
    AND the same npcParamID present in both A and B (true relocation, not a
    reused-npc separate boss). Returns the list + caches it."""
    asm, SoulsFormats = _sf()          # loads pythonnet/clr first
    from System import Array, Object
    from System import Type as SysType
    from System.IO import File as SysFile
    from System.Reflection import BindingFlags
    _s = SysType.GetType("System.String")
    em_read = asm.GetType("SoulsFormats.EMEVD").GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_s]), None)
    msb_read = asm.GetType("SoulsFormats.MSBE").GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_s]), None)
    import glob
    boss_flag, refs = {}, {}   # flag -> set(tiles); flag -> set(tiles referenced in)
    for ep in sorted(glob.glob(str(config.GAME_DIR / "event" / "m60_*.emevd.dcx")) +
                     glob.glob(str(config.GAME_DIR / "event" / "m61_*.emevd.dcx"))):
        mp = os.path.basename(ep).replace(".emevd.dcx", "")
        try:
            tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_rbf.tmp")
            SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(ep).ToArray())
            em = em_read.Invoke(None, Array[Object]([tmp])); _safe_unlink(tmp)
        except Exception:
            continue
        for ev in em.Events:
            for ins in ev.Instructions:
                a = bytes(ins.ArgData); b = int(ins.Bank)
                if b == 2000 and len(a) >= 12 and struct.unpack_from("<i", a, 4)[0] in (90005860, 90005870, 90005880):
                    f = struct.unpack_from("<i", a, 8)[0]
                    if f > 10000000:
                        boss_flag.setdefault(f, set()).add(mp)
                elif b == 1003 and len(a) >= 8:
                    for o in range(0, len(a) - 3, 4):
                        v = struct.unpack_from("<i", a, o)[0]
                        if v > 10000000:
                            refs.setdefault(v, set()).add(mp)

    def msb_enemies(tile):
        p = config.GAME_DIR / "map" / "MapStudio" / f"{tile}.msb.dcx"
        if not p.exists():
            return {}
        msb = msb_read.Invoke(None, Array[Object]([str(p)]))
        return {int(e.EntityID): (str(e.ModelName), int(e.NPCParamID),
                                  float(e.Position.X), float(e.Position.Y), float(e.Position.Z))
                for e in msb.Parts.Enemies}

    def flee_flag(tile, gx, gz, death_flag):
        """The 'flew away' flag, NOT the death flag. A relocating boss never
        dies at its first arena (it flees at low HP), so 90005860/70/80's death
        flag is never set there - the marker must clear on the FLEE flag instead.
        Signature: a flag Set ON (2003:66) directly in this tile AND read back
        as a condition (bank 1003) that gates the boss's appearance - i.e. the
        local 'I've fled, stop spawning me here' flag. Confined to the tile's
        own flag range (1e9 + gx*1e6 + gz*1e4 + local), distinct from the death
        flag (which is set via a common-event template, never a direct 2003:66).
        Returns None if absent → caller falls back to the death flag."""
        p = config.GAME_DIR / "event" / f"{tile}.emevd.dcx"
        if not p.exists():
            return None
        tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_rbf.tmp")
        SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(p)).ToArray())
        em = em_read.Invoke(None, Array[Object]([tmp])); _safe_unlink(tmp)
        lo = 1_000_000_000 + gx * 1_000_000 + gz * 10_000
        hi = lo + 10_000
        set_on, cond = set(), set()
        for ev in em.Events:
            for ins in ev.Instructions:
                b, a = int(ins.Bank), bytes(ins.ArgData)
                if b == 2003 and int(ins.ID) == 66 and len(a) >= 9 and a[8] == 1:
                    f = struct.unpack_from("<i", a, 4)[0]       # Target Event Flag ID, ON
                    if lo <= f < hi and f != death_flag:
                        set_on.add(f)
                elif b == 1003:                                 # any flag-reading condition
                    for o in range(0, len(a) - 3, 4):
                        v = struct.unpack_from("<i", a, o)[0]
                        if lo <= v < hi:
                            cond.add(v)
        both = set_on & cond
        return min(both) if both else (min(set_on) if set_on else None)

    out = []
    for flag, set_tiles in boss_flag.items():
        dest_tiles = refs.get(flag, set()) - set_tiles
        if not dest_tiles:
            continue
        flee_tile = sorted(set_tiles)[0]                 # tile where the flag is set
        ents = msb_enemies(flee_tile)
        if flag not in ents:                             # flag == the flee-spawn entity id
            continue
        model, npc, x, y, z = ents[flag]
        # confirm the SAME npc exists at a destination tile (true relocation)
        if not any(npc in {n for (_, n, _, _, _) in msb_enemies(dt).values()} for dt in dest_tiles):
            continue
        a, gx, gz = int(flee_tile[1:3]), int(flee_tile[4:6]), int(flee_tile[7:9])
        mnum = int(model[1:]) if model[:1] == "c" and model[1:].isdigit() else 0
        marker_flag = flee_flag(flee_tile, gx, gz, flag) or flag   # flee flag, not death
        out.append({"tile": [a, gx, gz], "flag": marker_flag, "death_flag": flag,
                    "x": x, "y": y, "z": z,
                    "enemy_id": 900000000 + mnum * 1000 + 4 if mnum else 0, "model": model})
    try:
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        json.dump(out, open(CACHE, "w", encoding="utf-8"), indent=1)
    except OSError:
        pass
    return out


def detect_flee_spawns(refresh=False):
    """Build-agnostic flee-spawn list (cached). tile stored as [a,gx,gz]."""
    if not refresh and CACHE.exists():
        data = json.load(open(CACHE, encoding="utf-8"))
    else:
        data = _scan_game_for_flee_spawns()
    return [{**d, "tile": tuple(d["tile"])} for d in data]


def main():
    """Detect the flee-spawns and cache them. The generators read the cache and never create the
    rows that used to be repaired here."""
    spawns = detect_flee_spawns(refresh="--refresh" in sys.argv)
    print(f"[relocating-spawns] profile={config.PROFILE} flee-spawns detected: {len(spawns)}")
    for s in spawns:
        print(f"  {s.get('name') or s['model']} flee-spawn tile "
              f"m{s['tile'][0]}_{s['tile'][1]}_{s['tile'][2]} flag={s['flag']} "
              f"enemy_id={s['enemy_id']}")
    # Sentinel kept so the pipeline stage has a stable output to check.
    (config.DATA_DIR / "_relocating_boss_fix.done").write_text(
        f"spawns={len(spawns)}\n", encoding="utf-8")


if __name__ == "__main__":
    main()
