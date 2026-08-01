// World-map coordinates (see the header - this is NOT a world->screen projector any more). See goblin_mapproject.hpp.
#include "goblin_mapproject.hpp"
#include "goblin_maphover.hpp"       // map_dialog() -> the live CS::WorldMapArea
#include "goblin_worldmap_probe.hpp" // fold non-overworld areas via the game's converter

#include <windows.h>

namespace
{
    // CS::WorldMapArea (r8 of the hover hook) field offsets (live-confirmed 2026-07-12).
    // pan @ +0x378/+0x37C (screen-local px), zoom @ +0x380, on-screen viewport rect
    // @+0x340..+0x34C (its midpoint = snapMid). The engine projection uses pan directly
    // (do NOT use +0x330, which equals -pan only on the overworld page, not DLC).
    constexpr size_t OFF_PAN = 0x378;       // 2 floats: panX, panZ
    constexpr size_t OFF_ZOOM = 0x380;
    constexpr size_t OFF_VISRECT = 0x340;   // 4 floats: minX, minZ, maxX, maxZ
    constexpr size_t OFF_FULLRECT = 0x350;  // complete canvas; normally {0,0,10496,10496}

    // World->map-space affine (overworld/underground/DLC), from the game's own converter
    // (FUN_140876140, area-60 converter: bGX=28,bGZ=64,off=128,scale=1): mapX = worldX-7040,
    // mapZ = -worldZ+16512. Confirmed by static RE + live converter read 2026-07-12.
    constexpr float CONST_X = 7040.0f;
    constexpr float CONST_Z = 16512.0f;

    // The engine renders the map in a fixed virtual 1920x1080 GFx canvas then scales to
    // the backbuffer, so the canvas factor (realW/1920, realH/1080) is mandatory.
    // (CANVAS_W / CANVAS_H, the 1920x1080 GFx canvas, stood here. They were the projector's
    //  units; the projector left on 2026-07-28 and took both consumers with it.)

    bool seh_read(const void *addr, void *out, size_t n)
    {
        __try { memcpy(out, addr, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    float rf(const uint8_t *base, size_t off)
    {
        float v = 0.0f;
        seh_read(base + off, &v, sizeof v);
        return v;
    }

}

bool goblin::mapproject::read_view(MapView &out)
{
    out.valid = false;
    void *area = goblin::maphover::map_dialog();  // returns the WorldMapArea
    if (!area) return false;
    auto *b = reinterpret_cast<const uint8_t *>(area);
    out.panX = rf(b, OFF_PAN + 0);
    out.panZ = rf(b, OFF_PAN + 4);
    out.zoom = rf(b, OFF_ZOOM);
    const float vminX = rf(b, OFF_VISRECT + 0), vminZ = rf(b, OFF_VISRECT + 4);
    const float vmaxX = rf(b, OFF_VISRECT + 8), vmaxZ = rf(b, OFF_VISRECT + 12);
    const float fminX = rf(b, OFF_FULLRECT + 0), fminZ = rf(b, OFF_FULLRECT + 4);
    const float fmaxX = rf(b, OFF_FULLRECT + 8), fmaxZ = rf(b, OFF_FULLRECT + 12);
    out.snapMidX = (vminX + vmaxX) * 0.5f;
    out.snapMidZ = (vminZ + vmaxZ) * 0.5f;
    // (out.fullMidX / fullMidZ were filled here and read by nobody: both live consumers of
    //  MapView - goblin_inject's reticle_view and stall_probe's zoom/pan reader - take only
    //  pan, snapMid and zoom. The OFF_FULLRECT reads above STAY: the validity test below is
    //  built on them.)
    if (!(out.zoom > 0.01f)) return false;
    if (!((vmaxX - vminX) > 1.0f) || !((vmaxZ - vminZ) > 1.0f)) return false;
    if (!((fmaxX - fminX) > 1.0f) || !((fmaxZ - fminZ) > 1.0f)) return false;
    out.valid = true;
    return true;
}

bool goblin::mapproject::to_map(uint8_t area, uint16_t gx, uint16_t gz,
                                float px, float pz, float &map_x, float &map_z)
{
    if (area == 60 || area == 61)
    {
        map_x = (static_cast<float>(gx) * 256.0f + px) - CONST_X;
        map_z = CONST_Z - (static_cast<float>(gz) * 256.0f + pz);
        return true;
    }
    return goblin::worldmap_probe::project(area, gx, gz, px, pz, map_x, map_z);
}

// project() (world -> screen) and calib() lived here. Their only consumer was the overlay's
// on-map drawing - the focus rings and the hover marker - which was retired 2026-07-28 when the
// native equivalents took over. read_view() and to_map() below them stay: those ARE live, called
// from goblin_inject and goblin_stall_probe, and to_map still routes non-overworld tiles through
// goblin::worldmap_probe::project.
