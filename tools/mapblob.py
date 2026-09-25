"""Pack the map table into the compressed blob the DLL expands at startup.

Replaces 3.34 MB of generated C++ brace initialisers per profile (which MSVC parsed on every build
of all nine) and 2.14 MB of .rdata in the shipped binary. The same data packs to ~40 bytes a row
and deflates to around a tenth of that, shipped as a byte array the way the icon tags already are.

THE LAYOUT LIVES IN TWO PLACES and they have to agree: this file and src/goblin_map_blob.cpp. The
version word is the guard.

  magic      'MFGD'                      4
  version    u16                         2
  count      u32                         4
  strtab_len u32                         4
  strtab     NUL-separated names         strtab_len   (index 0 is the empty name = nullptr)
  count x record:
    row_id u32, mask u32,
    category u8, lot_type u8, lot_aggregate u8, state_show u8,
    geom_slot i16, name_suffix i16, name_index u16, pad u16,
    lot_id u32, real_posX f32, real_posZ f32, state_flag u32,
    then one 4-byte value per set mask bit, in FIELD_ORDER: f32 for a position, u32 otherwise.

Version 3 added the world-state rule (state_show in the old pad byte, state_flag after real_posZ):
the marker is shown only while event flag state_flag is ON (1) or OFF (2); 0 = no rule.
"""
import re
import struct
import zlib
from pathlib import Path

import rowsink

MAGIC = b"MFGD"
VERSION = 3
# The record head before the per-field values. src/goblin_map_blob.cpp reads the same bytes.
RECORD_HEAD = "<IIBBBBhhHHIffI"
RECORD_HEAD_SIZE = struct.calcsize(RECORD_HEAD)
# goblin::generated::StateShow (src/goblin_map_data.hpp): the values the DLL compares against.
STATE_SHOW = {"always": 0, "while_on": 1, "while_off": 2}
# The packer and the DLL walk the mask in this order; it is rowsink's, so a field added there is
# added here by construction rather than by remembering to.
FIELD_ORDER = rowsink.FIELDS
FLOAT_FIELDS = rowsink.FLOAT_FIELDS


def category_index(header_path):
    """Category name -> its value in the C++ enum, read from the header so the two cannot drift.

    Comments are stripped FIRST. The enum carries multi-line // notes between its members, and
    splitting the raw text on commas turned one of those into an enumerator - which then shifted
    every category after it by one and silently mislabelled several thousand markers.
    """
    return enum_index(header_path, "Category")


# STATE_SHOW key -> the StateShow enumerator the DLL compares against.
_STATE_SHOW_ENUM = {"always": "Always", "while_on": "WhileOn", "while_off": "WhileOff"}


def check_state_show(header_path):
    """Raise unless STATE_SHOW matches src/goblin_map_data.hpp's StateShow enum, value for value.

    The polarity byte is part of the packed record: a StateShow renumbered on one side only would
    turn "show while ON" into "show while OFF" for every ruled marker, with nothing failing."""
    enum = enum_index(header_path, "StateShow")
    want = {_STATE_SHOW_ENUM[k]: v for k, v in STATE_SHOW.items()}
    if enum != want:
        raise ValueError(f"{header_path}: StateShow is {enum}, tools/mapblob.py STATE_SHOW packs {want}")


def enum_index(header_path, enum_name):
    """Enumerator name -> value for one `enum class <enum_name>` in a header (see category_index)."""
    text = Path(header_path).read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    m = re.search(r"enum\s+class\s+" + re.escape(enum_name) + r"\s*:\s*\w+\s*\{(.*?)\}", text, re.S)
    if not m:
        raise ValueError(f"{header_path}: {enum_name} enum not found")

    out = {}
    value = 0
    for item in m.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        name, _, explicit = (p.strip() for p in item.partition("="))
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            raise ValueError(f"{header_path}: cannot read enumerator {item!r}")
        if explicit:
            value = int(explicit, 0)
        out[name] = value
        value += 1
    return out


class StringTable:
    """NUL-separated names, deduplicated. Index 0 is the empty name, meaning 'no MSB object'."""

    def __init__(self):
        self.buf = bytearray(b"\0")
        self.index = {"": 0}

    def add(self, s):
        if not s:
            return 0
        if s in self.index:
            return self.index[s]
        off = len(self.buf)
        self.buf += s.encode("utf-8") + b"\0"
        self.index[s] = off
        return off


def pack(records, categories):
    """records: dicts with row_id, category (name), fields {param field: number}, geom_slot,
    name_suffix, object_name, lotId, lotType, lotAggregate, real_posX/Z, and optionally
    state_flag / state_show (the world-state rule; absent = none)."""
    names = StringTable()
    body = bytearray()
    for r in records:
        mask = 0
        for name in r["fields"]:
            bit = FIELD_ORDER.index(name)
            mask |= 1 << bit
        cat = categories.get(r["category"])
        if cat is None:
            raise KeyError(f"row {r['row_id']}: Category::{r['category']} is not in the enum")
        if cat > 0xFF:
            raise ValueError("the Category enum outgrew one byte; widen the record")
        name_index = names.add(r.get("object_name") or "")
        if name_index > 0xFFFF:
            raise ValueError("the name table outgrew a u16 index; widen the record")
        state_flag = int(r.get("state_flag", 0))
        state_show = int(r.get("state_show", STATE_SHOW["always"])) if state_flag else 0
        if state_show not in STATE_SHOW.values():
            raise ValueError(f"row {r['row_id']}: state_show {state_show} is not a StateShow value")
        body += struct.pack(
            RECORD_HEAD,
            r["row_id"], mask,
            cat, r["lotType"], r["lotAggregate"], state_show,
            r["geom_slot"], r["name_suffix"], name_index, 0,
            r["lotId"],
            float(r["real_posX"]), float(r["real_posZ"]),
            state_flag)
        for name in FIELD_ORDER:
            if name in r["fields"]:
                v = r["fields"][name]
                if name in FLOAT_FIELDS:
                    body += struct.pack("<f", float(v))
                else:
                    # & 0xFFFFFFFF, not "<i" or "<I": textId1..3 are signed in the paramdef (-1 is
                    # their default) and the flag ids are unsigned, so the record stores the 32 bits
                    # and the DLL's field table casts each one to the member's own type.
                    body += struct.pack("<I", int(float(v)) & 0xFFFFFFFF)

    head = MAGIC + struct.pack("<HII", VERSION, len(records), len(names.buf))
    return bytes(head + bytes(names.buf) + bytes(body))


def unpack(raw):
    """The inverse of pack(), for verification: it decodes what the DLL will decode.

    Returns [{row_id, category (index), fields {name: float|int}, geom_slot, name_suffix,
    object_name, lotId, lotType, lotAggregate, real_posX/Z, state_flag, state_show}]. Integers come back
    SIGNED where the paramdef's member is signed, so a decoded row compares directly against the
    old generated C++.
    """
    signed = {"textId1", "textId2", "textId3", "textEnableFlag2Id1", "textEnableFlag2Id2"}
    if raw[:4] != MAGIC:
        raise ValueError("not a packed map table")
    version, count, strtab_len = struct.unpack_from("<HII", raw, 4)
    if version != VERSION:
        raise ValueError(f"packed table version {version}, this reader is {VERSION}")
    off = 14
    strtab = raw[off:off + strtab_len]
    off += strtab_len

    def name_at(i):
        if i == 0:
            return None
        end = strtab.index(b"\0", i)
        return strtab[i:end].decode("utf-8")

    out = []
    for _ in range(count):
        (row_id, mask, cat, lot_type, lot_agg, state_show, geom, suffix, name_index, _pad2,
         lot_id, rpx, rpz, state_flag) = struct.unpack_from(RECORD_HEAD, raw, off)
        off += RECORD_HEAD_SIZE
        fields = {}
        for i, fname in enumerate(FIELD_ORDER):
            if not (mask & (1 << i)):
                continue
            if fname in FLOAT_FIELDS:
                fields[fname] = struct.unpack_from("<f", raw, off)[0]
            else:
                v = struct.unpack_from("<I", raw, off)[0]
                fields[fname] = struct.unpack("<i", struct.pack("<I", v))[0] \
                    if fname in signed else v
            off += 4
        out.append({"row_id": row_id, "category": cat, "fields": fields,
                    "geom_slot": geom, "name_suffix": suffix, "object_name": name_at(name_index),
                    "lotId": lot_id, "lotType": lot_type, "lotAggregate": lot_agg,
                    "real_posX": rpx, "real_posZ": rpz,
                    "state_flag": state_flag, "state_show": state_show})
    return out


def read_cpp(cpp_path):
    """The baked table, read back out of the generated C++ - what the later pipeline stages use.

    generate_geof_models.py and generate_location_overrides.py used to regex the emitted brace
    initialisers. There is nothing to regex now, and a reader that silently matched nothing has
    already cost this pipeline an empty table once, so they go through here instead.
    """
    text = Path(cpp_path).read_text(encoding="utf-8")
    body = text.split("MAP_BLOB[] = {", 1)[1].split("};", 1)[0]
    packed = bytes(int(b) for b in body.replace("\n", "").split(",") if b.strip())
    raw_size = int(re.search(r"MAP_BLOB_RAW_SIZE = (\d+)u", text).group(1))
    raw = zlib.decompress(packed)
    if len(raw) != raw_size:
        raise ValueError(f"{cpp_path}: expanded to {len(raw)} bytes, header says {raw_size}")
    return unpack(raw)


def write_cpp(raw, out_path):
    """Emit the deflated blob as a C byte array, the way the icon tags ship."""
    packed = zlib.compress(raw, 9)
    lines = ["// AUTO-GENERATED FILE - DO NOT EDIT",
             "// Generated by tools/generate_data.py via tools/mapblob.py.",
             "// The map table, packed and deflated; src/goblin_map_blob.cpp expands it at startup.",
             "",
             "namespace goblin::generated",
             "{",
             "// `extern const`, not plain `const`: a const at namespace scope has INTERNAL linkage",
             "// in C++, so without extern these would not be the symbols goblin_map_blob.cpp declares.",
             f"extern const unsigned int MAP_BLOB_RAW_SIZE = {len(raw)}u;",
             f"extern const unsigned int MAP_BLOB_SIZE = {len(packed)}u;",
             "extern const unsigned char MAP_BLOB[] = {"]
    for i in range(0, len(packed), 32):
        lines.append("".join(f"{b}," for b in packed[i:i + 32]))
    lines.append("};")
    lines.append("} // namespace goblin::generated")
    Path(out_path).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(raw), len(packed)
