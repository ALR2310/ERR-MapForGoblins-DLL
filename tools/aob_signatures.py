"""Single source of truth for the AOB byte-pattern signatures our DLL scans for
in eldenring.exe at runtime (modutils::scan).

A game update that shifts code silently breaks these runtime scans -> map
injection just stops working with no diagnostic. tools/check_aobs.py resolves
each signature statically against the target exe at BUILD time so a break is
caught before we ship.

Keep this list in sync with the .cpp string literals. The drift guard
(check_drift, run by check_aobs.py) greps src/ for hex-pattern string literals
and fails the build if any literal is not covered by an entry here, so a newly
added AOB cannot silently go unchecked.

Each entry:
  name     - short stable id (used in the report / JSON key)
  pattern  - the AOB string exactly as it appears in source ('??' = wildcard)
  slot     - (disp_off, instr_len) to resolve a RIP-relative slot RVA from the
             match (mirrors modutils ScanArgs.relative_offsets), or None if the
             match address itself is the target (direct function entry).
  critical - True: a miss/ambiguous match FAILS the build (load-bearing).
             False: a miss WARNS only (cosmetic / niche feature).
  refs     - source location(s) the literal lives at (file:line).
  note     - optional caveat (e.g. contains a build-specific rel32).
"""

# NOTE: patterns are duplicated verbatim from the source string literals. The
# drift guard cross-checks src/ against this list, so a divergence is caught.
SIGNATURES = [
    # ---- Event flag API (loot/boss/grace gating) - load-bearing ----
    {
        "name": "is_event_flag",
        "pattern": "48 83 EC 28 8B 12 85 D2",
        "slot": None,
        "critical": True,
        "refs": ["goblin_markers.cpp:81", "goblin_kindling.cpp:96", "goblin_inject.cpp:1297"],
        "note": "IsEventFlag(). Match address is the function entry (called directly).",
    },
    {
        "name": "event_man_slot",
        "pattern": "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_markers.cpp:89", "goblin_kindling.cpp:103", "goblin_inject.cpp:1299"],
    },
    # ---- Live marker array chain (the whole point of the mod) ----
    {
        "name": "marker_chain_slot",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 8B 49 30 48 8D 55 5F",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_markers.cpp:171"],
    },
    {
        "name": "marker_container_vtable",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 5F 10 48 8D 05 ?? ?? ?? ??",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_markers.cpp:177"],
    },
    # ---- Param list (needed before any marker injection) ----
    {
        "name": "param_list_slot",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA 90",
        "slot": (3, 7),
        "critical": True,
        "refs": ["from/params.cpp:15"],
    },
    # ---- CSFeManImp singleton (HUD mode restore after our over-gameplay screen) ----
    # The store at the tail of the manager's init. Only needed to put CSFeManImp+0x78 (the HUD
    # visibility mode) back to the value it had before we pushed a screen; a miss just means the HUD
    # stays hidden until the player opens any native menu, so this is not load-bearing.
    {
        "name": "feman_slot",
        "pattern": "48 89 05 ?? ?? ?? ?? 48 8B 8B 80 00 00 00 48 85 C9 74 05 E8",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_stall_probe.cpp:8506"],
    },
    # ---- Message repository (all marker text) ----
    {
        "name": "msg_repository_slot",
        "pattern": "48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_messages.cpp:483"],
    },
    # ---- Collected-tracking (GEOF/WGM live hide) ----
    {
        "name": "geom_flag_slot",
        "pattern": "48 8B 3D ?? ?? ?? ?? 33 F6 48 85 FF 74 ?? 48 8B CF E8 ?? ?? ?? ?? 4C 8B 07",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_collected.cpp:118"],
    },
    {
        "name": "world_geom_man_slot",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 8D 53 10 E8 ?? ?? ?? ?? 4C 8B E8",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_collected.cpp:124"],
    },
    {
        "name": "world_chr_man_slot",
        "pattern": "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 98 08 E5 01 00",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_collected.cpp:139"],
    },
    {
        "name": "worldmap_build_markers",
        "pattern": "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF "
                   "FF FF 4C 8B F9 8B 42 34",
        "slot": None,
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
    },
    {
        "name": "worldmap_converter",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 "
                   "48 83 EC 20 33 DB 4D 8B F9 4D 8B E0 4C 8B EA 48 8B F1 48 39 99 80 02 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_worldmap_probe.cpp"],
    },
    # ---- Map hover/projection vtable gates (resolved from each vtable's ctor lea) ----
    {
        "name": "maparea_vtable",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 79 70 48 8B 07 48 8B CF FF 50 08",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "CS::WorldMapArea vtable (was RVA 0x2B2CB08). Gates the r8 the hover hook "
                "publishes -> drives the map-layer highlight rings + overlay projection. "
                "A miss just disables those, not core icons.",
    },
    {
        "name": "pin_vtable",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 06 48 89 BE 30 02 00 00",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "CS::WorldMapPointPinData vtable (was RVA 0x2AD6688). Gates the hovered pin "
                "-> marker hover-detect (manual hide / hover overlay). Miss disables hover.",
    },
    {
        "name": "node_get_writable_data",
        "pattern": "48 89 6C 24 20 56 41 54 41 56 48 83 EC 20 "
                   "48 8B F1 4C 8B C1 48 81 E6 00 F0 FF FF",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Render::Context GetWritableData(entry, changeFlags) - the copy-on-write + "
                "change-record step every visual property of a display object goes through "
                "(FUN_141157a70 v2.6.2.0). Identified via the Scaleform SDK: the projection "
                "setter calls it with 0x100000 = Change_State_ProjectionMatrix3D. The "
                "location emphasis uses it to fade other-map markers; a miss costs only "
                "the colour, size and draw order still apply.",
    },
    {
        "name": "record_materialize_driver",
        "pattern": "4C 8B DC 55 56 41 55 49 8D 6B D8 48 81 EC 10 01 00 00 "
                   "48 8D 41 48 4C 8B EA",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Timeline-record materialization walker (FUN_1411bf1b0 v2.6.2.0). "
                "The V3 factory runs it on its queued records; a miss limits native "
                "marker creation to the finite build-burst trickle.",
    },
    {
        "name": "detach_remove_at",
        "pattern": "40 57 48 83 EC 20 48 8B 41 18 48 8B F9 3B 90 E0 00 00 00 72 08 "
                   "33 C0 48 83 C4 20 5F C3",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Remove-child-at-index (FUN_1410c87c0 v1.16), the removal half of the "
                "reparent path FUN_1410c8440. Lever C bulk self-detach calls it at WMD "
                "dtor; a miss just disables self-detach (engine full teardown instead).",
    },
    {
        "name": "pin_factory_site",
        "pattern": "48 89 BE 30 02 00 00 48 8D 05 ?? ?? ?? ?? 48 89 86 38 02 00 00 "
                   "8B 45 08 89 86 40 02 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "Unique lea site inside the pin builder (FUN_14087ba70 v2.6.2.0, entry = "
                "site-0x112). Powers the own-pin native-tooltip proxy for V3 markers; a "
                "miss falls back to borrowing a live pin (label needs a prior real hover).",
    },
    # ---- GFX icon injection (no-gfx icon rendering) - load-bearing ----
    {
        "name": "gfx_ctor",
        "pattern": "45 33 C0 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? "
                   "C7 41 08 01 00 00 00 4C 89 41 10",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1156"],
    },
    {
        "name": "gfx_adddisp",
        "pattern": "4C 89 4C 24 20 4C 89 44 24 18 55 53 41 54 41 55 41 56 41 57 "
                   "48 8D 6C 24 F9",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1162"],
    },
    {
        "name": "gfx_lookup",
        "pattern": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 8B DA "
                   "48 8B F1 E8 F4 F8 FF FF",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1166"],
        "note": "Ends in a fixed rel32 call disp (E8 F4 F8 FF FF); if a game "
                "update shifts the call target this tail must be re-found.",
    },
    {
        "name": "gfx_registrar",
        "pattern": "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 "
                   "41 8B 00 48 8B F9 48 8B 49 38",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1174"],
    },
    {
        "name": "gfx_lossless",
        "pattern": "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 48 8B 99 "
                   "18 04 00 00",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1177"],
    },
    {
        "name": "gfx_spriteloader",
        "pattern": "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 99 18 04 "
                   "00 00 48 8B F9 48 85 DB",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1180"],
    },
    {
        "name": "gfx_tag_loop",
        "pattern": "4C 89 44 24 18 53 55 56 57 41 55 41 56 48 83 EC 68 48 8B AA 18 04 00 00 "
                   "49 8B F8 4C 8B EA 4C 8B F1 48 85 ED",
        "slot": None,
        "critical": True,
        "refs": ["goblin_own_movie.cpp"],
        "note": "The movie tag loop. Entered with the header read and no tag read yet, which is where "
                "our own bytes are handed to the parser - the only point that sees a movie whichever "
                "loader served the file (ME3 answers for its overrides above the engine's own opener). "
                "The pattern covers the three facts the route rests on: arg3 saved, reader at ctx+0x418, "
                "inline reader fallback at ctx+0x50.",
    },
    {
        "name": "game_crt_malloc",
        "pattern": "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 77 ?? 48 85 C9 B8 01 00 00 00 48 0F 44 D8 EB ?? E8 ?? ?? ?? ?? 85 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:169"],
        "note": "_malloc_base (game static-CRT malloc). We allocate Scaleform-owned buffers (frame array/tag arrays/tags) here so the game's _free_base frees them on the same _crtheap; a foreign heap there = corruption (v2.0.4 crashes).",
    },
    # ---- Active save slot (per-character manual hides) - non-critical ----
    {
        "name": "game_man_slot",
        "pattern": "48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_inject.cpp"],
        "note": "GameMan singleton getter; GameMan+0xAC0 = active Save Slot (profile "
                "index). Scopes manual marker-hides per character. A miss just disables "
                "per-slot scoping (hides fall back to none), not core icons.",
    },
    # ---- Toast fallback (cosmetic on-screen popup) - non-critical ----
    {
        "name": "show_tutorial_popup",
        "pattern": "48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_inject.cpp:1204"],
    },
    # ---- Kindling per-spirit liveness (niche feature) - non-critical ----
    {
        "name": "kindling_distance_vft",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 01 F6 C2 01 74 ?? "
                   "BA 40 00 00 00 E8 ?? ?? ?? ?? 90 48 8B C3 48 83 C4 30 5B C3 90 78 ??",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_kindling.cpp:181"],
    },
    {
        "name": "kindling_world_sfx_man_slot",
        "pattern": "48 8B 05 ?? ?? ?? ?? 48 8D 4D 98 48 89 4C 24 60",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_kindling.cpp:189"],
    },
    # ---- Fast map-open timing hooks (cosmetic perf) - non-critical ----
    {
        "name": "map_callsite_dispatcher",
        "pattern": "48 8B 89 18 01 00 00 E8 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? "
                   "BA 01 00 00 00 48 8B CF E8 ?? ?? ?? ?? BA 03 00 00 00 48 8B CF "
                   "E8 ?? ?? ?? ?? 48 8B 5C 24 30 B0 01",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:151"],
    },
    {
        "name": "map_refresh_hook",
        "pattern": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 20 "
                   "48 8B D9 48 8B 50 10 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:169"],
    },
    {
        "name": "map_wmd_dtor_hook",
        "pattern": "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 30 "
                   "48 C7 45 F0 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B F1 48 8D 05 "
                   "B7 A3 16 02",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:177"],
        "note": "Ends in a build-specific lea disp (B7 A3 16 02); expected to "
                "shift on a game update - non-critical by design.",
    },
    # ---- Stall-probe cost counters (debug_logging diagnostics) - non-critical ----
    {
        "name": "stallprobe_widget_a",
        "pattern": "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC "
                   "A0 00 00 00 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-marker map-widget virtual (v1.16 entry 0x1410dbb70, vtable slot "
                "0x142cbc840). Pass-through cost counter only; a miss disables that counter.",
    },
    {
        "name": "stallprobe_widget_b",
        "pattern": "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D "
                   "6C 24 D1 48 81 EC 90 00 00 00 48 8B 41 08",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-marker map-widget virtual (v1.16 entry 0x1410dbea0, vtable slot "
                "0x142cbc848) - the reopen pin-construction hot path. Counter only.",
    },
    {
        "name": "stallprobe_widget_c",
        "pattern": "48 89 5C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-marker map-widget virtual (v1.16 entry 0x1410dc260, vtable slot "
                "0x142cbc850). Counter only.",
    },
    {
        "name": "stallprobe_typed_find",
        "pattern": "40 53 41 55 41 57 48 83 EC 30 33 DB 4C 8B F9 89 5C 24 58 4C 8B EA 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Typed-find walk (v1.16 entry 0x14113feb0, TRUE entry - never the "
                "0x14113feda continuation). Counter only.",
    },
    {
        "name": "stallprobe_child_step",
        "pattern": "48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 20 8B A9 B8 00 00 00 4C 8B F1 "
                   "48 8B B1 E0 00 00 00 C1 ED 03 40 80 E5 01 48",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-frame child-step loop (v1.16 entry 0x1411d3980) - walks every "
                "marker widget per frame; the while-map-open fps cost. Counter only.",
    },
    {
        "name": "stallprobe_v3_native_insert_core",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 "
                   "48 C7 44 24 28 FF FF FF FF 49 8B D9 48 89 5C 24 20 49 8B F0 48 8B EA 48 8B F9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Scaleform DisplayObjContainer insert core (v1.16 entry 0x14113e970). "
                "Pass-through spy records the +0xd8 child-vector count, sorted index, "
                "and child during debug-logging sessions.",
    },
    {
        "name": "stallprobe_v3_custom_placeobject",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
                   "41 54 41 56 41 57 48 83 EC 20 45 8B 70 4C 4C 8B FA 4C 8B 61 08 "
                   "41 8B D6 49 8B D9 49 8B E8 48 8B F9 E8",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Scaleform PlaceObject add path (v1.16 entry 0x14113e7e0). The spy "
                "filters to the spike's MAP_ICON_CHARID_BASE/depth-24 pair and captures "
                "the ready custom DisplayObject.",
    },
    {
        "name": "stallprobe_v3_attach_movie_bridge",
        "pattern": "4C 8B DC 4D 89 4B 20 4D 89 43 18 55 56 41 57 49 8D 6B D8 "
                   "48 81 EC 10 01 00 00 48 8B 41 08 49 8B F1 48 8B 4A 28 4C 8B 78 18 "
                   "8B 81 90 00 00 00 83 E8 1F 83 F8 05",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Scaleform attachMovie DAPI bridge (v1.16 entry 0x1410e00c0). "
                "Debug-only spy correlates export/init parameters with the actual "
                "display-list parent observed by the nested high-level attach hook.",
    },
    {
        "name": "stallprobe_v3_high_level_attach",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 "
                   "48 8B DA 45 8B F0 48 8B 51 18 48 8B E9 48 8B 4B 38 8B 82 E0 00 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "High-level Scaleform display-object move/attach API (v1.16 entry "
                "0x1410c8440). One-shot V3 visual experiment uses its trampoline so "
                "old-parent removal, refcounts and child state remain engine-owned.",
    },
    {
        "name": "stallprobe_xform_get",
        "pattern": "48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 51 50 48 85 D2 0F",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Widget transform getter (v1.16 entry 0x14117e140): full matrix "
                "decompose when cache [this+0x50] is null, cheap copy when cached. "
                "Counter tests the refresh-fills-the-cache hypothesis.",
    },
    {
        "name": "stallprobe_item_proc",
        "pattern": "40 53 56 57 48 83 EC 20 48 8B F9 83 CA FF 48 81 C1 88 00 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx batch per-movie-slot processor (v1.16 entry 0x140d716a0), runs "
                "on a worker. Counter only.",
    },
    {
        "name": "stallprobe_next_capture",
        "pattern": "48 89 5C 24 20 57 41 56 41 57 48 83 EC 20 48 8B 19 4D 8B F8 4C 8B F2 48",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx render next-capture / display-tree changelist apply (v1.16 entry "
                "0x1411577b0). Counter only.",
    },
    {
        "name": "stallprobe_movie_display",
        "pattern": "48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 80 00 00 00 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx movie display/draw (v1.16 entry 0x14115cde0). Counter only.",
    },
    {
        "name": "stallprobe_batch_job",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 48 8B F2 "
                   "E8 01 C4 FE FF 83",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx render batch job entry (v1.16 0x140d7d720, worker thread). "
                "Counter only. Contains a build-specific rel32 (E8 01 C4 FE FF) - "
                "expected to shift on a game update; non-critical by design.",
    },
    {
        "name": "stallprobe_job_poll",
        "pattern": "48 83 EC 28 48 8B 09 48 85 C9 74 16 48 83 C1 10 E8 FB A6 08 01 85 C0 0F",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Generic 'async job done?' poll (v1.16 entry 0x140e811c0); the map UI "
                "thread waits in it ~140ms after close. Spy logs (caller, job vtable) "
                "during capture windows. Contains a build-specific rel32 (E8 FB A6 08 "
                "01) - expected to shift on a game update; non-critical by design.",
    },
    {
        "name": "map_placename_update",
        "pattern": "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 "
                   "48 C7 45 B7 FE FF FF FF 48 89 9C 24 08 01 00 00 48 8B 05 ?? ?? ?? ?? "
                   "48 33 C4 48 89 45 1F 49 8B F8",
        "slot": None,
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "Map dialog per-frame 'name the focused pin' fn; RDX = hovered "
                "WorldMapPointPinData. Drives marker hover-detect (manual hide / hover "
                "overlay). Non-critical: a miss just disables hover, not core icons.",
    },
    # ---- Scaleform DrawingContext primitives (solid-fill route-c spike) ----
    # Dev-only, gated on debug_logging. All non-critical: a miss disables only the
    # spike (no shipping path depends on them). ctx = MovieClip child vtbl+0x2a0.
    {
        "name": "dc_begin",
        "pattern": "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 0F B6 FA 48 8B 49 38 48 85 C9 0F 84",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx DrawingContext::begin (v2.6.x 0x119CB50): open/clear the drawing "
                "(returns 0 on a fresh ctx). Solid-fill route-c spike primitive.",
    },
    {
        "name": "dc_beginfill_solid",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 8B DA "
                   "F6 81 D0 00 00 00 10 48",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx DrawingContext::beginFill SOLID color (v2.6.x 0x119CC20): "
                "void(ctx, u32 argb) - 0xAARRGGBB, NO GPU texture (unlike beginBitmapFill). "
                "The key primitive proving a native panel can be drawn from our own objects.",
    },
    {
        "name": "dc_moveto",
        "pattern": "40 53 48 81 EC 80 00 00 00 33 C0 0F 29 74 24 70 0F 57 C0 48 89 44 24 3C "
                   "F3 0F 7F 44 24 24",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx DrawingContext::moveTo (v2.6.x 0x119D690): void(ctx, int xTwips, yTwips). "
                "Solid-fill route-c spike primitive.",
    },
    {
        "name": "dc_lineto",
        "pattern": "40 53 48 83 EC 40 F6 81 D0 00 00 00 08 48 8B D9 0F 29 74 24 30 0F 28 F2 "
                   "0F 29 7C 24 20 0F 28 F9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx DrawingContext::lineTo (v2.6.x 0x119D7A0): void(ctx, int xTwips, yTwips). "
                "Solid-fill route-c spike primitive.",
    },
    {
        "name": "dc_endfill",
        "pattern": "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 C0 C7 83 CC 00 00 00 00 00 "
                   "80 00 48 8B CB 48 89 43",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx DrawingContext::endFill (v2.6.x 0x119D650): void(ctx). Contains a "
                "build-specific rel32 (masked) - non-critical by design. Route-c spike primitive.",
    },
    {
        "name": "settings_build_job",
        "pattern": "48 8B C4 55 57 41 56 48 8D 68 A1 48 81 EC C0 00 00 00 48 C7 45 D7 FE FF FF FF "
                   "48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 41 0F B6 F8 "
                   "48 8B F2",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Build the settings-menu open job (v2.6.x 0x8087e0): movie 02_040_OptionSetting "
                "+ factory lambda. Pushed via pushJob to open settings by our F11. Dev proto.",
    },
    {
        "name": "settings_refmove_job",
        "pattern": "48 89 54 24 10 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B DA "
                   "C7 44 24 20 00 00 00 00 48 8B 09 48 89 0A 48 85 C9 74",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Ref-move a job handle (v2.6.x 0x7a7b60), from the confirm-dialog machinery. "
                "Used in the F11 settings-open push sequence. Dev proto.",
    },
    {
        "name": "settings_push_job",
        "pattern": "4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC A0 00 00 00 "
                   "48 C7 44 24 40 FE FF FF FF 48 89 9C 24 D0 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "pushJob (v2.6.x 0x7edfa0): push a menu job onto the active menu = open it. "
                "Reused from the confirm-dialog work for the F11 settings open. Dev proto.",
    },
    {
        "name": "map_subdialog_job_step",
        "pattern": "4C 89 44 24 18 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 D9 "
                   "48 81 EC A0 00 00 00 48 C7 45 A7 FE FF FF FF 4D 8B F0 4C 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Map sub-dialog job step (v2.6.x 0x7ad1c0): creates the sub-dialog's movie "
                "(Path B, desc @job+0x58) + factory + registers render (FUN_140733ef0(job+0x50)). "
                "The map's own 'menu movie over the map' mechanism. Dev diagnostic hook.",
    },
    {
        "name": "settings_movie_job_builder",
        "pattern": "40 55 56 57 41 56 41 57 48 8D 6C 24 C0 48 81 EC 40 01 00 00 "
                   "48 C7 44 24 50 FE FF FF FF 48 89 9C 24 88 01 00 00 48 8B 05 ?? ?? ?? ?? "
                   "48 33 C4 48 89 45 30 4D 8B F0 48 8B F2 48 8B F9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Generic settings movie-job builder (v2.6.x 0x808630): (out, owner, DESC, flag). "
                "All movie sub-opens (Graphic/Brightness) funnel through it; F11's 0x8087e0 is its "
                "sibling. Task #9 capture hook to find the sub-page push target/site. Dev proto.",
    },
    {
        "name": "opt_build_category",
        "pattern": "4C 8B DC 49 89 4B 08 57 48 81 EC B0 00 00 00 48 C7 44 24 28 FE FF FF FF "
                   "49 89 5B 18 48 8B F9 C7 44 24 20 00 00 00 00 49 8D 43 C0 49 89 43 10 "
                   "BA B0 AD 01 00 49 8D 4B C0 E8 ?? ?? ?? ?? 48 8B D8 8B 15 ?? ?? ?? ?? "
                   "83 C2 14 89 54 24 30 0F 57 C0",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Build a settings MenuOptionCategory (v2.6.x 0x807d60): label from FMG 110000 "
                "+ icon block. Used to append our own settings tab. Non-critical (dev proto).",
    },
    {
        "name": "opt_append_tab",
        "pattern": "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B FA "
                   "48 8B D9 48 8B 81 58 05 00 00 48 FF C0 48 83 F8 0A",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Append a MenuOptionCategory to the settings tab vector (v2.6.x 0x967b50); "
                "copy-constructs into the slot, cap 10 (count @listBase+0x558). Dev proto.",
    },
    {
        "name": "opt_show_page",
        "pattern": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 99 B8 00 00 00 "
                   "48 8B F1 48 63 C2 48 8B 44 C1 68 48 85 C0 75",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "OptionSetting show-page-for-category (v2.6.x 0x93b760): 10-slot page cache "
                "@pageCtl+0x68 indexed by category id UNCHECKED - hooked so our out-of-range "
                "tab id opens our own page instead of reading OOB. Dev proto.",
    },
    {
        "name": "opt_populate_page5",
        "pattern": "40 55 56 57 48 8D AC 24 50 FE FF FF 48 81 EC B0 02 00 00 "
                   "48 C7 44 24 70 FE FF FF FF 48 89 9C 24 E0 02 00 00 48 8B 05 ?? ?? ?? ?? "
                   "48 33 C4 48 89 85 A0 01 00 00 48 8B FA 48 8B D9 48 8D 4D F0",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Populate of the generic list page #5 (v2.6.x 0x957ef0), the page our tab "
                "borrows; swapped to our config-bool checkbox rows when armed. Dev proto.",
    },
    {
        "name": "opt_dtor_category",
        "pattern": "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 "
                   "48 8B F9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 41 08 48 89 44 24 48 "
                   "48 8D 59 10 48 89 5C 24 48 48 83 7B 20 08 72 0E",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Destruct a temp MenuOptionCategory (v2.6.x 0x8691a0) after append copied it "
                "into the vector (the slot holds its own deep copy). Dev proto.",
    },
    {
        "name": "option_top_dialog_dtor",
        "pattern": "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 "
                   "48 89 74 24 58 48 8B F1 48 8D 05 ?? ?? ?? ?? 48 89 01 80 B9 98 18 00 00 00 "
                   "74 73 E8 ?? ?? ?? ?? 90 48 8B F8 48 8D 8E 68 17 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "CS::OptionSettingTopDialog dtor (v2.6.x 0x966980). Detour clears the F11 "
                "anti-restack tracker when our menu closes. Dev proto.",
    },
    {
        "name": "option_top_dialog_ctor",
        "pattern": "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C8 F5 FF FF "
                   "48 81 EC 38 0B 00 00 48 C7 45 A0 FE FF FF FF",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "CS::OptionSettingTopDialog ctor (v2.6.x 0x966120). Detour observes/"
                "populates the native settings tab list (MenuViewItemList<MenuOptionCategory>, "
                "cap 10, count @dialog+0x1760). Dev-only tab-injection proto; a miss disables it.",
    },
    {
        "name": "memo_dialog_show_site",
        "pattern": "40 53 48 83 EC 50 48 8B 49 08 44 0F B6 81 30 0A 00 00 4C 8D 89 08 2F 00 00 "
                   "48 8D 81 44 0A 00 00 48 89 44 24 40 4C 8D 91 B0 2D 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Map memo-flow show site (v2.6.x 0x9d0a80): spawns WorldMapMemoSelectDialog "
                "with every creator arg derived from one host object *(ctx+8). Detour captures "
                "the host + scene for our own dialog opens. Dev proto (Task #5 Route B).",
    },
    {
        "name": "memo_dialog_ctor",
        "pattern": "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 30 FB FF FF "
                   "48 81 EC D0 05 00 00 48 C7 45 E8 FE FF FF FF 48 89 9C 24 18 06 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "CS::WorldMapMemoSelectDialog ctor (v2.6.x 0x9d21c0). Detour observes/"
                "repopulates the item list (inline @dlg+0x1248 stride 0x1A8, count @+0x3370, "
                "HARD cap 20). Dev proto (Task #5 Route B).",
    },
    {
        "name": "memo_dialog_couple_callsite",
        "pattern": "54 24 30 E8 ?? ?? ?? ?? 45 33 C9 44 8B C3 49 8B D6 48 8D 8E 78 0A 00 00 "
                   "E8 ?? ?? ?? ??",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Unique call site of the memo view-couple FUN_1409d1ff0 inside the memo ctor "
                "(the couple itself is a template with ~31 byte-identical instantiations, "
                "unresolvable by prologue; target extracted from the tail E8 disp). Re-wires "
                "the Scaleform ItemList to the item data. Dev proto (Task #5 Route B).",
    },
    {
        "name": "memo_dialog_creator",
        "pattern": "48 8B C4 57 48 83 EC 70 48 C7 40 D8 FE FF FF FF 48 89 58 08 48 89 68 10 "
                   "48 89 70 18 49 8B D9 41 0F B6 F8 48 8B F2 48 8B E9 4C 8B 05 ?? ?? ?? ??",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "WorldMapMemoSelectDialog creator stub (v2.6.x 0x9d45b0): allocates 0x3528 "
                "and runs the ctor. Called by us to open our own list over the map. Dev proto "
                "(Task #5 Route B).",
    },
    {
        "name": "memo_dialog_item_ctor",
        "pattern": "48 89 4C 24 08 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 60 "
                   "49 8B D9 48 8B F9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ??",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "WorldMapMemoSelectDialog::_Item ctor (v2.6.x 0x9d3890): "
                "(item, dialog, textObj, cbBlock). Used to build our own rows. Dev proto "
                "(Task #5 Route B).",
    },
    {
        "name": "memo_dialog_list_append",
        "pattern": "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B FA "
                   "48 8B D9 48 8B 81 28 21 00 00 48 FF C0 48 83 F8 14 76 18",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Memo item-list append (v2.6.x 0x9d7300): copy-constructs into the inline "
                "slot, HARD-asserts above 20 (count @listBase+0x2128). Dev proto (Task #5 "
                "Route B).",
    },
    {
        "name": "game_heap_alloc",
        "pattern": "49 8B 00 4D 8B C8 4C 8B C2 48 8B D1 49 8B C9 48 FF 60 50 90 F3 41 0F 58 C7 "
                   "FF C7 48 83 C3 08 F3 48 85 C9 74 41",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Game heap allocator thunk FUN_141eb9ed0(rcx=size, rdx=align, r8=allocObj) -> "
                "allocObj->vtable[0x50](allocObj, size, align). Used to allocate an "
                "OptionSettingTopDialog (0x18a0) for the Route A settings-over-map experiment. Dev proto.",
    },
    {
        "name": "alloc_singleton_global",
        "pattern": "4C 8B 05 ?? ?? ?? ?? 4C 89 40 18 8D 53 08 B9 A0 18 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "mov r8,[rip+DAT_143d87350] in the settings build-job factory: loads the heap "
                "allocator singleton ptr passed to the FUN_141eb9ed0 thunk. relative_offsets {{3,7}} "
                "resolves the global's address. Dev proto (Route A settings-over-map).",
    },
    {
        "name": "menu_man_update_task",
        "pattern": "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 "
                   "48 81 EC 98 00 00 00 48 C7 45 A7 FE FF FF FF 0F 29 70 A8 "
                   "0F 29 78 98 44 0F 29 40 88 44 0F 29 4C 24 50 44 0F 29 54 24 40 "
                   "48 8B FA 48 8B D9 0F 57 FF F3 0F 10 05 ?? ?? ?? ?? 0F 2E C7 "
                   "7A 16 75 14 B9 29 0A 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "CSMenuMan::updateTask (v2.6.x 0x766980), per-frame menu UI-thread entry. "
                "Detour reads the active menu (*(this+0x80)) for the dev menu-movie "
                "graphics probe; also the UI-thread beachhead for future native announce/"
                "dialogs. Non-critical: a miss just disables the probe.",
    },
    {
        "name": "dc_shapereset",
        "pattern": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 28 48 8B D9 45 33 C0 "
                   "BA 80 00 00 00 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx DrawingContext::shapeReset (v2.6.x 0x119D0C0): clear the shape "
                "accumulator (the proven open on a fresh ctx). Route-c spike primitive.",
    },
    {
        "name": "gfx_readtaginfo",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 "
                   "44 8B 41 50 4C 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "GFx SWF ReadTagInfo (v2.6.x 0x11BAB80): rcx=reader; fills taginfo "
                "{tagCode@0,streamStartPos@+4,tagLen@+8,bodyStartPos@+0xc} from the record "
                "header, pushes the tag END bound onto the reader nesting stack, advances to "
                "the body. Path-A settings-clip inject core (dev, debug_logging). A miss just "
                "disables the re-host inject.",
    },
    {
        "name": "gfx_tagalign",
        "pattern": "40 53 48 83 EC 20 FF 49 48 48 8B D9 8B 41 48 8B 54 81 40 E8",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "GFx SWF tag-align/pop (v2.6.x 0x11BAC70): rcx=reader; pops the tag END bound "
                "pushed by ReadTagInfo (dec reader+0x48). Path-A settings-clip inject core "
                "(dev, debug_logging). Non-critical: a miss just disables the re-host inject.",
    },
    {
        "name": "gfx_chardef_registrar",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 33 DB 49 8B F8 83 B9 E4",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "GFx char-def registrar (v2.6.x 0x11169D90): rcx=char registry owner, rdx=&charId "
                "u32, r8=CharacterDef; inserts into the owner's char hashmap (+0x180). Dev-only "
                "diagnostic hook (debug_logging) for Path-A: logs which registry object native + "
                "injected char defs land in. Non-critical: a miss just disables the diagnostic.",
    },
    {
        "name": "gfx_placeobject2_exec",
        "pattern": "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? "
                   "F6 C3 01 74 0D BA 10 00 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "PlaceObject2::Execute (v2.6.x 0x140EE3D10): (thisTag, ctx, frame). Dev-only Path-A "
                "hook (debug_logging) - records the map _root's live DisplayObjContext in a TLS so the "
                "settings-root placement can Execute against it. Contains a build-specific rel32 (E8 "
                "wildcarded). Non-critical: a miss disables the Task-4 experiment only.",
    },
    {
        "name": "gfx_name_resolver",
        "pattern": "4C 89 44 24 18 4C 89 4C 24 20 55 53 56 57 41 56 41 57 48 8D 6C 24 D1 "
                   "48 81 EC C8 00 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "GFx display-tree name resolver FUN_14074a2f0(scene, out, path): the dialog ctor binds "
                "clips by name through this. Dev-only Path-A hook (debug_logging) - captures the map "
                "scene ptr + logs the ctor's name lookups + drives the verify-probe. Non-critical.",
    },
]


def _tokens(pattern):
    """Normalize a pattern to a space-joined token string (collapses the
    whitespace introduced by C++ adjacent-string-literal concatenation)."""
    return " ".join(pattern.split())


# Pre-normalized patterns for substring coverage checks.
_NORM_PATTERNS = [_tokens(s["pattern"]) for s in SIGNATURES]

import re
from pathlib import Path

# A run of >=6 byte/wildcard tokens - the same shape used to enumerate AOBs.
_HEX_RUN = re.compile(r"(?:[0-9A-Fa-f]{2}|\?\?)(?: (?:[0-9A-Fa-f]{2}|\?\?)){5,}")
# A whole string literal that is ENTIRELY byte/wildcard tokens (an AOB fragment).
_AOB_LITERAL = re.compile(r"^(?:[0-9A-Fa-f]{2}|\?\?)(?: (?:[0-9A-Fa-f]{2}|\?\?))*$")
_STRING_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def check_drift(src_dir):
    """Grep src/ for AOB-shaped string literals and return a list of
    (file:line, literal) fragments NOT covered by any SIGNATURES pattern.

    A C++ AOB may be split across adjacent string literals, so each source
    fragment is checked as a SUBSTRING of some normalized signature pattern.
    An empty return list means the list is in sync with the source."""
    src_dir = Path(src_dir)
    uncovered = []
    for path in sorted(src_dir.rglob("*.[ch]pp")):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in _STRING_LITERAL.finditer(line):
                lit = m.group(1).strip()
                # Only consider literals that ARE an AOB fragment (all hex/??
                # tokens) and long enough to be a real pattern piece.
                if not _AOB_LITERAL.match(lit):
                    continue
                toks = _tokens(lit)
                if len(toks.split()) < 4:
                    continue
                if not any(toks in norm for norm in _NORM_PATTERNS):
                    rel = path.relative_to(src_dir.parent) if src_dir.parent in path.parents else path
                    uncovered.append((f"{rel}:{lineno}", toks))
    return uncovered
