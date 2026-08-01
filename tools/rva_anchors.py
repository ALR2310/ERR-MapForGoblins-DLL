"""RVA anchors for the in-game (native) menu code.

Most of the mod resolves game functions by AOB scan, so a game update that moves code is
caught by tools/check_aobs.py. The native-menu work, however, calls a large set of small
UI helpers DIRECTLY as `base + RVA` (they are one-liners over Scaleform interfaces, and
scanning ~40 of them would be both slow and fragile because several share a prologue).

A direct RVA call cannot be repaired automatically, but a break MUST NOT be silent: after
a game patch the address would point at unrelated code and the menu would misbehave or
crash. So each address is anchored here by the first bytes that should live at it, and
check_aobs.py verifies every anchor against the shipped exe at BUILD time.

Adding an anchor is cheap: run
    py tools/rva_anchors.py --emit 0x74A2F0 my_helper
and paste the printed entry.

Fields:
  name  - stable id, matches the helper's role in the code
  rva   - the address the C++ uses as `base + rva`
  bytes - expected byte prefix at that RVA ('??' = wildcard, for rip-relative operands)
  used  - where it is called from, so a break points at the right code
"""

ANCHORS = [
    # ---- clip / text primitives (goblin_stall_probe.cpp draw + caption helpers) ----
    {"name": "clip_resolve_child", "rva": 0x74A2F0,
     "bytes": "4C 89 44 24 18 4C 89 4C 24 20 55 53 56 57 41 56",
     "used": "draw_our_row / set_form_captions / prepare_form_layout / draw_row_icon"},
    {"name": "clip_set_text_html", "rva": 0x74A000,
     "bytes": "40 53 48 83 EC 20 48 8B 09 48 8B DA 48 8B 01 FF",
     "used": "draw_our_row / set_form_captions (SetText with isHtml=1)"},
    {"name": "clip_set_visible", "rva": 0x733340,
     "bytes": "40 53 48 83 EC 20 48 8B 01 0F B6 DA FF 50 08 8B",
     "used": "draw_our_row / prepare_form_layout / draw_row_icon"},
    {"name": "clip_is_valid", "rva": 0x733150,
     "bytes": "48 83 EC 28 48 8B 01 FF 10 F6 40 20 8F 0F 95 C0",
     "used": "every resolve site (guards a missing clip)"},
    {"name": "clip_set_gray", "rva": 0x7331E0,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 0F B6 FA",
     "used": "row style (Grayout frame)"},
    {"name": "clip_set_scale", "rva": 0x733280,
     "bytes": "40 53 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF",
     "used": "draw_row_bar_clip (experimental graphic bar)"},
    {"name": "clip_goto_frame_name", "rva": 0x7499E0,
     "bytes": "48 89 54 24 10 48 83 EC 28 48 8B 09 48 8B 01 FF",
     "used": "row style frames Normal/Grayout/PadCategory"},
    {"name": "clip_goto_frame_num", "rva": 0x749980,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B 09 8B FA 48",
     "used": "prepare_form_layout (BG wide panel) / draw_row_icon (icon frame)"},
    {"name": "clip_proxy_dtor", "rva": 0xD7F850,
     "bytes": "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE",
     "used": "every resolve site (releases the proxy)"},

    # ---- row list machinery (the hooks + the rebuild path) ----
    {"name": "menu_row_build_dispatch", "rva": 0x868590,
     "bytes": "44 0F BE 42 08 45 85 C0 74 0B 41 83 F8 01 75 0A",
     "used": "build_items_detour hook + refresh_form_view"},
    {"name": "menu_row_render", "rva": 0x8674E0,
     "bytes": "48 8B C4 55 57 41 56 48 8D 68 A1 48 81 EC E0 00",
     "used": "row_render_detour hook (item vt+0x8)"},
    {"name": "menu_row_decide", "rva": 0x9411A0,
     "bytes": "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 38",
     "used": "form_decide_detour hook"},
    {"name": "menu_view_refresh", "rva": 0x942690,
     "bytes": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48",
     "used": "refresh_form_view"},
    {"name": "menu_row_vec_clear", "rva": 0x868F20,
     "bytes": "4C 89 44 24 18 56 57 48 83 EC 28 49 8B C1 48 8B",
     "used": "build_our_form_items"},
    {"name": "menu_row_vec_append", "rva": 0x868FE0,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA",
     "used": "build_our_form_items"},
    {"name": "menu_row_item_ctor", "rva": 0x866F80,
     "bytes": "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE",
     "used": "build_our_form_items (real row item)"},
    {"name": "menu_row_item_empty", "rva": 0x8686C0,
     "bytes": "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 28 FE",
     "used": "build_our_form_items (right-column filler)"},
    {"name": "grid_cursor_get", "rva": 0x739E20,
     "bytes": "8B 81 D4 00 00 00 C3 48 8D 64 24 08 FF 64 24 F8",
     "used": "selected_model_index (GridControl cursor)"},

    # ---- screen open / job plumbing ----
    {"name": "keyconfig_form_build", "rva": 0x8078F0,
     "bytes": "4C 8B DC 53 48 81 EC B0 00 00 00 49 C7 43 88 FE",
     "used": "open_keyconfig_form (movie 02_160 job)"},
    {"name": "job_ref_convert_a", "rva": 0x7A7E30,
     "bytes": "4C 8B DC 49 89 53 10 53 56 57 48 83 EC 70 49 C7",
     "used": "open_keyconfig_form ref chain"},
    {"name": "job_ref_convert_b", "rva": 0x7A7B60,
     "bytes": "48 89 54 24 10 53 48 83 EC 30 48 C7 44 24 28 FE",
     "used": "open_keyconfig_form ref chain"},
    {"name": "job_holder_store_seq", "rva": 0x7A9250,
     "bytes": "48 89 54 24 10 57 48 83 EC 30 48 C7 44 24 20 FE",
     "used": "open_keyconfig_form (sequence slot +0x10)"},
    {"name": "job_holder_store_child", "rva": 0x7A9460,
     "bytes": "4C 89 44 24 18 48 89 54 24 10 56 57 41 56 48 83",
     "used": "open_keyconfig_form (child slot +0xA28)"},
    {"name": "refcount_addref", "rva": 0x1EBA1C0,
     "bytes": "B8 01 00 00 00 F0 0F C1 01 C3 CC 8E 0D 0D 73 A7",
     "used": "job ref dance"},
    {"name": "refcount_unref", "rva": 0x1EBA200,
     "bytes": "83 C8 FF F0 0F C1 01 C3 90 76 10 4D 6C 78 ED E7",
     "used": "job ref dance / release_job_ref"},
    {"name": "list_row_path_build", "rva": 0x736FC0,
     "bytes": "4C 8B DC 57 48 81 EC 90 00 00 00 49 C7 43 90 FE",
     "used": "row_path_detour (gives the slot index for row icons)"},
    {"name": "clip_set_pos", "rva": 0x733230,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 41 8B D8",
     "used": "draw_row_icon (shifts the icon strip)"},

    # ---- movie transform (goblin_own_movie.cpp) ----
    # The engine's movie OPENER is deliberately not used and has no anchor: whether it is called for a
    # given movie is the mod loader's decision (ModEngine3 answers for the files it overrides before the
    # engine gets there, so of Convergence's 22 overridden movies it fired for 0). The transform runs on
    # the tag loop instead - AOB gfx_tag_loop - which every movie goes through whoever served the file.
    # Not called - it is the constructor whose body DEFINES the memory-file layout the transform
    # relies on (+0x18 buffer, +0x20 size, +0x24 position, +0x08 refcount). If these bytes stop
    # matching, re-read the layout before trusting the transform.
    {"name": "memory_file_ctor", "rva": 0xCE7BB0,
     "bytes": "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE",
     "used": "own_movie: source of the memory-file field offsets"},

    # ---- independent icon path (goblin_sfimage.cpp): NOT COMPILED as of 2026-07-31 ----
    # goblin_sfimage.cpp is out of CMakeLists.txt (its last external caller went with the own-draw
    # icon routes), so the four anchors below no longer guard code that ships. They are KEPT rather
    # than deleted, unlike the stale entries pruned from aob_signatures.py, because an anchor is a
    # byte-identity check against the exe, not a scan for one of OUR call sites: it costs nothing at
    # build time and it is exactly what a future patch-break investigation would want if that module
    # is ever built back in. The `used` fields below therefore describe the module's INTENDED
    # consumers, not live call sites.
    {"name": "rawimage_create", "rva": 0x11489B0,
     "bytes": "89 54 24 10 89 4C 24 08 56 57 41 54 41 55 41 57",
     "used": "sfimage::create_resource (Render::RawImage::Create)"},
    {"name": "image_resource_ctor", "rva": 0xD5FEE0,
     "bytes": "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE",
     "used": "sfimage::create_resource (CS::ScaleformImageResource)"},
    {"name": "draw_image_into_clip", "rva": 0xD81640,
     "bytes": "48 8B C4 48 89 50 10 56 57 41 54 41 56 41 57 48",
     "used": "sfimage::draw_into"},
    # sfimage::ensure_child_clip no longer CALLS this: it dispatches through the value's own
    # ObjectInterface vtable (slot 29), which is correct for either VM. The anchor stays so
    # that a patch shifting the interface layout is still caught - if these bytes ever stop
    # matching, re-derive the slot index before trusting the icon path.
    {"name": "create_empty_movie_clip_as3", "rva": 0x10DFDA0,
     "bytes": "4C 8B DC 55 56 41 56 41 57 48 8B EC 48 83 EC 78",
     "used": "sfimage: expected occupant of ObjectInterface vtable slot 29"},
]

# Data addresses (vtables / singletons) shift on every patch and cannot be byte-anchored
# usefully; the code validates them at RUNTIME instead (e.g. WorldMapDialog is only trusted
# when *(MapArea - 0x27D8) equals its vtable). Listed here so the set is documented in one
# place and a future patch has a checklist.
RUNTIME_VALIDATED = [
    {"name": "KeyConfigDialog::vftable", "rva": 0x2B0AC40,
     "used": "capture_form_dialog (validates dlg+0x1268 - 0x1268 belongs to the dialog)"},
    {"name": "WorldMapDialog::vftable", "rva": 0x2B2D7D8,
     "used": "map_menu_window (validates MapArea - 0x27D8)"},
    {"name": "Scaleform MemoryFile::vftable", "rva": 0x2BA4C80,
     "used": "own_movie (a movie buffer is only re-pointed when the File carries this vtable)"},
]

# Struct offsets the menu code depends on. Not checkable statically at all - they are
# documented here and logged at runtime by the diagnostics so a broken update is obvious.
STRUCT_OFFSETS = [
    ("KeyConfigDialog", 0x120, "clip-root proxy (captions resolve from it)"),
    ("KeyConfigDialog", 0xA38, "GridControl (cursor)"),
    ("KeyConfigDialog", 0x1260, "device mode byte"),
    ("KeyConfigDialog", 0x1268, "MenuViewItemList (our rows)"),
    ("KeyConfigDialog", 0x1290, "CSMenuKeyConfig (bind manager)"),
    ("MenuWindow", 0x10, "sequence job holder (screen replaces host)"),
    ("MenuWindow", 0xA28, "child job holder (screen over host)"),
    ("MenuWindow", 0x1F8, "registered input commands vector begin"),
    ("WorldMapArea", -0x27D8, "back to WorldMapDialog"),
]


def _emit(rva, name, exe=None, n=16):
    """Print a ready-to-paste anchor entry for an address."""
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).parent))
    import config
    import pefile
    exe = exe or (config.GAME_DIR / "eldenring.exe")
    pe = pefile.PE(str(exe), fast_load=True)
    texts = sorted([s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"],
                   key=lambda s: s.VirtualAddress)
    t = texts[0]
    off = t.PointerToRawData + (rva - t.VirtualAddress)
    data = pe.__data__[off:off + n]
    print('    {"name": "%s", "rva": 0x%X,' % (name, rva))
    print('     "bytes": "%s",' % " ".join(f"{b:02X}" for b in data))
    print('     "used": "TODO"},')


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--emit", metavar="RVA", help="print an anchor entry for this RVA (hex)")
    ap.add_argument("name", nargs="?", default="unnamed")
    a = ap.parse_args()
    if a.emit:
        _emit(int(a.emit, 16), a.name)
    else:
        print(f"{len(ANCHORS)} anchors, {len(RUNTIME_VALIDATED)} runtime-validated data "
              f"addresses, {len(STRUCT_OFFSETS)} documented struct offsets")
