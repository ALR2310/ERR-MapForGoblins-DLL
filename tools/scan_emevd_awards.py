#!/usr/bin/env python3
"""
Scan all EMEVD files for item-award references.
For each ItemLotID found in instruction/initializer arg bytes, record
any nearby 4-byte values that match MSB EntityIDs - these are likely
spawn positions for scripted treasure.

Output: data/emevd_lot_mapping.json
  { lot_id (str): [{map, entity_id, spawn_x/y/z, source: 'inst'|'init', event_id}] }
"""
import sys, io, os, tempfile, struct, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from collections import defaultdict
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


asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_emevd(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_emevd.tmp')
    SysFile.WriteAllBytes(tmp, data)
    e = _emevd_read.Invoke(None, Array[Object]([tmp]))
    _safe_unlink(tmp)
    return e


# ── Load lot IDs from regulation.bin ──
def load_lot_ids():
    """Return (map_lots, enemy_lots) sets of ItemLotParam IDs."""
    reg_path = config.require_err_mod_dir() / 'regulation.bin'
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    map_lots = set()
    enemy_lots = set()
    for f in bnd.Files:
        fn = str(f.Name)
        if 'ItemLotParam_map' in fn:
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_p.tmp')
            # f.Bytes is Memory<byte>; need ToArray()
            arr = f.Bytes.ToArray() if hasattr(f.Bytes, 'ToArray') else f.Bytes
            SysFile.WriteAllBytes(tmp, arr)
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            _safe_unlink(tmp)
            for row in p.Rows:
                map_lots.add(int(row.ID))
        elif 'ItemLotParam_enemy' in fn:
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_p.tmp')
            # f.Bytes is Memory<byte>; need ToArray()
            arr = f.Bytes.ToArray() if hasattr(f.Bytes, 'ToArray') else f.Bytes
            SysFile.WriteAllBytes(tmp, arr)
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            _safe_unlink(tmp)
            for row in p.Rows:
                enemy_lots.add(int(row.ID))
    return map_lots, enemy_lots


# Instructions whose args carry ids but which award nothing, so a lot-looking value in them is a
# coincidence. Kept as an explicit list rather than a rule about banks: this scan is deliberately
# id-shaped rather than instruction-aware (a treasure lot usually arrives as a generic PARAMETER of an
# InitializeCommonEvent template, so requiring a declared "item lot" argument would throw the real
# mechanism away), and the price of that is exactly this - a few instructions have to be named.
# Signatures read from DarkScript's er-common.emedf.json, which is what says these carry no lot at all:
#   2009[00] Register Ladder(Disable Top Event Flag ID, Disable Bottom Event Flag ID, Entity ID)
#   2009[03] Register Bonfire(Event Flag ID, Entity ID, Reaction Distance, Reaction Angle,
#                             Set Standard Kindling Level, Enemy Deactivation Distance)
# Every "lot" this scan used to find in them was a flag or an entity id that happens to equal a real
# ItemLotParam_map id. Measured on the err data: 65 lots lost a candidate record, and it changed 7 of
# 9201 markers - the phantom Golden Rune [1] went away, and six others moved off a BONFIRE
# (AEG099_060, entity ...1950) onto the pickup asset their treasure template actually names
# (AEG099_090), each still inside its own map.
# (2009[01] is not an instruction in Elden Ring - do not add it back.)
NON_AWARD_INSTRUCTIONS = {
    (2009, 0),
    (2009, 3),
}


# The two instructions that actually hand the player an item lot, per DarkScript's
# er-common.emedf.json:  2003[04] Award Item Lot(Item Lot ID)  and
# 2003[36] Award Items (Including Clients)(Item Lot ID). The lot is arg 0 of both.
AWARD_INSTRS = {(2003, 4), (2003, 36)}


def award_events_of(emevd):
    """event id -> (param byte offsets feeding an award, lots awarded as a CONSTANT).

    Both halves are needed and the second was learned the hard way. Night's Cavalry in
    m60_48_51 is initialized with `InitializeCommonEvent(0, 1048512800, 1048510800)` - the called
    event lives in the MAP's own file, not in common_func, and it awards its lot as a literal
    (`AwardItems(IncludingClients)(1048510700)`), not through a parameter. A whitelist built only
    from common_func, or one that only understood parameter-fed lots, dropped that marker.

    So: a caller's blob yields the parameter-fed lots at their known offsets PLUS every constant
    lot the called event awards, and the entities still come from the blob - which is what puts
    the marker on the enemy.
    """
    out = {}
    for evt in emevd.Events:
        instrs = list(evt.Instructions)
        award_idx = {i for i, ins in enumerate(instrs)
                     if (int(ins.Bank), int(ins.ID)) in AWARD_INSTRS}
        if not award_idx:
            continue
        offsets = set()
        parametrised = set()
        for pr in (list(evt.Parameters) if hasattr(evt, 'Parameters') else []):
            i = int(pr.InstructionIndex)
            if i not in award_idx:
                continue
            # the lot is arg 0 of both award instructions -> target bytes 0..3
            if int(pr.TargetStartByte) == 0 and int(pr.ByteCount) >= 4:
                offsets.add(int(pr.SourceStartByte))
                parametrised.add(i)
        fixed = set()
        for i in award_idx - parametrised:
            ab = bytes(instrs[i].ArgData) if instrs[i].ArgData else b''
            if len(ab) >= 4:
                fixed.add(struct.unpack_from('<I', ab, 0)[0])
        out[int(evt.ID)] = (sorted(offsets), sorted(fixed))
    return out


def load_award_common_events(event_dir):
    """common event id -> the event-arg byte offsets its awarded lot comes from.

    WHY THIS EXISTS. InitializeCommonEvent (2000[06]) is where the real treasure templates pass
    their lot, so it cannot be added to NON_AWARD_INSTRUCTIONS - measured, it is the source of
    427-492 of the 466-605 lot candidates on every profile, roughly 90%. But its parameters are
    whatever the CALLED event wants them to be, and blind-scanning them for lot-looking values is
    how the Mohgwyn Palace lift grew a Golden Rune [1]: common event 90005500 is
    "2フロア移動エレベータ" (two-floor elevator), its first parameter is the lift's event FLAG, and
    12050510 happens to be both that flag and a real ItemLotParam_map row.

    So ask the data instead of guessing. A common event awards a lot iff its body runs one of
    AWARD_INSTRS, and when the lot arrives through an EMEVD Parameter that record also names the
    event-arg byte it is read from - which turns the blind blob scan into an exact read. Measured
    on Convergence 3.x: 37 of 752 common events award a lot at all.

    Events that award a FIXED lot (no parameter) return no offsets and are treated as
    non-awarding here: their lot is not in the caller's arg blob to begin with, so scanning that
    blob for it could only ever produce a coincidence.
    """
    out = {}
    for name in ('common_func', 'common'):
        p = event_dir / (name + '.emevd.dcx')
        if not p.exists():
            continue
        try:
            emevd = load_emevd(p)
        except Exception:
            continue
        out.update(award_events_of(emevd))
    return out


def scan_arg_blob(arg_bytes, lot_set, entity_set, skip_offsets=()):
    """Find all (lot_id, entity_id) co-occurrences in one arg blob.

    Returns (lots, ents). Dungeon ENTITY ids share the ItemLot numbering space
    (both 12NNxxxx), so a value that is a known MSB entity id is treated as an
    entity reference, NEVER as a lot - otherwise e.g. Clayman entity 12070250
    becomes a phantom "Golden Rune" treasure at the enemy's feet.
    skip_offsets: arg byte-offsets that hold known NON-lot ids (e.g. the
    event-id arg of InitializeEvent - 12020700 there is an event id, not a lot).
    """
    vals = []
    # Scan every 4-byte aligned (step by 4 to match common arg layout)
    # But also try unaligned in case args have different offset
    for i in range(0, len(arg_bytes) - 3):
        vals.append((i, struct.unpack_from('<I', arg_bytes, i)[0]))
    lots = [v for i, v in vals
            if v in lot_set and v not in entity_set and i not in skip_offsets]
    ents = [v for i, v in vals if v in entity_set]
    return lots, ents


def main():
    print('Loading MSB entity index...')
    ei_path = config.DATA_DIR / 'msb_entity_index.json'
    entity_idx = json.load(open(ei_path, encoding='utf-8'))
    entity_set = set(int(k) for k in entity_idx.keys())
    print(f'  {len(entity_set)} entities')

    print('Loading ItemLot IDs from regulation...')
    map_lots, enemy_lots = load_lot_ids()
    lot_set = map_lots | enemy_lots
    print(f'  {len(map_lots)} map lots, {len(enemy_lots)} enemy lots, {len(lot_set)} total')

    event_dir = config.require_err_mod_dir() / 'event'
    emevd_files = sorted(event_dir.glob('*.emevd.dcx'))

    print('Reading which common events award a lot...')
    award_common_events = load_award_common_events(event_dir)
    _offs = sorted({o for offs, _ in award_common_events.values() for o in offs})
    _fixed = sum(1 for offs, fx in award_common_events.values() if fx)
    print(f'  {len(award_common_events)} awarding common event(s); lot read from arg byte(s) '
          f'{_offs}; {_fixed} of them award a constant lot')

    print(f'Scanning {len(emevd_files)} EMEVDs...')

    # lot -> list of candidate mappings
    mapping = defaultdict(list)
    non_award_skipped = defaultdict(int)  # (bank, idx) -> how many were passed over
    common_event_skipped = defaultdict(int)  # called common event id -> times passed over

    for idx, p in enumerate(emevd_files):
        if (idx + 1) % 100 == 0:
            print(f'  [{idx+1}/{len(emevd_files)}]')
        try:
            emevd = load_emevd(p)
        except Exception as e:
            continue

        map_name = p.name.replace('.emevd.dcx', '')
        # A map's own events are valid InitializeCommonEvent targets too (Night's Cavalry is
        # initialized with the local 1048512800), so they join the lookup for this file only.
        local_award_events = award_events_of(emevd)

        # Collect all arg blobs from every instruction in every event
        for evt in emevd.Events:
            event_id = int(evt.ID)
            for inst in evt.Instructions:
                ab = bytes(inst.ArgData) if inst.ArgData else b''
                if not ab: continue
                # InitializeEvent (2000[0]) / InitializeCommonEvent (2000[6]):
                # args = [slot, event_id, params...] - the event-id at byte
                # offset 4 is never an item lot (dungeon event ids collide
                # with the lot numbering, e.g. 12020700).
                bank, iid = int(inst.Bank), int(inst.ID)
                # An instruction that AWARDS nothing cannot tell us where a lot is, however many of
                # its arg bytes happen to match a lot id. RegisterLadder is the proven case: it takes
                # (2009[0]) three ids, one of them numerically equal to map lot 35000580, and this scan
                # read that as "the lot is at the ladder" - which put a Golden Rune [1] marker on the
                # Leyndell map that nothing in the game ever awards (the lot is cut content: its
                # getItemFlag appears in no EMEVD at all). See scratch/keep/notes/bugs_2026-07-29_*.
                if (bank, iid) in NON_AWARD_INSTRUCTIONS:
                    non_award_skipped[(bank, iid)] += 1
                    continue
                if bank == 2000 and iid == 6:
                    # InitializeCommonEvent(slot, event_id, params...). The params mean whatever
                    # the CALLED event decides, so do not sweep them. Resolve the callee - it may
                    # live in THIS map's file as well as in common_func/common - and take only the
                    # lots it can actually hand out: the ones it reads from a parameter, at the
                    # byte it reads them from, plus the ones it awards as a constant.
                    # See award_events_of() for why both halves are needed.
                    if len(ab) < 8:
                        continue
                    called = struct.unpack_from('<I', ab, 4)[0]
                    info = local_award_events.get(called) or award_common_events.get(called)
                    if not info:
                        common_event_skipped[called] += 1
                        continue
                    param_offs, fixed_lots = info
                    lots = list(fixed_lots)
                    for off in param_offs:
                        pos = 8 + off  # the caller's blob holds params after slot + event id
                        if pos + 4 <= len(ab):
                            v = struct.unpack_from('<I', ab, pos)[0]
                            if v in lot_set and v not in entity_set:
                                lots.append(v)
                    if not lots:
                        common_event_skipped[called] += 1
                        continue
                    # Entities still come from the whole blob - that is what gives the position.
                    _, ents = scan_arg_blob(ab, lot_set, entity_set, (4,))
                else:
                    skip = (4,) if (bank == 2000 and iid == 0) else ()
                    lots, ents = scan_arg_blob(ab, lot_set, entity_set, skip)
                # Filter trivial lot IDs (0-9999) that collide with common small integers
                lots = [l for l in lots if l >= 10000]
                if lots and ents:
                    for lot in lots:
                        for ent in ents:
                            mapping[lot].append({
                                'map_emevd': map_name,
                                'event_id': event_id,
                                'entity_id': ent,
                                'bank': int(inst.Bank),
                                'idx': int(inst.ID),
                            })

    if common_event_skipped:
        top = sorted(common_event_skipped.items(), key=lambda kv: -kv[1])[:6]
        print(f'\nInitializeCommonEvent: passed over {sum(common_event_skipped.values())} call(s) '
              f'to {len(common_event_skipped)} common event(s) that award no item lot')
        print('  most frequent: ' + ', '.join(f'{eid} x{n}' for eid, n in top))

    print(f'\nFound {len(mapping)} lots with entity candidates')
    print(f'Total (lot, entity) records: {sum(len(v) for v in mapping.values())}')

    # Attach entity positions, filter noise
    enriched = {}
    for lot, recs in mapping.items():
        # Deduplicate by entity_id
        seen = set()
        uniq = []
        for r in recs:
            if r['entity_id'] in seen: continue
            seen.add(r['entity_id'])
            # Skip trivial IDs (likely counts/flags mistaken for entities)
            if r['entity_id'] < 10000: continue
            ent_info = entity_idx.get(str(r['entity_id']))
            if not ent_info: continue
            # Require MSB map prefix matches EMEVD map (same tile) to reduce cross-map collisions
            if ent_info['map'] != r['map_emevd']:
                continue
            uniq.append({**r, **{
                'msb_map': ent_info['map'],
                'x': ent_info['x'], 'y': ent_info['y'], 'z': ent_info['z'],
                'model': ent_info['model'],
                'kind': ent_info['kind'],
            }})
        if uniq:
            enriched[str(lot)] = uniq

    out_path = config.DATA_DIR / 'emevd_lot_mapping.json'
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(enriched, f, indent=1)
    print(f'Saved {len(enriched)} resolved lots to {out_path}')


if __name__ == '__main__':
    main()
