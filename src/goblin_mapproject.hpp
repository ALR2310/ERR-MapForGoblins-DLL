#pragma once
// World-map coordinates. Two things live here now:
//   read_view()  the live view transform (pan / snapMid / zoom) off the map dialog
//   to_map()     a raw WorldMapPointParam position -> the map canvas coordinate that native
//                Scaleform display objects use
//
// Coordinate chain (overworld / underground / DLC - one shared affine):
//   world = gridNo*256 + pos      (from WORLD_MAP_POINT_PARAM_ST)
//   mapX  = worldX - CONST_X ; mapZ = CONST_Z - worldZ
// Legacy dungeons use a per-area fold (WorldMapLegacyConvParam), resolved in tools/legacy_conv.py
// at bake time rather than here.
//
// THIS IS NO LONGER A world->SCREEN PROJECTOR. It was: project() turned a marker into a screen
// pixel so the overlay could draw a highlight ring over the game-rendered icon, and calib() tuned
// the logical->device scale against the rendered frame. Both went with the overlay's on-map
// drawing on 2026-07-28 (the rings are native display children now), and with them the last
// consumer of the screen half of the chain.
#include <cstdint>

namespace goblin::mapproject
{
    struct MapView
    {
        // Live WorldMapArea view state. The engine projection is:
        //   viewCentre = (pan + snapMid) / zoom
        //   screen     = (marker - viewCentre) * zoom * (real/1920or1080) + real/2
        // pan @ +0x378/+0x37C (already in screen-local px), zoom @ +0x380, snapMid =
        // midpoint of the on-screen viewport rect +0x340..+0x34C. This is cursor- AND
        // layer-independent (all fields are live), so it holds for overworld/DLC/underground.
        float panX, panZ;
        float zoom;
        float snapMidX, snapMidZ;
        // (fullMidX / fullMidZ - the fixed midpoint of the complete map canvas - were members
        //  here. Filled every read_view(), read by nothing.)
        bool valid;
    };

    // A Calib struct (a map-space nudge tuned live from overlay sliders) lived here alongside
    // project() below. Both went with the overlay's on-map drawing on 2026-07-28: nothing
    // projects to screen pixels any more, because the rings and the hover marker are native
    // display children positioned in map space.

    // Read the live view transform from the map dialog. false if the map is closed or
    // the dialog is not resolvable this frame.
    bool read_view(MapView &out);

    // Convert a raw WorldMapPointParam position to the map canvas coordinate
    // used by native Scaleform display objects. It does NOT depend on the current
    // pan/zoom or on the client size - that is what makes it safe to bake against.
    bool to_map(uint8_t area, uint16_t gx, uint16_t gz, float px, float pz,
                float &map_x, float &map_z);

}
