#pragma once

// Runtime icon-frame injector + (dev-only) RE probe. setup() ALWAYS arms the injection hooks
// (register our DefineBitsLossless2 bitmaps, append a frame per icon to worldmap SpriteDef-171,
// remap markers to the injected frames) so icons render without a shipped/modified gfx.
//
// What debug_logging does and does NOT do (the .cpp has carried the correct version since
// 2026-07-30; this header still claimed the old one): it is LOGGING ONLY. It does not arm the
// RM2::Execute hook - that hook is armed by the kNativeMarkers build variant and carries the
// marker-factory pulse, so it is live in every shipping build and is not a trace. The live
// SpriteDef / frame dumps additionally require the compile key MFG_DUMP_FRAMES, which is not
// defined anywhere in the tree.
// See docs/research_no_gfx_icons.md.
#include <cstdint>

namespace goblin::gfx_probe
{
    void setup();

    // menu_icons_ready() was declared here and returned a flag nothing ever set, so it answered
    // "no icons" forever - while the menu icons were in fact working, spliced by goblin_own_movie
    // on the parse. It had no callers, which is the only reason the false answer went unnoticed.

    /// The GAME's own CRT malloc (resolved by AOB), for buffers the ENGINE may later free: it uses the
    /// same heap the game's own free() does, so nothing rests on our allocator matching by luck.
    /// Returns null if the allocator could not be resolved.
    void *game_alloc(size_t bytes);
    // For buffers the ENGINE will free through its own DL heap allocator, whose Free is
    // _aligned_free: that reads a back-pointer at (p & ~7) - 8 and frees THAT, so such a buffer
    // must come from the matching _aligned_malloc. The FMG slot buffers are the case; the
    // Scaleform tag objects are not (nothing frees those).
    void *game_aligned_alloc(size_t bytes);

    // PREFERRED over game_aligned_alloc for any buffer HANDED INTO AN ENGINE SLOT the engine
    // later releases through DLNew's operator delete (the expanded WMP/TutorialParam tables,
    // the PlaceName FMG). That delete asks the DL range table who owns the pointer and only
    // falls back to the _aligned_free-compatible allocator when NO arena claims it - and a mod
    // host (ME3's mod host, ModEngine2) substitutes exactly that fallback object, whose free
    // then walks ITS metadata for a pointer it never issued: AV in me3_mod_host (reports
    // 25/27/28/37) / delayed 0xC0000374 under ME2. Allocating from the arena that owns
    // `neighbor` (the ORIGINAL buffer we are replacing) makes the range lookup SUCCEED, so the
    // release never reaches the substituted fallback - symmetric under every host, including a
    // host that substituted the arena allocator itself (then IT served this allocation too).
    // 16-aligned, zeroed. Returns null when the owner cannot be resolved or refuses the size;
    // the caller then falls back to game_aligned_alloc (the status quo: correct unhosted,
    // broken only under a substituted fallback).
    void *dl_alloc_like(const void *neighbor, size_t bytes);

    /// A movie's own file name, read from the definition its load context points at (ctx+0x38). This is
    /// how a parse is told apart from another movie's: the field reads the same whether the file came
    /// out of an archive or out of a mod's folder. false = no name could be established, and a caller
    /// must then not assume anything about which movie this is.
    bool movie_name(void *ctx, char *out, size_t cap);

    // Injected iconId (1-based frame appended at worldmap load) for a category's source gfx iconId,
    // or 0 if that source icon wasn't injected / before load. goblin_inject's remap_injected_icons
    // points every marker at injected_iconid(its baked iconId) so our embedded bitmaps render without
    // a shipped custom gfx, on any base.
    uint32_t injected_iconid(int srcIconId);
    // Reverse the per-load frame remap. Returns the original generated icon id,
    // or -1 when the runtime frame is not one of ours.
    int source_iconid(uint32_t runtimeIconId);
    // Character id of the injected bitmap used by a source icon. Unlike the
    // 1-based frame id returned by injected_iconid(), this is the resource id
    // encoded in the frame's PlaceObject tag. Used by the deferred V3 factory
    // to identify the real DisplayObject when Scaleform materialises it.
    // The ids are chosen at runtime, per world-map parse, above every id that movie registers (no
    // build-time number exists). 0 = no ids yet (the map's icons are not in), or this icon was left
    // out because its id did not resolve to our bitmap right after registration, or stopped doing so
    // by the end of the parse (see after_parse) - callers skip on 0.
    uint32_t native_character_id(int sourceIconId);

    // Called by goblin_own_movie's parse hook as the engine's tag loop RETURNS for any movie (loader
    // thread; the movie is parsed and not yet instanced). For the world map's parse - the load context
    // AND the load data our ids went into - it checks once that every id still resolves to the bitmap
    // we registered. An icon id some later registration took over is logged and that icon left out of
    // the marker layer; the logo is only logged. If (nearly) all ids read as changed - counting the
    // ones already missing at registration - the check itself is taken as failed and no icon is left
    // out (those turned off at registration are put back). The ids are never moved. For any other
    // movie this is two compares.
    void after_parse(void *loadData, void *ctx);
    // Injected iconId for the anon "?" (its source = generated::ANON_ICON_ID). 0 before load.
    uint32_t anon_dynamic_iconid();
    // Lowest / highest 1-based frame id we appended this worldmap load (0,0 before load). Used by
    // remap_injected_icons to flag a marker whose iconId is ALREADY an injected frame id (a double-
    // remap = two DLL instances injected; resolves to the wrong frame when the ranges overlap).
    void injected_iid_range(uint32_t &lo, uint32_t &hi);
    // Called periodically from the DLL's background watcher thread (NOT the overlay Present hook, so it
    // runs regardless of menu_enabled): the patched-slot audit, the map-stall sampler and (when
    // debug_logging) read-only diagnostics. It touches no character ids: those are chosen and checked
    // on the loader thread during the world map's parse (injection and after_parse), not here.
    void tick();

    // [tickcost] (debug_logging only): what the last tick() on the CALLING thread spent per block, in
    // QueryPerformanceCounter ticks. Zero when debug_logging is off.
    struct TickSplit
    {
        int64_t msgcheck = 0; // check_patched_slots
        int64_t stall = 0;    // the map-stall sampler
        int64_t layer = 0;    // the debug dialog scan
        // Reported as gfx.charids. It timed the tick-driven charId check, which is gone (the ids are
        // chosen per parse now); what is left under it is the debug-only sprite line, so it reads ~0.
        // Kept so the [tickcost] columns stay the same.
        int64_t charids = 0;
    };
    const TickSplit &last_tick_split();

    // Debug V3 spike lifecycle: the map owns and destroys the transplanted child.
    // Re-arm creation after that owner starts teardown so the next map can create
    // a fresh instance. No-op for normal icon/resource injection state.
    void v3_on_map_close();

    // RM2::Execute traffic since the last map close, i.e. for THIS open. Answers the one
    // question the build counters cannot: when an open builds far too few markers, was the
    // engine's burst itself short, or did our own gate throw the pulses away? `total` counts
    // every RM2 tag executed anywhere; `sprite171` those that matched the worldmap icon
    // sprite's own tags; `no_screen` / `no_icons` count sprite-171 hits we then refused
    // because no map screen was up or our frames were not injected.
    //
    // It answered its question on the first starved open it saw (2026-08-05): sprite-171 reads
    // 7877 on EVERY open, healthy or not - the engine's burst never varies - while a starved one
    // logged `refused 7803 no_screen`. The pulses were never scarce; we were discarding them.
    void rm2_stats(uint64_t &total, uint64_t &sprite171, uint64_t &no_screen, uint64_t &no_icons);

    // How hard the node search in create_native_icon_instance had to work since the last reset:
    // `calls` searches examining `iters` nodes in total, the worst single walk `max_walk`, and the
    // longest display list encountered `max_list`. The `create` stage is the only part of a
    // factory pulse whose cost grows across map opens, and this search is the only thing in it
    // that can scale with the scene - these four numbers say whether it does.
    void scan_stats(uint64_t &calls, uint64_t &iters, uint64_t &max_walk, uint64_t &max_list);

    // Have our icon frames been appended to the worldmap icon sprite for the current map load?
    // Nothing native can materialize before that, so it is the first gate to check when markers
    // do not appear.
    bool icons_injected();

    // Queue one lightweight bitmap placement in the CURRENT live icon timeline
    // callback. The return value is the timeline record, not a DisplayObject;
    // Scaleform materialises the real child later through Sprite::AddDisplayObject.
    // `ctx` is deliberately not retained outside this synchronous Execute call.
    uintptr_t create_native_icon_instance(int sourceIconId, uint16_t depth,
                                          void *ctx, uint32_t frame);

    // Neutralize OUR timeline record at `depth` on the live ctx (a synthesized
    // RemoveObject2 executed through its own vtable). Called after the child
    // has been transplanted out, so the engine's own materialization pass can
    // never re-process the record and spawn a duplicate inside the host sprite.
    bool remove_native_icon_record(uint16_t depth, void *ctx, uint32_t frame);

    // [v3pump] frame pump generator: queue ONE sprite placement of `spriteCharId` at `depth`
    // on the live ctx - the create_native_icon_instance path with the HasImage flag cleared, so the
    // engine instantiates a Sprite (MovieClip) instead of a bitmap leaf. Returns the timeline record
    // found at `depth`, or 0; the caller runs the materialize driver and the place detour stages the
    // child exactly like a marker. Revives the solid-fill spike's placement (commit 60a8f09).
    uintptr_t create_native_sprite_child(uint16_t spriteCharId, uint16_t depth, void *ctx,
                                         uint32_t frame);
    // NOTE: the Lever A single-mesh DrawingContext probe was removed 2026-07-18 after it
    // hit a GPU-texture wall (our injected icons cannot supply a created texture to
    // beginBitmapFill). Findings + revival options: docs/research_native_singlemesh_wall.md.
}
