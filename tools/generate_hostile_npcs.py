#!/usr/bin/env python3
"""
Generate World - Hostile NPC.rows - fully auto-discovered.

Strategy:
  1. From NpcParam (regulation.bin), collect NPC IDs with teamType in
     {24, 27} - both are hostile-invader variants used in vanilla and ERR.
  2. Scan all MSBs for Enemies whose NPCParamID is in that set AND
     whose EntityID > 0 (placed, not script-spawned dummies).
  3. Cross-reference items_database (built by extract_all_items.py) by
     (map, partName) to attach:
       - defeatFlag (from template 90005792 X0_4) for clearedEventFlagId
         and textDisableFlagId (hide-on-kill behaviour)
       - main drop item for textId1 fallback when NPC has no NpcName entry
  4. Each matched enemy becomes a marker labeled with the NPC's name
     (NpcParam.nameId + 700000000 → NpcName FMG via runtime patcher).
"""
import sys, io, os, tempfile, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
import rowsink
from collections import defaultdict
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats


def _safe_unlink(path):
    try:
        os.unlink(path)
    except PermissionError:
        pass


from npcname_known import npcname_resolvable
from marker_common import (OUT_DIR, DATA_DIR, UNDERGROUND_AREAS, DLC_AREAS,
                             OVERWORLD_AREAS, get_disp_mask, resolve_location_id_at)

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_paramdefs():
    defs = {}
    for x in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
            if pd and pd.ParamType:
                defs[str(pd.ParamType)] = pd
        except Exception:
            pass
    return defs


def read_param(bnd, name, paramdefs):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_hnp_p.tmp')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            _safe_unlink(tmp)
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in paramdefs:
                p.ApplyParamdef(paramdefs[pt])
            return p
    return None


def read_msb(path):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_hnp_m.tmp')
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(path)).ToArray())
    m = _msbe_read.Invoke(None, Array[Object]([tmp]))
    _safe_unlink(tmp)
    return m


def map_to_area(map_name):
    parts = map_name.replace('m', '').split('_')
    try: return int(parts[0]), int(parts[1]), int(parts[2])
    except: return 0, 0, 0


# Both teams used by hostile NPC invaders in ER + ERR:
#   24 = vanilla invader (Edgar, Vyke, ...)
#   27 = ERR / DLC hostile-NPC variant (Millicent, ...)
INVADER_TEAM_TYPES = {24, 27}


def main():
    print('Loading NpcParam (invader filter, NPC names)...')
    pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(
        str(config.require_err_mod_dir() / 'regulation.bin'))
    np = read_param(bnd, 'NpcParam', pds)

    # Map NpcParam ID -> (teamType, nameId)
    npc_info = {}
    invader_ids = set()
    for row in np.Rows:
        team = name_id = None
        for cell in row.Cells:
            n = str(cell.Def.InternalName)
            if n == 'teamType':
                try: team = int(str(cell.Value))
                except: pass
            elif n == 'nameId':
                try: name_id = int(str(cell.Value))
                except: pass
        npc_info[int(row.ID)] = (team, name_id)
        if team in INVADER_TEAM_TYPES:
            invader_ids.add(int(row.ID))
    print(f'  {len(invader_ids)} NpcParam IDs with teamType in {INVADER_TEAM_TYPES}')

    # Index items_database by (map, partName) → list of records (for drops + defeat flag)
    items_db_path = DATA_DIR / 'items_database.json'
    db_by_part = defaultdict(list)
    db_by_npc = defaultdict(list)   # (map, npcParamId) - quest invaders' drop
                                    # records often sit under a different part
    if items_db_path.exists():
        with open(items_db_path, encoding='utf-8') as f:
            for entry in json.load(f):
                if not isinstance(entry, dict): continue
                key = (entry.get('map'), entry.get('partName'))
                if key[0] and key[1]:
                    db_by_part[key].append(entry)
                npc_id = int(entry.get('npcParamId', 0) or 0)
                if key[0] and npc_id > 0:
                    db_by_npc[(key[0], npc_id)].append(entry)

    # Curated quest-invader overrides (committed repo data, profile-independent;
    # decompile-verified flags/positions for invaders outside the 90005792
    # template - e.g. the Knight of the Great Jar trio).
    overrides_path = config.INPUTS_DIR / 'quest_invader_overrides.json'
    quest_overrides = {}
    if overrides_path.exists():
        with open(overrides_path, encoding='utf-8') as f:
            quest_overrides = {int(k): v for k, v in json.load(f).items()
                               if not k.startswith('_')}

    # entity -> defeat flag, harvested from EMEVD initializations of common
    # event 90005792 "[Common] Hostile NPC_Defeated" (X0_4 = defeat flag set
    # on death, X12_4 = character entity, X16_4 = award lot). Unlike the
    # items_database route this also covers invaders WITHOUT a drop lot
    # (the event awards nothing but still sets the flag).
    import struct as _struct
    _emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
        BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)
    ent_defeat_flag = {}
    event_dir = config.require_err_mod_dir() / 'event'
    for ep in sorted(event_dir.glob('*.emevd.dcx')):
        try:
            data = SoulsFormats.DCX.Decompress(str(ep)).ToArray()
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_hnp.emevd')
            SysFile.WriteAllBytes(tmp, data)
            em = _emevd_read.Invoke(None, Array[Object]([tmp]))
        except Exception:
            continue
        for ev in em.Events:
            for ins in ev.Instructions:
                if int(ins.Bank) != 2000 or int(ins.ID) not in (0, 6):
                    continue
                args = bytes(ins.ArgData)
                if len(args) < 8 + 16:
                    continue
                if _struct.unpack_from('<I', args, 4)[0] != 90005792:
                    continue
                blob = args[8:]
                flag = _struct.unpack_from('<I', blob, 0)[0]   # X0_4
                ent = _struct.unpack_from('<I', blob, 12)[0]   # X12_4
                if ent > 0 and flag > 0:
                    ent_defeat_flag.setdefault(ent, flag)
    print(f'  {len(ent_defeat_flag)} entity->defeat-flag pairs from 90005792 inits')

    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    records = []
    print(f'Scanning MSBs for invader placements...')
    for msb_path in sorted(msb_dir.glob('*.msb.dcx')):
        try: msb = read_msb(msb_path)
        except Exception: continue
        map_name = msb_path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            if int(getattr(e, 'GameEditionDisable', 0) or 0) == 1:
                continue
            npc = int(getattr(e, 'NPCParamID', 0) or 0)
            if npc not in invader_ids: continue
            entity = int(getattr(e, 'EntityID', 0) or 0)
            if entity <= 0:
                continue  # runtime-only dummy
            # Filter out mob enemies that happen to share teamType 24/27
            # (Bloodfiends c4280, dungeon Battlemages c4300_*_28 variants,
            # scarabs c4190/91/92, etc). Real NPC invaders all have a
            # named NpcName entry - `nameId > 0` is the canonical signal.
            _team, name_id = npc_info.get(npc, (None, None))
            if not name_id or name_id <= 0:
                continue
            part_name = str(e.Name)
            pos = e.Position
            area, gx, gz = map_to_area(map_name)

            # Lookup drops + defeat flag from items_database. Preferred: the
            # explicit invader-defeat flag (ERR template 90005792). Fallback:
            # the drop lot's acquisition flag (eventFlag) - the lot is awarded
            # the moment the invader dies, so its flag doubles as a kill flag.
            # This is the only per-invader flag available in vanilla, and it
            # also covers the ERR invaders the template scan misses.
            db_entries = db_by_part.get((map_name, part_name), [])
            defeat_flag = ent_defeat_flag.get(entity, 0)
            if defeat_flag <= 0:
                for db_e in db_entries:
                    if db_e.get('defeatFlag', 0) > 0:
                        defeat_flag = int(db_e['defeatFlag'])
                        break
            if defeat_flag <= 0:
                for db_e in db_entries:
                    ef = int(db_e.get('eventFlag', 0) or 0)
                    if ef > 0:
                        defeat_flag = ef
                        break
            if defeat_flag <= 0:
                # Quest invaders: drop record may sit under a different part
                # name - match by (map, npcParamId) and use the drop's
                # acquisition flag (set when the kill awards the lot).
                for db_e in db_by_npc.get((map_name, npc), []):
                    ef = int(db_e.get('defeatFlag', 0) or 0) or int(db_e.get('eventFlag', 0) or 0)
                    if ef > 0:
                        defeat_flag = ef
                        break

            # Curated override: flag and/or marker position (duel-sign spot
            # instead of the parked character part).
            pos_x, pos_y, pos_z = float(pos.X), float(pos.Y), float(pos.Z)
            ov = quest_overrides.get(entity)
            if ov:
                if ov.get('flag'):
                    defeat_flag = int(ov['flag'])
                if 'x' in ov:
                    pos_x, pos_y, pos_z = float(ov['x']), float(ov['y']), float(ov['z'])

            records.append({
                'entity': entity, 'npc': npc, 'nameId': name_id,
                'map': map_name, 'area': area, 'gx': gx, 'gz': gz,
                'x': pos_x, 'y': pos_y, 'z': pos_z,
                'model': str(e.ModelName) if hasattr(e, 'ModelName') else '',
                'defeatFlag': defeat_flag, 'partName': part_name,
            })

    # Dedup per (tile, rounded_coords) and per (tile, entity) - multiple invader variants
    # stacked at the same EMEVD trigger spot would otherwise produce overlapping markers.
    # Keep the first (which is usually the canonical placement).
    #
    # "Tile", not the MSB name: a tile with a story variant (m61_44_46_00 and _10, the
    # Sealing Tree burn) places the same NPC, with the same entity and defeat flag, in both
    # MSBs. Keyed by MSB name they came out as two markers - Hornsent, Moore and Queelign each
    # stood twice, an offline spiral apart, and both copies answered the same kill.
    def tile_of(map_name):
        parts = map_name.split('_')
        return '_'.join(parts[:3]) if len(parts) >= 4 else map_name

    seen = set()
    uniq = []
    for r in records:
        tile = tile_of(r['map'])
        keys = [(tile, round(r['x'], 1), round(r['z'], 1))]
        if r['entity'] > 0:
            keys.append((tile, 'entity', r['entity']))
        if any(k in seen for k in keys): continue
        seen.update(keys)
        uniq.append(r)
    records = uniq
    records.sort(key=lambda r: (r['area'], r['gx'], r['gz'], r['x'], r['z']))

    sink = rowsink.RowSink()
    row_id = __import__("row_id_registry").base("World - Hostile NPC")  # z-order slot; see row_id_registry
    named = 0
    flagged = 0
    unnamed_ids = set()  # NpcParam.nameId values no NpcName FMG resolves (generic word used)
    for r in records:
        disp = get_disp_mask(r['area'])
        fields = {'iconId': __import__("icon_registry").iconid("hostile_npc"), disp: 1,
                  'areaNo': r['area']}
        if r['area'] in OVERWORLD_AREAS or r['area'] in DLC_AREAS or r['gx'] > 0:
            fields['gridXNo'] = r['gx']
            fields['gridZNo'] = r['gz']
        fields['posX'] = r['x']
        if r['y'] != 0.0:
            fields['posY'] = r['y']
        fields['posZ'] = r['z']

        # textId1: NPC name via NpcName FMG (resolved at runtime by
        # goblin_messages.cpp using the +700000000 offset convention).
        # Only an id that HAS a string: NpcParam.nameId can point at nothing (Golden Age
        # 3.6.8, id 135700 on three rows) and a marker whose only text resolves to nothing
        # is cleared by the DLL's sanitizer and never drawn. Such a row gets the generic
        # "strong foe" word instead, which every language has.
        if r['nameId'] > 0 and npcname_resolvable(r['nameId']):
            fields['textId1'] = r['nameId'] + 700000000
            named += 1
            # textDisableFlagId1: hide NPC name once defeated
            if r['defeatFlag'] > 0:
                fields['textDisableFlagId1'] = r['defeatFlag']
        elif r['nameId'] > 0:
            unnamed_ids.add(r['nameId'])
            fields['textId1'] = 950000000 + 30003
            # textDisableFlagId1: hide NPC name once defeated
            if r['defeatFlag'] > 0:
                fields['textDisableFlagId1'] = r['defeatFlag']

        # textId2: location subtitle (interior maps only)
        loc_id = resolve_location_id_at(r['map'], r['x'], r['y'], r['z'])
        if loc_id > 0:
            fields['textId2'] = loc_id
            if r['defeatFlag'] > 0:
                fields['textDisableFlagId2'] = r['defeatFlag']

        # clearedEventFlagId: shows green checkmark when defeated. C++
        # config (hideKilledBosses) chooses between checkmark and full hide.
        if r['defeatFlag'] > 0:
            fields['clearedEventFlagId'] = r['defeatFlag']
            flagged += 1

        fields['selectMinZoomStep'] = 1
        sink.add(row_id, fields)
        row_id += 1

    out = sink.write(OUT_DIR / 'World - Hostile NPC.rows')
    print(f'Written {len(records)} hostile NPC markers ({named} with name, {flagged} with defeat flag) to {out.name}')
    if unnamed_ids:
        print(f'  {len(unnamed_ids)} NpcName id(s) with no string in this profile, labelled "strong foe": {sorted(unnamed_ids)}')


if __name__ == '__main__':
    main()
