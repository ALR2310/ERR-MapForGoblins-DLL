#!/usr/bin/env python3
"""
Generate World - Strong Enemies.rows - every enemy the game wires through the common
"strong enemy" templates, 90005300 / 90005301 (common_func: 【共通】強敵リスポン処理).

Both templates are the same logic: on death SetEventFlag(X0_4) and AwardItems(X8_4); on every map
load, if X0_4 is already ON the character X4_4 is disabled. So these enemies stay dead once killed -
resting, reloading and quitting do not bring them back (only NG+ does, with the rest of the world).
The one exception in vanilla is common event 6901, a run-once save fix from a patch that cleared 23
of these flags a single time.

A marker per such enemy, at its MSB placement (the owning emevd's tile first; supertile placements
remapped onto their fine owner tile), labeled with its NpcName when it has one, else its enemy-type
name, hidden - or checkmarked, per hide_killed_bosses - on its kill flag X0_4. Their drops keep their
own loot markers.

Left out, each with a count: calls with no kill flag (nothing to hide on), characters with no MSB
placement, and named invader NPCs (teamType 24/27) - those are World - Hostile NPC markers already.
"""
import os
import struct
import tempfile
from collections import Counter

import config
import rowsink
import generate_hostile_npcs as H   # pythonnet/SoulsFormats readers, invader team filter
from marker_common import (OUT_DIR, OVERWORLD_AREAS, DLC_AREAS, get_disp_mask,
                             resolve_location_id_at, remap_placeholder_xz)
import SoulsFormats
from System import Array, Object, Type as SysType
from System.IO import File as SysFile
from System.Reflection import BindingFlags

TEMPLATES = (90005300, 90005301)
CATEGORY = 'World - Strong Enemies'


def npc_params():
    """NpcParam id -> (teamType, nameId)."""
    pds = H.load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.require_err_mod_dir() / 'regulation.bin'))
    out = {}
    for row in H.read_param(bnd, 'NpcParam', pds).Rows:
        team = name_id = None
        for cell in row.Cells:
            n = str(cell.Def.InternalName)
            if n == 'teamType':
                team = int(str(cell.Value))
            elif n == 'nameId':
                name_id = int(str(cell.Value))
        out[int(row.ID)] = (team, name_id or 0)
    return out


def template_calls():
    """[(emevd map, kill flag X0, character X4)] for every 90005300/301 initialisation."""
    reader = H.asm.GetType('SoulsFormats.EMEVD').GetMethod(
        'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([H._str_type]), None)
    calls = []
    for ep in sorted((config.require_err_mod_dir() / 'event').glob('*.emevd.dcx')):
        try:
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_se.emevd')
            SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(ep)).ToArray())
            em = reader.Invoke(None, Array[Object]([tmp]))
            H._safe_unlink(tmp)
        except Exception:
            continue
        name = ep.name.replace('.emevd.dcx', '')
        for ev in em.Events:
            for ins in ev.Instructions:
                if int(ins.Bank) != 2000 or int(ins.ID) not in (0, 6):
                    continue
                args = bytes(ins.ArgData)
                if len(args) < 16 or struct.unpack_from('<I', args, 4)[0] not in TEMPLATES:
                    continue
                flag, char = struct.unpack_from('<II', args, 8)   # unsigned: DLC ids pass 2^31
                calls.append((name, flag, char))
    return calls


def placements(wanted, dummies=None):
    """(msb stem, EntityID) -> placement, for the characters the templates name. EntityIDs found only
    among DummyEnemies (never spawned) go into `dummies` when a set is passed."""
    out = {}
    for msb_path in sorted((config.require_err_mod_dir() / 'map' / 'MapStudio').glob('*.msb.dcx')):
        try:
            msb = H.read_msb(msb_path)
        except Exception:
            continue
        stem = msb_path.name.replace('.msb.dcx', '')
        if dummies is not None:
            dummies.update(int(e.EntityID) for e in msb.Parts.DummyEnemies if int(e.EntityID) in wanted)
        for e in msb.Parts.Enemies:
            if int(getattr(e, 'GameEditionDisable', 0) or 0) == 1:
                continue
            eid = int(e.EntityID)
            if eid not in wanted:
                continue
            out.setdefault((stem, eid), {
                'stem': stem, 'part': str(e.Name), 'model': str(e.ModelName),
                'npc': int(e.NPCParamID), 'x': float(e.Position.X), 'y': float(e.Position.Y),
                'z': float(e.Position.Z)})
    return out


# BloodMsg word 30003 "strong foe" (localized in every language: "сильный враг", "starker Feind",
# "强敌" ...): the label of a strong enemy the name tables do not cover (DLC models such as c6201,
# c5960, c5800). A marker needs SOME text line - its hide/checkmark flag rides on the text.
STRONG_FOE_WORD = 30003


def enemy_label(model, npc, name_id):
    """textId for the enemy's name: its NpcName (+700M), else its enemy-type name - the ERR codex
    (+900M) or, off ERR, the closest blood-message word (+950M); same rules as the loot markers -
    and failing those, the blood-message word "strong foe"."""
    if name_id > 0:
        return name_id + 700000000
    import generate_loot as L
    tid = L.resolve_enemy_tutorial_id(model, npc)
    if tid > 0:
        return tid + 900000000
    if config.PROFILE != 'err':
        word = L.BLOODMSG_WORDS.get(model[:5], 0)
        if word > 0:
            return word + 950000000
    return STRONG_FOE_WORD + 950000000


def main():
    npcs = npc_params()
    calls = template_calls()
    dummies = set()
    places = placements({c for _, _, c in calls}, dummies)
    by_char = {}
    for (stem, eid), p in places.items():
        by_char.setdefault(eid, p)

    skipped = Counter()
    seen = set()
    records = []
    for emevd_map, flag, char in calls:
        # The emevd's own tile first: an EntityID can be reused in tiles that never load together.
        p = places.get((emevd_map, char)) or by_char.get(char)
        if not p:
            skipped['character placed only as a DummyEnemy (never spawns)' if char in dummies
                    else 'character not placed in any MSB'] += 1
            continue
        if not flag:
            skipped['no kill flag (X0 = 0)'] += 1
            continue
        team, name_id = npcs.get(p['npc'], (None, 0))
        if team in H.INVADER_TEAM_TYPES and name_id > 0:
            skipped['named invader - a World - Hostile NPC marker already'] += 1
            continue
        area, gx, gz = H.map_to_area(p['stem'])
        x, z = p['x'], p['z']
        moved = remap_placeholder_xz(p['part'], p['stem'], x, z)
        if moved:
            gx, gz, x, z = moved
        # One enemy per part of one tile, across the tile's variants (_00 / _10 place the same part).
        key = (area, gx, gz, p['part'] if not moved else p['part'].split('-', 1)[1])
        if key in seen:
            skipped['same enemy in a tile variant'] += 1
            continue
        seen.add(key)
        records.append({'area': area, 'gx': gx, 'gz': gz, 'x': x, 'y': p['y'], 'z': z, 'flag': flag,
                        'model': p['model'], 'label': enemy_label(p['model'], p['npc'], name_id),
                        'named': name_id > 0,
                        'map': f'm{area:02d}_{gx:02d}_{gz:02d}_00'})
    records.sort(key=lambda r: (r['area'], r['gx'], r['gz'], r['x'], r['z']))

    icon = __import__('icon_registry').iconid_for_name(CATEGORY)
    row_id = __import__('row_id_registry').base(CATEGORY)
    sink = rowsink.RowSink()
    for r in records:
        fields = {'iconId': icon, get_disp_mask(r["area"], r["gx"]): 1, 'areaNo': r['area']}
        if r['area'] in OVERWORLD_AREAS or r['area'] in DLC_AREAS or r['gx'] > 0:
            fields['gridXNo'] = r['gx']
            fields['gridZNo'] = r['gz']
        fields['posX'] = r['x']
        if r['y'] != 0.0:
            fields['posY'] = r['y']
        fields['posZ'] = r['z']
        if r['label']:
            fields['textId1'] = r['label']
            fields['textDisableFlagId1'] = r['flag']
        loc_id = resolve_location_id_at(r['map'], r['x'], r['y'], r['z'])
        if loc_id > 0:
            fields['textId2'] = loc_id
            fields['textDisableFlagId2'] = r['flag']
        # Checkmark vs hide on the kill flag - the DLL picks per hide_killed_bosses, as for bosses.
        fields['clearedEventFlagId'] = r['flag']
        fields['selectMinZoomStep'] = 1
        sink.add(row_id, fields)
        row_id += 1

    out = sink.write(OUT_DIR / f'{CATEGORY}.rows')
    models = Counter(r['model'] for r in records)
    print(f'{len(calls)} strong-enemy template calls -> {len(records)} markers '
          f'({sum(r["named"] for r in records)} named, {sum(1 for r in records if not r["label"])} unlabeled)')
    for why, n in skipped.most_common():
        print(f'  skipped {n}: {why}')
    print('  by model:', ', '.join(f'{m} x{n}' for m, n in models.most_common(12)))
    print(f'Written {out.name}')


if __name__ == '__main__':
    main()
