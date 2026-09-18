"""The one place map-marker rows are written.

Before this module every generator built its own `param WorldMapPointParam: id N: field: = v;`
f-strings by hand - about 190 such sites across 16 scripts - so the storage format was spread over
the whole toolchain and any change to it meant twenty edits and twenty chances to typo. A generator
now hands rows to a RowSink and the rendering lives here, once.

The file is binary: a magic, a version, then per row an id, a presence mask and only the fields
that row actually sets. FIELDS below IS the format - a field's index is its bit - so a new field
goes on the END and nothing is reordered without a version bump. tools/mapblob.py packs the same
field list into the blob the DLL reads, and src/goblin_map_blob.cpp maps each bit to its paramdef
member, so all three walk one order.

Positions are stored as float64 and rendered at 3 decimals downstream (what every generator wrote
when this was text); every other field the param carries is an integer. A float handed to an
integer field is a bug in the caller, not something to round silently, so it raises.

Typical use:

    sink = RowSink(out_dir / "World - Graces")     # written as World - Graces.rows
    for g in graces:
        sink.row(row_id, iconId=icon, dispMask00=1, areaNo=g.area,
                 posX=g.x, posY=g.y, posZ=g.z, textId1=g.text)
    sink.write()
"""
import struct
from pathlib import Path

# The only fields the param stores as decimals. Everything else is an integer field; see the
# WORLD_MAP_POINT_PARAM_ST paramdef.
FLOAT_FIELDS = frozenset({"posX", "posY", "posZ"})

# Integer fields the paramdef declares SIGNED (textId's default is -1). Stored as their 32 bits and
# read back signed, so a negative id survives the round trip instead of raising on the way out.
SIGNED_FIELDS = frozenset({"textId1", "textId2", "textId3",
                           "textEnableFlag2Id1", "textEnableFlag2Id2"})

# Every field any generator has ever written. The ORDER IS THE FILE FORMAT - a field's position
# here is its bit in the presence mask, so new fields go on the END and nothing is ever removed or
# reordered without a version bump.
FIELDS = (
    "iconId", "dispMask00", "dispMask01", "pad2_0", "areaNo", "gridXNo", "gridZNo",
    "posX", "posY", "posZ",
    "textId1", "textId2", "textId3",
    "textDisableFlagId1", "textDisableFlagId2", "textDisableFlagId3",
    "textEnableFlag2Id1", "textEnableFlag2Id2",
    "clearedEventFlagId", "selectMinZoomStep",
)
FIELD_BIT = {name: i for i, name in enumerate(FIELDS)}
MAGIC = b"MFGR"
VERSION = 1


def coerce(field, value):
    """The value as it is stored: a float for the three position fields, an int for the rest."""
    if field in FLOAT_FIELDS:
        return float(value)
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, float):
        if not value.is_integer():
            raise ValueError(
                f"{field} is an integer param field but got {value!r}; "
                "round it in the caller, where the reason is known")
        return int(value)
    if not isinstance(value, int):
        raise TypeError(f"{field} expects an int, got {type(value).__name__}: {value!r}")
    return value


def pack_row(row_id, fields):
    mask = 0
    for name in fields:
        bit = FIELD_BIT.get(name)
        if bit is None:
            # Silently dropping this is what the old text pipeline did at the far end, and it cost
            # a whole category once. A name that is not in FIELDS is a typo or a new field that has
            # to be added to the table above.
            raise KeyError(f"unknown param field {name!r}; add it to the END of rowsink.FIELDS")
        mask |= 1 << bit
    out = [struct.pack("<II", int(row_id), mask)]
    for name in FIELDS:
        if name in fields:
            v = coerce(name, fields[name])
            # & 0xFFFFFFFF rather than "<I": textId1..3 are SIGNED in the paramdef (-1 is their
            # default), so the row stores the 32 bits and the reader gives them back with the
            # member's own signedness. A plain "<I" would raise on the first negative id written.
            out.append(struct.pack("<d", v) if name in FLOAT_FIELDS
                       else struct.pack("<I", v & 0xFFFFFFFF))
    return b"".join(out)


class RowSink:
    """Collects rows for one category file and renders them on write().

    `path` may be None for a sink that is only read back (a generator that post-processes its own
    rows before deciding where they go).
    """

    def __init__(self, path=None):
        self.path = Path(path) if path is not None else None
        self.rows = []          # [(row_id, {field: value})] in emission order

    def row(self, row_id, **fields):
        """Add one marker. Field order is kept as given."""
        self.add(row_id, fields)
        return row_id

    def add(self, row_id, fields):
        """Same, for callers that already hold a dict (order preserved)."""
        clean = {k: v for k, v in fields.items() if v is not None}
        self.rows.append((int(row_id), clean))
        return row_id

    def extend(self, rows):
        for row_id, fields in rows:
            self.add(row_id, fields)

    def __len__(self):
        return len(self.rows)

    def row_ids(self):
        return [rid for rid, _ in self.rows]

    def drop(self, row_ids):
        """Remove rows by id; returns how many went. Used where a correction has to take markers
        back out (the relocating-boss duplicates), so the caller never edits rendered text."""
        drop = set(row_ids)
        before = len(self.rows)
        self.rows = [(rid, f) for rid, f in self.rows if rid not in drop]
        return before - len(self.rows)

    def blob(self):
        parts = [MAGIC, struct.pack("<HI", VERSION, len(self.rows))]
        for row_id, fields in self.rows:
            parts.append(pack_row(row_id, fields))
        return b"".join(parts)

    def write(self, path=None):
        """Writes `<name>.rows` beside the path given, whatever extension the caller passed.

        Callers still name their category file the way they always did; only the extension moves,
        so the per-category file names stay the category names that generate_data reads.
        """
        target = Path(path) if path is not None else self.path
        if target is None:
            raise ValueError("RowSink has no path to write to")
        target = target.with_suffix(".rows")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(self.blob())
        return target


def read(path):
    """[(row_id, {field: value})] from a .rows file, in the order written."""
    data = Path(path).read_bytes()
    if data[:4] != MAGIC:
        raise ValueError(f"{path}: not a rows file")
    version, count = struct.unpack_from("<HI", data, 4)
    if version != VERSION:
        raise ValueError(f"{path}: rows version {version}, this build writes {VERSION}")
    off = 10
    out = []
    for _ in range(count):
        row_id, mask = struct.unpack_from("<II", data, off)
        off += 8
        fields = {}
        for i, name in enumerate(FIELDS):
            if mask & (1 << i):
                if name in FLOAT_FIELDS:
                    fields[name] = struct.unpack_from("<d", data, off)[0]
                    off += 8
                else:
                    fields[name] = struct.unpack_from(
                        "<i" if name in SIGNED_FIELDS else "<I", data, off)[0]
                    off += 4
        out.append((row_id, fields))
    return out
