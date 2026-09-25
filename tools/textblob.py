"""Pack the generated string tables into the compressed blobs the DLL expands at startup.

Three tables used to ship as C++ string literals in every DLL: the localized enemy names
(goblin_enemy_names.cpp), the English item-name fallback (goblin_item_fallback.cpp) and the
overlay/ini text bundles (goblin_i18n_strings.cpp) - measured 2026-09-24, dropping them took
513-538 KB off every non-ERR DLL (the name tables were UTF-16) and 265 KB off ERR (no enemy names).
They ship now the way the map table does (tools/mapblob.py): packed, deflated, as a byte array.

THE LAYOUTS LIVE IN TWO PLACES and they have to agree: this file and the C++ reader named with
each one. The version word is the guard.

Names table - 'MFGN', read by src/goblin_names_blob.cpp (the enemy names and the item fallback):
  magic      'MFGN'                      4
  version    u16                         2
  columns    u16                         2    15 for the enemy names (ENEMY_NAME_LANGS order), 1 for the fallback
  count      u32                         4
  count x id delta u32                        id[i] - id[i-1] (id[-1] = 0), modulo 2^32
  columns x count strings                     column by column, each NUL-terminated UTF-8. In a
                                              column after the first, the one-byte string "\\x01"
                                              means "the same text as column 0 of this row".
Column by column because one language's names deflate far better next to each other than
interleaved with fourteen other scripts, and the "\\x01" rows because an untranslated name is a
copy of the English one, often further back than deflate can see.

i18n bundles - 'MFGI', read by src/goblin_i18n_blob.cpp:
  magic      'MFGI'                      4
  version    u16                         2
  locales    u16                         2
  locales x:
    language u8                               the goblin::i18n::Language value it answers for;
                                              the first locale also answers for any value without one
    6 x table, in LocaleBundle order (texts, toasts, sect_labels, sect_comments, entry_labels,
    entry_comments):
      count  u32                              0 = no table (nullptr in the bundle)
      texts:   count x id u16, then count x NUL-terminated UTF-8
      toasts:  count x id u16, then count x UTF-16LE terminated by a 0 unit
      names:   count x key, then count x value, each NUL-terminated UTF-8
Ids apart from strings and keys apart from values for the same reason as the names table's
columns: ~3.5 KB less deflated. The ids are the enumerators' values; generate_i18n.py emits a
static_assert per name so a blob that no longer matches the header fails the compile instead of
shifting every string by one.
"""
import re
import struct
import zlib
from pathlib import Path

NAMES_MAGIC = b"MFGN"
NAMES_VERSION = 1
I18N_MAGIC = b"MFGI"
I18N_VERSION = 1
SAME_AS_FIRST = "\x01"
I18N_TABLES = ("texts", "toasts", "sect_labels", "sect_comments", "entry_labels", "entry_comments")


def _check_text(s, what):
    # NUL ends a string in the blob and "\x01" is the same-as-column-0 marker, so neither may be
    # part of a real one. Neither has ever been in a name, a menu string or a comment.
    if "\x00" in s or "\x01" in s:
        raise ValueError(f"{what}: {s!r} holds a control character the packed table reserves")


def pack_names(rows, columns):
    """rows: [(id, [str] * columns)], ascending by id as the DLL's lower_bound needs."""
    body = bytearray()
    prev = 0
    for i, (row_id, cells) in enumerate(rows):
        if len(cells) != columns:
            raise ValueError(f"row {row_id}: {len(cells)} cells, the table has {columns}")
        if i and row_id <= rows[i - 1][0]:
            raise ValueError(f"row {row_id}: ids must ascend (the DLL binary-searches them)")
        body += struct.pack("<I", (row_id - prev) & 0xFFFFFFFF)
        prev = row_id
    for c in range(columns):
        for row_id, cells in rows:
            s = cells[c]
            if s is None:
                raise ValueError(f"row {row_id}: an empty cell has no packed form")
            _check_text(s, f"row {row_id}")
            if c and s == cells[0]:
                s = SAME_AS_FIRST
            body += s.encode("utf-8") + b"\0"
    head = NAMES_MAGIC + struct.pack("<HHI", NAMES_VERSION, columns, len(rows))
    return bytes(head + body)


def unpack_names(raw):
    """The inverse of pack_names(), done the way the DLL does it: the whole string block is widened
    in ONE UTF-8 -> UTF-16 conversion (MultiByteToWideChar there) and then split at the 0 units.

    Returns (columns, [(id, [tuple of UTF-16 code units] * columns)]).
    """
    if raw[:4] != NAMES_MAGIC:
        raise ValueError("not a packed names table")
    version, columns, count = struct.unpack_from("<HHI", raw, 4)
    if version != NAMES_VERSION:
        raise ValueError(f"packed names version {version}, this reader is {NAMES_VERSION}")
    off = 12
    ids = []
    cur = 0
    for _ in range(count):
        cur = (cur + struct.unpack_from("<I", raw, off)[0]) & 0xFFFFFFFF
        off += 4
        ids.append(cur - (1 << 32) if cur >= (1 << 31) else cur)
    wide = raw[off:].decode("utf-8").encode("utf-16-le")  # strict both ways, as MB_ERR_INVALID_CHARS
    units = [wide[i] | (wide[i + 1] << 8) for i in range(0, len(wide), 2)]
    cells = [[None] * columns for _ in range(count)]
    p = 0
    for c in range(columns):
        for i in range(count):
            end = units.index(0, p)
            s = tuple(units[p:end])
            p = end + 1
            if c and s == (1,):
                s = cells[i][0]
            cells[i][c] = s
    if p != len(units):
        raise ValueError(f"{len(units) - p} code units left over after the last string")
    return columns, list(zip(ids, cells))


def pack_i18n(locales):
    """locales: [(language value, {table: rows or None})], the fallback locale first.
    texts/toasts rows are (enumerator value, str); name-table rows are (key, value)."""
    out = bytearray(I18N_MAGIC + struct.pack("<HH", I18N_VERSION, len(locales)))
    for language, tables in locales:
        out += struct.pack("<B", language)
        for name in I18N_TABLES:
            rows = tables.get(name) or []
            out += struct.pack("<I", len(rows))
            for a, b in rows:
                _check_text(b, f"{name} {a}")
                if name in ("texts", "toasts"):
                    out += struct.pack("<H", a)
                else:
                    _check_text(a, name)
                    out += a.encode("utf-8") + b"\0"
            for a, b in rows:
                out += (b.encode("utf-16-le") + b"\0\0") if name == "toasts" \
                    else (b.encode("utf-8") + b"\0")
    return bytes(out)


def unpack_i18n(raw):
    """The inverse of pack_i18n(), walked the way the DLL walks it. Narrow strings come back as the
    bytes the DLL points at, toasts as UTF-16 code units, an empty table as None (nullptr).

    Returns [(language value, {table: rows or None})].
    """
    if raw[:4] != I18N_MAGIC:
        raise ValueError("not a packed i18n table")
    version, count = struct.unpack_from("<HH", raw, 4)
    if version != I18N_VERSION:
        raise ValueError(f"packed i18n version {version}, this reader is {I18N_VERSION}")
    off = 8

    def narrow():
        nonlocal off
        end = raw.index(b"\0", off)
        s = bytes(raw[off:end])
        off = end + 1
        return s

    def wide():
        nonlocal off
        units = []
        while True:
            u = struct.unpack_from("<H", raw, off)[0]
            off += 2
            if u == 0:
                return tuple(units)
            units.append(u)

    out = []
    for _ in range(count):
        language = raw[off]
        off += 1
        tables = {}
        for name in I18N_TABLES:
            n = struct.unpack_from("<I", raw, off)[0]
            off += 4
            firsts = []
            for _ in range(n):
                if name in ("texts", "toasts"):
                    firsts.append(struct.unpack_from("<H", raw, off)[0])
                    off += 2
                else:
                    firsts.append(narrow())
            rows = [(a, wide() if name == "toasts" else narrow()) for a in firsts]
            tables[name] = rows if n else None
        out.append((language, tables))
    if off != len(raw):
        raise ValueError(f"{len(raw) - off} bytes left over after the last locale")
    return out


def write_cpp(raw, out_path, symbol, *, namespace, generator, what, includes=(), extra=()):
    """Emit the deflated blob as a C byte array (the map table's form, see mapblob.write_cpp).
    `includes` go above the namespace, `extra` lines inside it ahead of the blob."""
    packed = zlib.compress(raw, 9)
    lines = ["// AUTO-GENERATED FILE - DO NOT EDIT",
             f"// Generated by {generator} via tools/textblob.py.",
             f"// {what}",
             "",
             *[f'#include "{h}"' for h in includes],
             *([""] if includes else []),
             f"namespace {namespace}",
             "{",
             *extra,
             "// `extern const`, not plain `const`: a const at namespace scope has INTERNAL linkage",
             "// in C++, so without extern these would not be the symbols the reader declares.",
             f"extern const unsigned int {symbol}_RAW_SIZE = {len(raw)}u;",
             f"extern const unsigned int {symbol}_SIZE = {len(packed)}u;",
             f"extern const unsigned char {symbol}[] = {{"]
    for i in range(0, len(packed), 32):
        lines.append("".join(f"{b}," for b in packed[i:i + 32]))
    lines.append("};")
    lines.append(f"}} // namespace {namespace}")
    Path(out_path).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(raw), len(packed)


def read_cpp(cpp_path, symbol):
    """The raw (inflated) blob, read back out of the generated C++ - for verification."""
    text = Path(cpp_path).read_text(encoding="utf-8")
    body = text.split(f"unsigned char {symbol}[] = {{", 1)[1].split("};", 1)[0]
    packed = bytes(int(b) for b in body.replace("\n", "").split(",") if b.strip())
    raw_size = int(re.search(rf"{symbol}_RAW_SIZE = (\d+)u", text).group(1))
    size = int(re.search(rf"{symbol}_SIZE = (\d+)u", text).group(1))
    if size != len(packed):
        raise ValueError(f"{cpp_path}: {len(packed)} bytes, header says {size}")
    raw = zlib.decompress(packed)
    if len(raw) != raw_size:
        raise ValueError(f"{cpp_path}: expanded to {len(raw)} bytes, header says {raw_size}")
    return raw
