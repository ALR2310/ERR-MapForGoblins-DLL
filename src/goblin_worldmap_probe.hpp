#pragma once
// Fold a marker's raw area-local coordinates to world-map space using the game's OWN
// per-map converter (the same routine the engine calls to place every pin). Overworld
// (area 60) and DLC-overworld (61) are a plain affine (see goblin_mapproject); every
// other area (underground 12, legacy dungeons/caves, DLC-legacy) is translated onto the
// overworld-numeric frame by a regulation-driven fold, so it adapts to any profile
// (vanilla / ERR / overhauls) with no hardcoded per-area tables.
#include <cstdint>

namespace goblin::worldmap_probe
{
    // Arm a passive hook on the converter routine to capture the live WorldMapViewModel.
    // Call once at DLL init, before modutils::enable_hooks().
    void setup();

    // Why project() said no. The first two mean we did NOT ask the game: no view model captured
    // yet (no_view), or the game has not converted a point itself in the last 300 ms (stale), so
    // the one we hold may be gone. Only `declined` is the converter's own answer for the tile;
    // `faulted` is a call that raised and was caught, which says nothing about the tile.
    enum class ProjFail : uint8_t { none, no_view, stale, declined, faulted };

    // Fold (area, grid, area-local pos) -> map-space (u,v) via the captured converter.
    // Returns false if the view-model has not been captured yet (the world map has not
    // been opened this session) or the point is not placeable on any map layer; `why`, when
    // given, says which.
    bool project(uint8_t area, uint16_t gx, uint16_t gz, float px, float pz,
                 float &map_u, float &map_v, ProjFail *why = nullptr);
}
