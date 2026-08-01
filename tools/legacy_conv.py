"""WorldMapLegacyConvParam, resolved to one hop per key.

The param says how a dungeon area's local coordinates land on another area's grid, and it CHAINS: the
Subterranean Shunning-Grounds reach the overworld through Leyndell (35 -> 11 -> 60) and Deeproot Depths
through both (12_03 -> 35 -> 11 -> 60). Two places used to read this param, and both kept only the rows
whose destination was already an overworld tile - which silently dropped exactly the chained ones. In
the vanilla bake that was 253 marker rows (100 in m35, 139 in m12_03, 14 more with no conv row at all),
absent from every report with no warning: see scratch/bugs_2026-07-29_dump_conv_and_phantom_rune.md.

One hop is

    world = dst_grid * 256 + dst_pos + (local_pos - src_pos)

so hopping again only carries the offset: the position handed to the next row is this row's dst_pos, and
that row's own src_pos comes off it. Composing this way keeps the result a single lookup - callers need
no loop, and the C++ side stays a flat table.

Checked against the chain resolved by hand in that report: (35, 0) comes out as tile 45/52 with an
effective offset of (+16.679, -120.564), and marker row 5200160 then lands 5.7 units from the beacon the
report dumped - the same numbers.
"""
import json
from pathlib import Path

OVERWORLD_AREAS = (60, 61)
MAX_HOPS = 4  # longest real chain is 12_03 -> 35 -> 11 -> 60; the cap only stops a cycle


def load_raw(conv_json):
    """Every row keyed the way a lookup is done - (srcAreaNo, srcGridXNo), first row wins."""
    rows = json.load(open(Path(conv_json), encoding="utf-8"))
    raw = {}
    for r in rows:
        if not isinstance(r, dict):
            continue
        key = (int(r.get("srcAreaNo", 0)), int(r.get("srcGridXNo", 0)))
        if key in raw:
            continue
        raw[key] = {
            "src_area": key[0], "src_gx": key[1],
            "src_pos_x": float(r.get("srcPosX", 0)),
            "src_pos_z": float(r.get("srcPosZ", 0)),
            "dst_area": int(r.get("dstAreaNo", 0)),
            "dst_gx": int(r.get("dstGridXNo", 0)),
            "dst_gz": int(r.get("dstGridZNo", 0)),
            "dst_pos_x": float(r.get("dstPosX", 0)),
            "dst_pos_z": float(r.get("dstPosZ", 0)),
        }
    return raw


def resolve(conv_json):
    """-> (entries, chains, unresolved).

    entries: {(src_area, src_gx): entry} where every entry's dst_area is an overworld tile.
    chains:  the multi-hop ones, as (key, "35 -> 11 -> 60", dst_gx, dst_gz, off_x, off_z) for logging.
    unresolved: [(key, "37 -> 70")] - keys that never reach one, so their markers stay unplaced. Report
    them; they are the only honest thing to say about a marker that cannot be positioned.
    """
    raw = load_raw(conv_json)
    entries, chains, unresolved = {}, [], []
    for key, e in sorted(raw.items()):
        dst_area, dst_gx, dst_gz = e["dst_area"], e["dst_gx"], e["dst_gz"]
        dst_x, dst_z = e["dst_pos_x"], e["dst_pos_z"]
        hops = [dst_area]
        while dst_area not in OVERWORLD_AREAS and len(hops) <= MAX_HOPS:
            nxt = raw.get((dst_area, dst_gx))
            if not nxt:
                break
            dst_x = nxt["dst_pos_x"] + (dst_x - nxt["src_pos_x"])
            dst_z = nxt["dst_pos_z"] + (dst_z - nxt["src_pos_z"])
            dst_area, dst_gx, dst_gz = nxt["dst_area"], nxt["dst_gx"], nxt["dst_gz"]
            hops.append(dst_area)
        chain = " -> ".join(str(h) for h in hops)
        if dst_area not in OVERWORLD_AREAS:
            unresolved.append((key, chain))
            continue
        entries[key] = dict(e, dst_area=dst_area, dst_gx=dst_gx, dst_gz=dst_gz,
                            dst_pos_x=dst_x, dst_pos_z=dst_z)
        if len(hops) > 1:
            chains.append((key, chain, dst_gx, dst_gz,
                           dst_x - e["src_pos_x"], dst_z - e["src_pos_z"]))
    return entries, chains, unresolved


def report(chains, unresolved, prefix="  legacy conv:"):
    """Print what was composed and what could not be - exact keys, never a count on its own."""
    for key, chain in unresolved:
        print(f"{prefix} (area {key[0]}, gx {key[1]}) does not reach an overworld tile ({chain}) - "
              f"markers there stay unplaced")
    for key, chain, gx, gz, ox, oz in chains:
        print(f"{prefix} (area {key[0]}, gx {key[1]}) resolved through {chain} -> tile {gx}/{gz}, "
              f"effective offset ({ox:+.3f}, {oz:+.3f})")
