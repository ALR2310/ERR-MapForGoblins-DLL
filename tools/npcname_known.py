"""Which NpcName FMG ids can actually be resolved at runtime for the active profile.

A marker whose textId points at an NpcName id with no string behind it comes out with no
text at all, and the engine draws no icon for a text-less marker: the DLL's sanitizer clears
such ids at startup, so the marker silently disappears. NpcParam.nameId is not a promise that
the string exists - Golden Age 3.6.8 has three NpcParam rows naming id 135700, which no
NpcName FMG (the overhaul's or vanilla's) carries; two hostile-NPC markers and three drop
subtitles pointed at it.

The set comes from the same dump the DLL's English fallback is baked from
(data/<profile>/english_fallback.json, keyed 700000000 + id), which lists every NpcName the
overhaul's engus files have. The ERR profile has no such dump - it uses the ERR FMG at
runtime - so it is checked against data/npc_name_text_map.json, the ERR NpcName dump.
"""
import json
import config

_KNOWN = None


def npcname_known():
    """Set of NpcName FMG ids (un-offset) a marker of this profile may reference."""
    global _KNOWN
    if _KNOWN is not None:
        return _KNOWN
    ids = set()
    fb = config.DATA_DIR / 'english_fallback.json'
    if config.PROFILE != 'err' and fb.exists():
        with open(fb, encoding='utf-8') as f:
            for k in json.load(f):
                k = int(k)
                if 700000000 <= k < 800000000 or 1600000000 <= k < 1700000000:
                    ids.add(k - 700000000)
    if not ids:
        tm = config.PROJECT_DIR / 'data' / 'npc_name_text_map.json'
        if tm.exists():
            with open(tm, encoding='utf-8') as f:
                ids = {int(k) for k in json.load(f)}
    _KNOWN = ids
    return _KNOWN


def npcname_resolvable(name_id):
    """True when the id has a string, or when nothing is known (never drop on missing data)."""
    known = npcname_known()
    return not known or int(name_id) in known
