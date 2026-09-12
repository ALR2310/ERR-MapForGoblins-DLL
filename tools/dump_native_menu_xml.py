"""Export the native (F8) menu structure to XML, for restructuring by hand.

The XML is DERIVED from the same sources the DLL itself uses, so it cannot drift into a
paraphrase:

  src/goblin_config_schema.cpp          the ini sections and their entries. Each SECTION becomes a
                                        menu page (id = kPageSectionBase + index) and each ENTRY a
                                        row - this is why adding an ini section needs no menu code.
  src/goblin_native_menu.cpp            the root page's own rows, the fixed pages, and the Actions
                                        page (its rows are hand-built, with C function names).
  src/generated_shared/goblin_overlay_icons.cpp
                                        ini key -> atlas cell -> source icon id, resolved exactly as
                                        goblin_native_menu.cpp's icon_for_key() does.
  i18n/en.json                          the English labels/help the player actually sees
                                        (section_labels, section_comments, entry_labels,
                                        entry_comments, texts).

Usage:
    py tools/dump_native_menu_xml.py                    -> docs/native_menu_structure.xml
    py tools/dump_native_menu_xml.py out.xml
"""

import json
import pathlib
import re
import sys
import xml.dom.minidom as minidom
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
OUT_DEFAULT = ROOT / "docs" / "native_menu_structure.xml"

# Page ids, mirrored from goblin_native_menu.hpp. Kept here as data so the XML can name them.
PAGES = {0: "root", 1: "progress", 2: "hidden", 3: "actions", 4: "value", 5: "rebind"}
PAGE_SECTION_BASE = 100
PAGE_ADDON_BASE = 1000
PAGE_REGION_BASE = 4000

ROW_KINDS = {
    "Back": "go up one level",
    "SubPage": "opens another page in place",
    "Toggle": "bool config value; decide flips it",
    "Number": "numeric config value; decide opens its value page",
    "Enum": "value from a fixed list; decide opens its value page",
    "ValueOption": "one choice ON a value page; decide applies it and returns",
    "Rebind": "hotkey; decide opens the 'press a key' page",
    "Action": "runs something once",
    "Info": "read-only text",
    "Progress": "read-only text + a bar drawn from collected/total",
}


def read(p):
    return (SRC / p).read_text(encoding="utf-8", errors="replace")


# ── icons: ini key -> source icon id (same arithmetic as icon_for_key) ──────────────────────────
def icon_map():
    txt = (SRC / "generated_shared" / "goblin_overlay_icons.cpp").read_text(encoding="utf-8")
    atlas_w = int(re.search(r"const int ATLAS_W\s*=\s*(\d+)", txt).group(1))
    cell = int(re.search(r"const int CELL\s*=\s*(\d+)", txt).group(1))
    src_icons = [int(x) for x in
                 re.search(r"const int CELL_SRC_ICON\[\]\s*=\s*\{([^}]*)\}", txt).group(1).split(",")
                 if x.strip()]
    per_row = atlas_w // cell
    out = {}
    for key, col, row in re.findall(r'\{"([A-Za-z0-9_]+)",\s*(\d+),\s*(\d+)\}', txt):
        idx = int(row) * per_row + int(col)
        out[key] = src_icons[idx] if 0 <= idx < len(src_icons) else -1
    return out


# ── the ini schema: sections + entries ─────────────────────────────────────────────────────────
def parse_schema():
    txt = read("goblin_config_schema.cpp")
    body = txt[txt.index("std::vector<IniSection> build_schema()"):]
    body = body[: body.index("\n    }\n")] if "\n    }\n" in body else body

    sections = []
    # A section opens with {"Name", <comment or nullptr>, <errOnly>, {  ... }}
    for m in re.finditer(r'\n\s*\{"([^"]+)",\s*(nullptr|"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)\s*,\s*(ERR|true|false)\s*,\s*\{',
                         body):
        sections.append({"name": m.group(1), "err_only": m.group(3) in ("ERR", "true"),
                         "start": m.end(), "entries": []})
    for i, sec in enumerate(sections):
        end = sections[i + 1]["start"] if i + 1 < len(sections) else len(body)
        chunk = body[sec["start"]:end]
        # Both entry shapes, kept in SOURCE ORDER. The order IS the menu's row order and the
        # schema interleaves the macro form with the explicit one, so grouping by shape (which an
        # earlier version did) silently reordered every mixed section.
        found = []
        for em in re.finditer(r'\b(BE?)\(\s*"([A-Za-z0-9_]+)"\s*,\s*([A-Za-z0-9_]+)\s*,\s*'
                              r'((?:"(?:[^"\\]|\\.)*"|MFG_LL_DEF|[A-Za-z0-9_]+))\s*,', chunk):
            found.append((em.start(), {"key": em.group(2), "var": em.group(3), "type": "Bool",
                                       "default": em.group(4).strip('"'),
                                       "err_only": em.group(1) == "BE"}))
        for em in re.finditer(r'IniEntry\{\s*"([A-Za-z0-9_]+)"\s*,\s*IniType::(\w+)\s*,\s*'
                              r'&cfg::(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"', chunk):
            found.append((em.start(), {"key": em.group(1), "var": em.group(3), "type": em.group(2),
                                       "default": em.group(4), "err_only": False}))
        sec["entries"] = [e for _, e in sorted(found, key=lambda t: t[0])]
        sec.pop("start")
    return sections



# ── the generated region table: place_name_id -> name, and its tiles' areas -> mega group ──────
def regions():
    """[(place_name_id, english name, mega)] in the generated order."""
    cpp = (SRC / "generated" / "goblin_region_map.cpp").read_text(encoding="utf-8", errors="replace")
    names = [(int(pid), nm) for pid, nm in
             re.findall(r'\{(-?\d+),\s*"((?:[^"\\]|\\.)*)"\}', cpp)]
    # RegionTile{key, place_name_id} with key = area<<16 | gridX<<8 | gridZ
    areas = {}
    for key, pid in re.findall(r"\{(\d+)u?,\s*(-?\d+)\}", cpp):
        areas.setdefault(int(pid), set()).add((int(key) >> 16) & 0xFFFF)
    out = []
    for pid, nm in names:
        a = areas.get(pid, set())
        mega = ("Realm of Shadow" if 61 in a else
                "Underground" if 12 in a else
                "The Lands Between")
        out.append((pid, nm, mega))
    return out


# ── the menu layout table (goblin_native_menu.cpp) ─────────────────────────────────────────────
def layout():
    """[(label, [section names], [ini keys], toggle_all)] in page order, read from kLayout."""
    txt = read("goblin_native_menu.cpp")
    arrays = {}
    for m in re.finditer(r"const char \*const (k\w+)\[\] = \{([^}]*)\}", txt):
        arrays[m.group(1)] = re.findall(r'"([^"]+)"', m.group(2))
    block = re.search(r"const LayoutPage kLayout\[\] = \{(.*?)\n    \};", txt, re.S).group(1)
    pages = []
    # Over the whole block, not per line: an entry may wrap, and its counts may be a
    # sizeof(...)/sizeof(...) expression ("Menu settings"), which the per-line \d+ match skipped -
    # the page silently vanished from the dump.
    for m in re.finditer(r'\{"([^"]+)",\s*(\w+|nullptr),\s*[^,{}]+?,\s*(\w+|nullptr),\s*[^,{}]+?,'
                         r'\s*(true|false),\s*(true|false)\}', block, re.S):
        label, secs, keys, toggle, dump = m.groups()
        pages.append((label, arrays.get(secs, []), arrays.get(keys, []), toggle == "true",
                      dump == "true"))
    return pages


def readonly_keys():
    """Keys the model shows but refuses to change from the menu (key_is_readonly)."""
    txt = read("goblin_native_menu.cpp")
    body = txt[txt.index("bool key_is_readonly("):]
    body = body[: body.index("\n    }")]
    return set(re.findall(r'strcmp\(key,\s*"([^"]+)"\)', body))


def parse_root_extra():
    """The root page's non-section rows, in the order build_root() pushes them."""
    txt = read("goblin_native_menu.cpp")
    start = txt.index("void build_root()")
    body = txt[start: txt.index("\n    }", start)]
    rows = []
    for m in re.finditer(r"\n        Row (\w+);(.*?)push\(\1\);", body, re.S):
        blk = m.group(2)
        page = re.search(r"\.page_id\s*=\s*goblin::nmenu::(\w+)", blk)
        lab_tr = re.search(r"\.label\s*=\s*text\(tr::TextId::(\w+)\)", blk)
        rows.append({"target_page": page.group(1) if page else None,
                     "label_textid": lab_tr.group(1) if lab_tr else None,
                     "dynamic_value": "%zu" in blk})
    return rows


def main():
    out_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else OUT_DEFAULT
    icons = icon_map()
    schema = parse_schema()
    i18n = json.loads((ROOT / "i18n" / "en.json").read_text(encoding="utf-8"))
    sec_labels = i18n.get("section_labels", {})
    sec_comments = i18n.get("section_comments", {})
    ent_labels = i18n.get("entry_labels", {})
    ent_comments = i18n.get("entry_comments", {})
    texts = i18n.get("texts", {})

    root = ET.Element("mfg-native-menu")
    root.set("note", "Derived from the code+i18n sources listed in tools/dump_native_menu_xml.py. "
                     "Edit freely to propose a new structure; ids are informational. Leaving a page is Q "
                     "or the pad's circle (the engine's own Back) - there are no Back rows.")

    legend = ET.SubElement(root, "legend")
    for k, d in ROW_KINDS.items():
        ET.SubElement(legend, "row-kind", {"name": k, "means": d})
    ET.SubElement(legend, "page-ids", {
        "root": "0", "progress": "1", "hidden": "2", "actions": "3", "value": "4", "rebind": "5",
        "section-base": str(PAGE_SECTION_BASE), "addon-base": str(PAGE_ADDON_BASE),
        "region-base": str(PAGE_REGION_BASE),
        "note": "a section page id is section-base + its index in the ini schema"})

    # ── root ────────────────────────────────────────────────────────────────────────────────
    pages = layout()
    ro = readonly_keys()
    by_name = {sec["name"]: sec for sec in schema}
    pr = ET.SubElement(root, "page", {"id": "0", "kind": "root", "title": "MapForGoblins"})
    ET.SubElement(pr, "row", {
        "kind": "Toggle", "ini-key": "require_map_fragments",
        "label": ent_labels.get("require_map_fragments", "require_map_fragments"),
        "help": ent_comments.get("require_map_fragments", ""),
        "note": "lifted out of its section: important enough to sit at the top level"})
    for i, (label, secs, keys, _toggle, _dump) in enumerate(pages):
        row = ET.SubElement(pr, "row", {
            "kind": "SubPage", "target-page": str(PAGE_SECTION_BASE + i),
            "label": sec_labels.get(label, label),
            "value": "<count of visible rows>  >"})
        tip = sec_comments.get(label)
        if tip:
            row.set("help", tip)
        if i == 0:  # Progress and Hidden sit right after Categories
            ET.SubElement(pr, "row", {
                "kind": "SubPage", "target-page": "progress",
                "label": texts.get("TabProgress", "Progress"), "label-textid": "TabProgress",
                "value": ">", "red-when": "a category is isolated on the map"})
            ET.SubElement(pr, "row", {
                "kind": "SubPage", "target-page": "hidden",
                "label": texts.get("HiddenMarkers", "Hidden markers"),
                "label-textid": "HiddenMarkers", "value": "<count>  >"})
    ET.SubElement(pr, "row", {"kind": "SubPage", "target-page": "addon-pages",
                              "label": "<one row per page registered by another mod>",
                              "note": "sdk/mfg_menu_api.h; ids from addon-base"})
    ET.SubElement(pr, "row", {"kind": "Info", "label": "<not in the menu on purpose>",
                              "note": "native_menu (switching the menu off from inside it) and the "
                                      "overlay-only settings (window geometry, opacity, font scale, "
                                      "render mode) stay ini-only"})

    # ── one page per LAYOUT entry ───────────────────────────────────────────────────────────
    for i, (label, secs, keys, toggle_all, dump_rows) in enumerate(pages):
        pg = ET.SubElement(root, "page", {"id": str(PAGE_SECTION_BASE + i), "kind": "layout",
                                          "title": sec_labels.get(label, label)})
        if toggle_all:
            ET.SubElement(pg, "row", {
                "kind": "Action",
                "label": texts.get("AllIconCategories", "All icon categories:"),
                "label-textid": "AllIconCategories",
                "value": "<on>/<total> (runtime), green when all on",
                "note": "ONE row, not a pair: everything on -> turn everything off, anything else "
                        "-> turn everything on. Not a Toggle because the state is an aggregate of "
                        "every show_* key, with no single bool to point at"})
        emit = []
        for name in secs:
            sec = by_name.get(name)
            if not sec:
                continue
            if len(secs) > 1:
                emit.append(("sep", name, None))
            for e in sec["entries"]:
                emit.append(("row", name, e))
        for k in keys:
            e = next((x for sec in schema for x in sec["entries"] if x["key"] == k), None)
            if e:
                emit.append(("row", None, e))
        for what, name, e in emit:
            if what == "sep":
                ET.SubElement(pg, "row", {
                    "kind": "Info", "label": sec_labels.get(name, name), "separator": "true",
                    "note": "valueless Info row -> PadCategory frame (one wide caption)"})
                continue
            kind = {"Bool": "Toggle", "U8": "Number", "Float": "Number", "Int": "Number",
                    "Text": "Enum", "Language": "Enum",
                    "VkKey": "Rebind", "GamepadMask": "Rebind"}.get(e["type"], "Info")
            a = {"kind": "Info" if e["key"] in ro else kind, "ini-key": e["key"],
                 "config-var": e["var"], "ini-type": e["type"], "default": e["default"],
                 "label": ent_labels.get(e["key"], e["key"])}
            if e["key"] in ro:
                a["read-only"] = "true"
                a["note"] = "shown with its value as a greyed Info row - cannot be changed here"
            ic = icons.get(e["key"], -1)
            if ic >= 0:
                a["icon-id"] = str(ic)
            if e["err_only"]:
                a["err-only"] = "true"
            hlp = ent_comments.get(e["key"])
            if hlp:
                a["help"] = hlp
            ET.SubElement(pg, "row", a)
        if dump_rows:
            for lbl, tid, note in (
                    (texts.get("DumpBeacons", "Dump beacons"), "DumpBeacons",
                     "snapshots the live beacon markers into memory"),
                    (texts.get("DumpStamps", "Dump stamps"), "DumpStamps",
                     "same for map stamps")):
                ET.SubElement(pg, "row", {"kind": "Action", "label": lbl,
                                          "label-textid": tid, "note": note})
            ET.SubElement(pg, "row", {
                "kind": "Action", "label": texts.get("Copy", "Copy"), "label-textid": "Copy",
                "value": "<size> B",
                "note": "copies the dump to the Windows clipboard (CF_UNICODETEXT); a greyed Info "
                        "row until a dump has been taken, and its value shows what would be copied"})
            ET.SubElement(pg, "row", {
                "kind": "Action", "label": texts.get("CopyStatus", "Copy status"),
                "label-textid": "CopyStatus",
                "note": "copies goblin::diag::report() - the inject-status readout used in bug reports"})

    # ── the fixed extra pages ───────────────────────────────────────────────────────────────
    pg = ET.SubElement(root, "page", {"id": "1", "kind": "progress",
                                      "title": texts.get("TabProgress", "Progress")})
    ET.SubElement(pg, "row", {
        "kind": "Action", "label": texts.get("ProgressFocusClear", "clear the highlight"),
        "label-textid": "ProgressFocusClear",
        "note": "red and usable while a category is isolated; a greyed Info row otherwise"})
    ET.SubElement(pg, "row", {"kind": "Progress", "label": texts.get("MenuTotal", "Total"),
                              "label-textid": "MenuTotal",
                              "value": "<collected>/<total> + bar (runtime)"})
    # The three mega-sections always exist in this order; the separator itself is a valueless
    # Info row, which is what selects the row clip's PadCategory frame.
    examples = {
        "The Lands Between": [("Stormveil Castle", 10000), ("Leyndell, Royal Capital", 11000),
                              ("Academy of Raya Lucaria", 14000)],
        "Underground": [("Nokron, Eternal City", 12020), ("Subterranean Shunning-Grounds", 11000)],
        "Realm of Shadow": [("Belurat, Tower Settlement", 20000), ("Shadow Keep", 21000),
                            ("Midra's Manse", 28000)],
    }
    for mega, rows in examples.items():
        ET.SubElement(pg, "row", {
            "kind": "Info", "label": mega, "separator": "true",
            "note": "mega-section header - a valueless Info row, so the row clip draws it on the "
                    "PadCategory frame (one wide caption, no value)"})
        for nm, pid in rows:
            ET.SubElement(pg, "row", {
                "kind": "SubPage", "label": nm, "place-name-id": str(pid),
                "target-page": "region-base + index", "example": "true",
                "red-when": "this region holds the isolated category",
                "value": "<collected>/<total> + bar (runtime)"})
        ET.SubElement(pg, "row", {
            "kind": "SubPage", "label": f"<other {mega} regions>", "example-note": "same shape",
            "target-page": "region-base + index", "value": "<collected>/<total> + bar (runtime)"})
    ET.SubElement(pg, "row", {
        "kind": "SubPage", "label": "Other", "place-name-id": "<negative>",
        "target-page": "region-base + index", "value": "<collected>/<total> + bar (runtime)",
        "note": "trailing bucket for markers with no region; deliberately gets NO header above it"})
    ET.SubElement(pg, "row", {"kind": "Info", "label": texts.get("ProgressNoMarkers", "no markers"),
                              "label-textid": "ProgressNoMarkers",
                              "note": "the ONLY row when nothing is loaded yet"})
    pg.set("note",
           "STRUCTURE is exact; the region ROWS above are examples. The real list is built at "
           "runtime: a marker's region comes from the dungeon tile map (generated::REGION_TILES, "
           "areas 10..45) or AREA_FALLBACK, and for the overworld from the marker's own baked "
           "location id - whose NAME is the game's own text, not ours. A region with zero markers "
           "is skipped, and the order is the sorted region list, not this file's order.")

    # one worked example of a per-region page
    rp = ET.SubElement(root, "page", {"id": "region-base + <index>", "kind": "region",
                                      "title": "example: Stormveil Castle"})
    ET.SubElement(rp, "row", {
        "kind": "Action", "label": texts.get("ProgressFocusClear", "clear the highlight"),
        "note": "same row as on the progress page"})
    ET.SubElement(rp, "row", {"kind": "Progress", "label": texts.get("MenuTotal", "Total"),
                              "value": "<collected>/<total> + bar (runtime)"})
    ET.SubElement(rp, "row", {
        "kind": "Progress", "label": "<one row per category present in this region>",
        "value": "<collected>/<total> + bar (runtime)",
        "note": "label is the category's ini entry_label; confirming isolates that category on the "
                "live map (the glow icon) and confirming it again clears the filter"})

    pg = ET.SubElement(root, "page", {"id": "2", "kind": "hidden",
                                      "title": texts.get("HiddenMarkers", "Hidden markers")})
    ET.SubElement(pg, "row", {"kind": "Action", "label": "<one row per manually hidden marker>",
                              "value": "<marker name> (runtime)",
                              "note": "activating un-hides it; the list is whatever the player hid "
                                      "with the hide-marker key, so it is empty by default"})

    for pid, kind, title in ((4, "value", "value chooser for one entry"),
                             (5, "rebind", "press a key")):
        pg = ET.SubElement(root, "page", {"id": str(pid), "kind": kind, "title": title})
        if kind == "value":
            ET.SubElement(pg, "row", {"kind": "ValueOption", "label": "<one row per choice>"})
        else:
            ET.SubElement(pg, "row", {"kind": "Info", "label": "<waits for a key press>"})
            # The one surviving Back-kind row: it means "keep the current binding", a choice
            # rather than navigation, which is why the Back-row cleanup left it alone.
            ET.SubElement(pg, "row", {"kind": "Back", "label": "Keep current"})

    xml = minidom.parseString(ET.tostring(root, encoding="unicode")).toprettyxml(indent="  ")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(xml, encoding="utf-8")
    rows = sum(len(list(p)) for p in root.findall("page"))
    print(f"wrote {out_path}  ({len(root.findall('page'))} pages, {rows} rows, "
          f"{len(schema)} ini sections, {len(icons)} icon keys)")


if __name__ == "__main__":
    main()
