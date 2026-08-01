#pragma once
// World-map hover detection. Hooks the game's per-frame "name the focused pin" routine
// (FUN_14087a8e0) so we always know which map pin the cursor is over - exactly the icon
// the game highlights + shows a popup for. Used by manual marker-hide (hover + hotkey)
// and (later) the passive hover-info overlay. The hovered pin's underlying
// WorldMapPointParam row pointer is published; goblin::inject matches it to one of our
// injected rows. Native + exact (no compute-nearest approximation).
#include <cstdint>

namespace goblin::maphover
{
    // Arm the hook (call once at DLL init, before enable_hooks()).
    void setup();

    // The WORLD_MAP_POINT_PARAM_ST* of the pin currently under the cursor, or nullptr
    // when nothing is focused OR the world map is not currently open (heartbeat-gated:
    // the hook only fires while the map is up). Thread-safe.
    void *hovered_row();

    // The live CS::WorldMapArea object (r8 of the hook, vtable RVA 0x2B2CB08), or nullptr
    // when the map is not open. Carries the view transform (pan @+0x378/+0x37C, zoom
    // @+0x380, full-map side @+0x358) that drives the overlay world->screen projection.
    // Thread-safe; nullptr when stale (map closed). (Named map_dialog for API stability.)
    void *map_dialog();

    // The game's hover POPUP panel (the one that names the focused pin), or nullptr when the
    // map is not open. Everything FUN_14087A8E0 does to it is reachable through small
    // primitives, so this handle is enough to place it and fill its lines ourselves.
    void *popup_panel();
    // How many text-line slots the panel's current variant has (-1 = not measured yet).
    int32_t popup_line_slots();

    // The currently displayed map layer, decoded live from the WorldMapDialog field at
    // MapArea+0x904 (value = world*10 + sublayer). Matches WorldMapPointParam dispMask
    // bits: 0 = overworld (M00), 1 = underground (M01), 2 = DLC (M02, both sublayers).
    // -1 when the map is closed / not yet known.
    int map_layer();

    // The reticle in MAP space, as last reported by the game's own hover (it only reports a pin the
    // reticle is on), or false if no sample is that recent. Kept as the cross-check for the field
    // below; the hover pick no longer has to infer an anchor from it.
    bool reticle_map(float *mx, float *mz, uint64_t max_age_ms);

    // Where the GAME looks for a pin this frame, in MAP space, read from the dialog it uses itself -
    // including which of its two position pairs is live (`pointer_mode` = the one that follows the
    // pointer). false = the map is closed or the field did not read as a position, and the caller must
    // fall back to whatever it did before.
    //
    // This replaces guessing the anchor per build. "The reticle is the centre of the view" holds only
    // while the map can still pan: at full zoom-out it cannot, WASD moves the reticle across a fixed
    // view, and the pick then described whatever was in the middle of the screen.
    bool reticle_live(float *mx, float *mz, bool *pointer_mode);


    // GetTickCount64() of the last time the map dialog's per-frame Update ran (this hook fires
    // every frame the world map is open, independent of any marker build). 0 if it has never
    // fired. A value that has advanced PAST a recorded close time is proof the map genuinely
    // reopened - the dialog resumed - which a cached pointer cannot tell you. Thread-safe.
    uint64_t last_activity_ms();
}
