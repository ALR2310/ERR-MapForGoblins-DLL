#pragma once

// Runtime icon-frame injector + (dev-only) RE probe. setup() ALWAYS arms the injection hooks
// (register our DefineBitsLossless2 bitmaps, append a frame per icon to worldmap SpriteDef-171,
// remap markers to the injected frames) so icons render without a shipped/modified gfx. When
// config::debugLogging is on it additionally dumps the live SpriteDef/resource-dict + arms a
// read-only RM2::Execute trace (never changes injection behavior).
// See docs/research_no_gfx_icons.md.
#include <cstdint>

namespace goblin::gfx_probe
{
    void setup();

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
    // runs regardless of enable_overlay): locates the worldmap icon sprite (charId 171), runs the charId
    // collision self-heal, and (when debug_logging) the read-only diagnostic dumps. Injection itself is
    // done by the load-time hooks, independent of this.
    void tick();

    // Debug V3 spike lifecycle: the map owns and destroys the transplanted child.
    // Re-arm creation after that owner starts teardown so the next map can create
    // a fresh instance. No-op for normal icon/resource injection state.
    void v3_on_map_close();

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
}
