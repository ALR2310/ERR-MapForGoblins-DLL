#pragma once

// Runtime icon-frame injector + (dev-only) RE probe. setup() ALWAYS arms the injection hooks
// (register our DefineBitsLossless2 bitmaps, append a frame per icon to worldmap SpriteDef-171,
// remap markers to the injected frames) so icons render without a shipped/modified gfx.
//
// What debug_logging does and does NOT do (the .cpp has carried the correct version since
// 2026-07-30; this header still claimed the old one): it is LOGGING ONLY. It does not arm the
// RM2::Execute hook - that hook is armed by the kNativeMarkers build variant and carries the
// marker-factory pulse, so it is live in every shipping build and is not a trace. The live
// SpriteDef / resource-dict dumps additionally require the compile key MFG_DUMP_FRAMES, which
// is not defined anywhere in the tree.
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
    uint32_t native_character_id(int sourceIconId);
    // Injected iconId for the anon "?" (its source = generated::ANON_ICON_ID). 0 before load.
    uint32_t anon_dynamic_iconid();
    // Lowest / highest 1-based frame id we appended this worldmap load (0,0 before load). Used by
    // remap_injected_icons to flag a marker whose iconId is ALREADY an injected frame id (a double-
    // remap = two DLL instances injected; resolves to the wrong frame when the ranges overlap).
    void injected_iid_range(uint32_t &lo, uint32_t &hi);
    // Called periodically from the DLL's background watcher thread (NOT the overlay Present hook, so it
    // runs regardless of menu_enabled): locates the worldmap icon sprite (charId 171), runs the charId
    // collision self-heal, and (when debug_logging) the read-only diagnostic dumps. Injection itself is
    // done by the load-time hooks, independent of this.
    void tick();

    // Debug V3 spike lifecycle: the map owns and destroys the transplanted child.
    // Re-arm creation after that owner starts teardown so the next map can create
    // a fresh instance. No-op for normal icon/resource injection state.
    void v3_on_map_close();

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
    // NOTE: the Lever A single-mesh DrawingContext probe was removed 2026-07-18 after it
    // hit a GPU-texture wall (our injected icons cannot supply a created texture to
    // beginBitmapFill). Findings + revival options: docs/research_native_singlemesh_wall.md.
}
