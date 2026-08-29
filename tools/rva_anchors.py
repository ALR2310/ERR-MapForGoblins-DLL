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
and paste the printed entry, then regenerate its pattern (below).

Fields:
  name  - stable id, matches the helper's role in the code
  rva   - the address the C++ uses as `base + rva`
  bytes - the bytes that must live at that RVA ('??' = wildcard, for rip-relative
          displacements and rel32 branch targets - the operands that differ per build)
  used  - where it is called from, so a break points at the right code

THE PATTERNS ARE GENERATED, NOT HAND-PICKED (since report 31). They used to be a flat
16-byte prefix, and 33 of 44 of those matched more places than they identified -
clip_proxy_dtor's matched 3638 - so the resolver was choosing between copies on evidence
that could not tell them apart, and on one player's build it called a stranger. Regenerate
with `py scratch/anchor_patterns.py --write`: it grows each pattern instruction by
instruction until it matches exactly ONCE, wildcards the build-specific operand bytes, and
stops at the function's end. ANCHOR_KIND below records the outcome per anchor:
  pin      - the pattern matches exactly once on every exe build we hold. It identifies its
             function, so the resolver trusts it and uses it to establish the local shift.
  follower - byte-identical copies exist (menu_row_item_empty has 290), so no pattern can
             pick one out. The resolver may only place these RELATIVE to the pins around
             them, never on their own evidence.
"""

ANCHORS = [
    # ---- clip / text primitives (goblin_stall_probe.cpp draw + caption helpers) ----
    {"name": "clip_resolve_child", "rva": 0x74B140,
     "bytes": "4C 89 44 24 18 4C 89 4C 24 20 55 53 56 57 41 56 41 57 48",
     "used": "draw_our_row / set_form_captions / prepare_form_layout / draw_row_icon"},
    {"name": "clip_set_text_html", "rva": 0x74AE50,
     "bytes": "40 53 48 83 EC 20 48 8B 09 48 8B DA 48 8B 01 FF 50 08",
     "used": "draw_our_row / set_form_captions (SetText with isHtml=1)"},
    {"name": "clip_set_visible", "rva": 0x734190,
     "bytes": "40 53 48 83 EC 20 48 8B 01 0F B6 DA FF 50 08 8B",
     "used": "draw_our_row / prepare_form_layout / draw_row_icon"},
    {"name": "clip_is_valid", "rva": 0x733FA0,
     "bytes": "48 83 EC 28 48 8B 01 FF 10 F6",
     "used": "every resolve site (guards a missing clip)"},
    {"name": "clip_set_gray", "rva": 0x734030,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 0F B6 FA 48 8B D9 FF 50",
     "used": "row style (Grayout frame)"},
    {"name": "clip_set_scale", "rva": 0x7340D0,
     "bytes": "40 53 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 0F 29 B4 24 80 00 "
               "00 00 0F",
     "used": "draw_row_bar_clip (experimental graphic bar)"},
    {"name": "clip_goto_frame_name", "rva": 0x74A830,
     "bytes": "48 89 54 24 10 48 83 EC 28 48 8B 09 48 8B 01 FF 50 08 8B 48 20 81 E1 8F "
               "00 00 00 83 F9 02 72 1A",
     "used": "row style frames Normal/Grayout/PadCategory"},
    {"name": "clip_goto_frame_num", "rva": 0x74A7D0,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B 09 8B",
     "used": "prepare_form_layout (BG wide panel) / draw_row_icon (icon frame)"},
    {"name": "clip_proxy_dtor", "rva": 0xD81590,
     "bytes": "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8D 05 ?? ?? "
               "?? ?? 48 89 01 48 8D 59 08 8B",
     "used": "every resolve site (releases the proxy)"},

    # ---- row list machinery (the hooks + the rebuild path) ----
    {"name": "menu_row_build_dispatch", "rva": 0x869580,
     "bytes": "44 0F BE 42 08",
     "used": "build_items_detour hook + refresh_form_view"},
    {"name": "menu_row_render", "rva": 0x8684D0,
     "bytes": "48 8B C4 55 57 41 56 48 8D 68 A1 48 81 EC E0 00 00 00 48 C7 45 8F FE FF "
               "FF FF 48 89 58 18",
     "used": "row_render_detour hook (item vt+0x8)"},
    {"name": "menu_row_decide", "rva": 0x942340,
     "bytes": "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 38 FD",
     "used": "form_decide_detour hook"},
    {"name": "menu_view_refresh", "rva": 0x943830,
     "bytes": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F1 48 8D 91",
     "used": "refresh_form_view"},
    {"name": "menu_row_vec_clear", "rva": 0x869F10,
     "bytes": "4C 89 44 24 18 56 57 48 83 EC 28 49 8B C1 48 8B FA 48 8B F1 4C 3B 41 08 "
               "75 1D 48 3B 41 10 75 17 E8 ?? ?? ?? ?? 48 8B 44 24 50 48 89 07 48 8B C7 "
               "48 83 C4 28 5F 5E C3 4C 3B C0 74 6D 48 8B 56 10 33 C9 44 0F B6 C9 48 89 "
               "5C 24 40 48 8B C8 48 89 6C 24 48 4C 89 74 24 20 E8 ?? ?? ?? ?? 48 8B 6E "
               "10 4C 8B F0 48 8B D8 48 3B C5 74 18 0F 1F 40 00 4C",
     "used": "build_our_form_items"},
    {"name": "menu_row_vec_append", "rva": 0x869FD0,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 49 10 48 3B D1 73 "
               "4B 48 8B 43 08 48 3B C2 77 42 48 2B F8 48 B8 67 66 66 66 66 66 66 66 48 "
               "F7 EF 48 8B FA 48 C1 FF 05",
     "used": "build_our_form_items"},
    {"name": "menu_row_item_ctor", "rva": 0x867F70,
     "bytes": "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B C2 48 8B "
               "D9 48 8D 0D ?? ?? ?? ?? 48 89 0B 48 8D 0D ?? ?? ?? ?? 48 89 0B 44",
     "used": "build_our_form_items (real row item)"},
    {"name": "menu_row_item_empty", "rva": 0x8696B0,
     "bytes": "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B D9 C7 44 "
               "24 20 00 00 00 00 E8 ?? ?? ?? ?? 90 C7 44 24 20 01 00 00 00 48 8B C3 48 "
               "83 C4 30 5B C3",
     "used": "build_our_form_items (right-column filler)"},
    {"name": "grid_cursor_get", "rva": 0x73AC70,
     # The whole function is these 7 bytes; anything past the ret is the next build's padding
     # and killed the match on 2.6.0/2.2.3 (the body itself sat at the cluster shift on both).
     "bytes": "8B 81 D4 00 00 00 C3",
     "used": "selected_model_index (GridControl cursor)"},

    # ---- the snapshot-slot class, for its VTABLE ----
    # v3_child_releasable identifies a child's timeline snapshot slot by its vtable. That vtable
    # lives in .rdata, so no byte pattern reaches it and the class carries no RTTI either - it was
    # a baked address, correct on 2.6.2/2.6.1/2.6.0 and WRONG on 2.7.0, 2.2.3 and 2.2.0, which
    # silently disabled the generation release and leaked the Scaleform arena until the engine
    # panicked. These two slot FUNCTIONS are code, so the resolver finds them anywhere; the vtable
    # is then the one place in .rdata where both sit at their own slot index (goblin_anchors.cpp,
    # vtable_with). Verified on all six builds by scratch/verify_slot_layout_all.py.
    {"name": "snapshot_slot_vt_fn4", "rva": 0x11FA7F0,
     "bytes": "48 8B C4 48 89 58 08 48 89 78 10 55 48 8D 68 A1 48 81 EC A0",
     "used": "v3_child_releasable (snapshot-slot vtable, slot 4)"},
    {"name": "snapshot_slot_vt_fn6", "rva": 0x11CDEA0,
     "bytes": "49 3B D0 0F 84 ?? ?? ?? ?? 48 8B C4",
     "used": "v3_child_releasable (snapshot-slot vtable, slot 6)"},

    # ---- screen open / job plumbing ----
    {"name": "keyconfig_form_build", "rva": 0x808770,
     "bytes": "4C 8B DC 53 48 81 EC B0 00 00 00 49 C7 43 88 FE FF FF FF 48 8B 05 ?? ?? "
               "?? ?? 48 33 C4 48 89 84 24 A0 00 00 00 48 8B D9 49 89 4B 90 C7 44 24 20 "
               "00 00 00 00 49 C7 43 E0 00 00 00 00 49 8D 43 A8 49 89 43 98 49 8D 43 A8 "
               "48 89 44 24 30 48 8D 05 ?? ?? ?? ?? 49 89 43 A8 48 8D 05 ?? ?? ?? ?? 49 "
               "89 43 A8 45 88 43 B0 49 8D 43 A8 49 89 43 E0 C7 44 24 30 08",
     "used": "open_keyconfig_form (movie 02_160 job)"},
    {"name": "job_ref_convert_a", "rva": 0x7A8CB0,
     "bytes": "4C 8B DC 49 89 53 10 53 56 57 48 83 EC 70",
     "used": "open_keyconfig_form ref chain"},
    {"name": "job_ref_convert_b", "rva": 0x7A89E0,
     "bytes": "48 89 54 24 10 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B DA C7 44 "
               "24 20 00 00 00 00 48 8B 09 48 89",
     "used": "open_keyconfig_form ref chain"},
    {"name": "job_holder_store_seq", "rva": 0x7AA0D0,
     "bytes": "48 89 54 24 10 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 "
               "48 8B FA 48 8B D9 48 8B 0A 48 85 C9 74 28 48 8D 44 24 50 48 89 44 24 58 "
               "48 89 4C 24 50 48 83 C1 08 E8 ?? ?? ?? ?? 90 48 8D 4B 08",
     "used": "open_keyconfig_form (sequence slot +0x10)"},
    {"name": "job_holder_store_child", "rva": 0x7AA2E0,
     "bytes": "4C 89 44 24 18 48 89 54 24 10 56 57 41 56 48 83 EC 30 48 C7 44 24 28",
     "used": "open_keyconfig_form (child slot +0xA28)"},
    # The two refcount thunks are one instruction each; bytes past the ret are data that
    # changes per build. The exe holds many byte-identical copies of each - any copy is
    # semantically the same call, so a nearest-match rebase is always safe for these.
    {"name": "refcount_addref", "rva": 0x1EBBFC0,
     "bytes": "B8 01 00 00 00 F0 0F C1 01 C3",
     "used": "job ref dance"},
    {"name": "refcount_unref", "rva": 0x1EBC000,
     "bytes": "83 C8 FF F0 0F C1 01 C3",
     "used": "job ref dance / release_job_ref"},
    {"name": "list_row_path_build", "rva": 0x737E10,
     "bytes": "4C 8B DC 57 48 81 EC 90 00 00 00 49 C7 43 90 FE FF FF FF 49 89 5B 20 48 "
               "8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 48 8B FA 48 8B D9 49 "
               "89 53 98 C7 44 24 20 00 00 00 00 45",
     "used": "row_path_detour (gives the slot index for row icons)"},
    {"name": "clip_set_pos", "rva": 0x734080,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 41 8B D8 8B FA FF 50 08 8B 48 20 "
               "81 E1 8F 00 00 00 83 F9 02 72 16",
     "used": "draw_row_icon (shifts the icon strip)"},

    # ---- movie transform (goblin_own_movie.cpp) ----
    # The engine's movie OPENER is deliberately not used and has no anchor: whether it is called for a
    # given movie is the mod loader's decision (ModEngine3 answers for the files it overrides before the
    # engine gets there, so of Convergence's 22 overridden movies it fired for 0). The transform runs on
    # the tag loop instead - AOB gfx_tag_loop - which every movie goes through whoever served the file.
    # Not called - it is the constructor whose body DEFINES the memory-file layout the transform
    # relies on (+0x18 buffer, +0x20 size, +0x24 position, +0x08 refcount). If these bytes stop
    # matching, re-read the layout before trusting the transform.
    {"name": "memory_file_ctor", "rva": 0xCE9280,
     "bytes": "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 "
               "48 89 6C 24 50 48 89 74 24 58 41 8B F1 49",
     "used": "own_movie: source of the memory-file field offsets"},

    # ---- independent icon path (goblin_sfimage.cpp): NOT COMPILED as of 2026-07-31 ----
    # goblin_sfimage.cpp is out of CMakeLists.txt (its last external caller went with the own-draw
    # icon routes), so the four anchors below no longer guard code that ships. They are KEPT rather
    # than deleted, unlike the stale entries pruned from aob_signatures.py, because an anchor is a
    # byte-identity check against the exe, not a scan for one of OUR call sites: it costs nothing at
    # build time and it is exactly what a future patch-break investigation would want if that module
    # is ever built back in. The `used` fields below therefore describe the module's INTENDED
    # consumers, not live call sites.
    {"name": "rawimage_create", "rva": 0x114A7B0,
     "bytes": "89 54 24 10 89 4C 24 08 56 57 41 54",
     "used": "sfimage::create_resource (Render::RawImage::Create)"},
    {"name": "image_resource_ctor", "rva": 0xD61C20,
     "bytes": "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 "
               "49 8B C0 48 8B DA 48 8B F9 45",
     "used": "sfimage::create_resource (CS::ScaleformImageResource)"},
    {"name": "draw_image_into_clip", "rva": 0xD83380,
     "bytes": "48 8B C4 48 89 50 10 56 57 41 54 41 56 41 57 48 83 EC 60 48 C7 40 98",
     "used": "sfimage::draw_into"},
    # sfimage::ensure_child_clip no longer CALLS this: it dispatches through the value's own
    # ObjectInterface vtable (slot 29), which is correct for either VM. The anchor stays so
    # that a patch shifting the interface layout is still caught - if these bytes ever stop
    # matching, re-derive the slot index before trusting the icon path.
    {"name": "create_empty_movie_clip_as3", "rva": 0x10E1BA0,
     "bytes": "4C 8B DC 55 56 41 56 41 57 48 8B",
     "used": "sfimage: expected occupant of ObjectInterface vtable slot 29"},

    # ---- added 2026-08-05 with the runtime rebase resolver (goblin_anchors.cpp) ----
    # Every code address the DLL calls as base+RVA must be in this table: the resolver verifies
    # the bytes at the baked address once at startup and, on a shifted exe (downpatch, future
    # patch), re-finds each anchor by its bytes near the shift its neighbours resolved at. These
    # eleven were called as raw literals with no anchor at all until today's sweep.
    {"name": "form_update_heartbeat", "rva": 0x9406E0,
     "bytes": "4C 8B DC 57 48 81 EC 90 00 00 00 49 C7 43 98 FE FF FF FF 49 89 5B 20",
     "used": "form_update_detour hook (dialog liveness heartbeat)"},
    {"name": "clip_set_pos_i", "rva": 0x733FF0,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 41 8B D8 8B FA FF 50 08 8B 48 20 "
               "81 E1 8F 00 00 00 83 F9 02 72 0D",
     "used": "maphover own-tip (plain proxy setPosition, int pair; prologue identical to "
             "clip_set_pos - the resolver's shift prior is what tells them apart)"},
    {"name": "panel_set_visible", "rva": 0x7368B0,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 0F B6 DA 48 8B F9 38 51 69",
     "used": "maphover game-popup wrapper show/hide"},
    {"name": "panel_set_pos_f", "rva": 0x736530,
     "bytes": "40 53 48 83 EC 30 F3 0F 10 49",
     "used": "maphover game-popup wrapper position (float pair)"},
    {"name": "clip_set_text_color", "rva": 0x74B020,
     "bytes": "89 54 24 10 48 83 EC 28 48 8B 09 48 8B 01 FF 50 08 8B 48 20 81 E1 8F 00 "
               "00 00 83 F9 02 72 2D",
     "used": "maphover own-tip text colour"},
    {"name": "caption_text_ctor", "rva": 0x7617C0,
     "bytes": "48 89 4C 24 08 53 48 83 EC 40 48 C7 44 24 38 FE FF FF FF 48 8B D9 C7 44 "
               "24 30 00 00 00 00 48 8D 05 ?? ?? ?? ?? 48 89 44 24 60 48 8D 05 ?? ?? ?? "
               "?? 48 89 44 24 20 4C 8D 0D ?? ?? ?? ?? 44 8B C2 48 8D 54 24 60 E8 ?? ?? "
               "?? ?? 90 C7 44 24 30 01 00 00 00 48 8B C3 48 83 C4 40 5B C3",
     "used": "set_form_captions (engine text-value ctor)"},
    {"name": "caption_register", "rva": 0x745390,
     "bytes": "40 53 55 56 57 48 81 EC 88 01 00 00 48 C7",
     "used": "set_form_captions (register the text value on the movie)"},
    {"name": "caption_pack", "rva": 0x745FC0,
     "bytes": "48 89 54 24 10 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B DA C7 44 "
               "24 20 00 00 00 00 48 83 C1 40",
     "used": "set_form_captions (pack the entry for the caption slot)"},
    # One-instruction holder tests: bytes past the ret are the next build's padding (the
    # grid_cursor_get lesson), so these patterns stop at the ret.
    {"name": "job_holder_test_seq", "rva": 0x7AA0B0,
     "bytes": "48 83 79 30 00 75 09",
     "used": "open_screen (is the sequence slot free)"},
    {"name": "job_holder_test_child", "rva": 0x7AA080,
     "bytes": "83 39 01 0F 97",
     "used": "open_screen (is the child slot busy)"},
    # The two functions the input-trigger vtable check reads OUT OF a candidate vtable
    # (vt[0] and vt+0x38): anchoring the functions keeps that content compare working on a
    # shifted exe with no data-address anchor. trigger_vt_slot0_fn is truncated before a
    # call rel32 whose displacement is build-specific.
    {"name": "trigger_vt_slot0_fn", "rva": 0x735100,
     "bytes": "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B F8 48 85 DB "
               "74 19 4C 8B 03 33 D2 48 8B CB 41 FF 50 08 4C 8B 07 48 8B D3 48 8B CF 41 "
               "FF 50 68 48 8B 5C 24 30 48 83 C4 20 5F C3",
     "used": "find_input_trigger (vt[0] content compare)"},
    {"name": "trigger_vt_slot7_fn", "rva": 0x746A20,
     "bytes": "40 57 48 81 EC D0 08 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 E0 08 "
               "00 00 48 8B F9 48 81",
     "used": "find_input_trigger (vt+0x38 content compare)"},

    # NOT here, deliberately: rm2_addsnapshot / po3_addsnapshot (capture_tag_vtables'
    # +0x30 identity compare, report 30). An anchor is a BYTE-identity check in the LIVE
    # process, and rm2_addsnapshot is a function our own rm2exec MinHook has already
    # re-prologued by the time the table resolves - so the anchor failed AT HOME on the
    # supported exe and the resolver rebased it onto a byte-twin +0x12be10 away (measured
    # 2026-08-06: capture dead, 0 pulses, every marker dropped). Any function this DLL
    # hooks must never be anchored unless the resolve is guaranteed to run first. Those
    # two are resolved by AOB instead (see po3_addsnapshot in aob_signatures.py; the rm2
    # address is the rm2exec hook's own scan result).
]

# Which anchors their own bytes can identify, measured across every exe build we hold
# (2.6.2 / 2.6.1 / 2.6.0 / 2.2.3 / 2.2.0). Written by scratch/anchor_patterns.py --write;
# documentation, not input - the resolver counts matches in the LIVE exe and classifies each
# anchor from that, so an unseen build where a pin turns ambiguous degrades to a follower
# instead of being trusted on a stale flag.
# <kinds>
ANCHOR_KIND = {
    "clip_resolve_child": "pin",
    "clip_set_text_html": "pin",
    "clip_set_visible": "pin",
    "clip_is_valid": "pin",
    "clip_set_gray": "pin",
    "clip_set_scale": "pin",
    "clip_goto_frame_name": "pin",
    "clip_goto_frame_num": "pin",
    "clip_proxy_dtor": "pin",
    "menu_row_build_dispatch": "pin",
    "menu_row_render": "pin",
    "menu_row_decide": "pin",
    "menu_view_refresh": "pin",
    "menu_row_vec_clear": "pin",
    "menu_row_vec_append": "pin",
    "menu_row_item_ctor": "pin",
    "menu_row_item_empty": "follower",
    "grid_cursor_get": "follower",
    "snapshot_slot_vt_fn4": "pin",
    "snapshot_slot_vt_fn6": "pin",
    "keyconfig_form_build": "pin",
    "job_ref_convert_a": "pin",
    "job_ref_convert_b": "pin",
    "job_holder_store_seq": "pin",
    "job_holder_store_child": "pin",
    "refcount_addref": "follower",
    "refcount_unref": "follower",
    "list_row_path_build": "pin",
    "clip_set_pos": "pin",
    "memory_file_ctor": "pin",
    "rawimage_create": "pin",
    "image_resource_ctor": "pin",
    "draw_image_into_clip": "pin",
    "create_empty_movie_clip_as3": "follower",
    "form_update_heartbeat": "pin",
    "clip_set_pos_i": "pin",
    "panel_set_visible": "pin",
    "panel_set_pos_f": "pin",
    "clip_set_text_color": "pin",
    "caption_text_ctor": "follower",
    "caption_register": "pin",
    "caption_pack": "pin",
    "job_holder_test_seq": "pin",
    "job_holder_test_child": "pin",
    "trigger_vt_slot0_fn": "follower",
    "trigger_vt_slot7_fn": "pin",
}
# </kinds>

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
