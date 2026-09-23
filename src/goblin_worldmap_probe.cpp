// See goblin_worldmap_probe.hpp. Captures CS::WorldMapViewModel from the engine's
// world->map-space converter and re-invokes that converter to fold any marker.
#include "goblin_worldmap_probe.hpp"

#include "goblin_guarded.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>

namespace
{
    struct Vec2 { float x, y; };
    struct Vec3 { float x, y, z; };

    // Converter loop (FUN_1408877D0): __fastcall(rcx = WorldMapViewModel, rdx = out map
    // Vec2, r8 = packed id {area<<24|gridX<<16|gridZ<<8}, r9 = area-local Vec3 {x,y,z}).
    // Iterates the VM's per-map converters (VM+0xF8, count VM+0x280), applies the legacy
    // fold, and writes the map-space (u,v) to *out. Returns nonzero on a placed point.
    using ConvertFn = char(void *, Vec2 *, uint32_t *, Vec3 *);
    ConvertFn *o_convert = nullptr;

    std::atomic<void *> g_vm{nullptr};
    // The freshness gate the old note here asked for, now that report 19 has shown the bill for
    // going without it. The view model is not a singleton: it is new-ed (0x450 bytes) by
    // CS::MoveMapStep's constructor and freed and nulled by its destructor, so it dies on every
    // map transition. g_vm is written ONLY from this detour, i.e. only when the GAME itself
    // converts - our own project() calls go straight to o_convert and never refresh it. So a
    // pointer that has not been re-stamped recently is a pointer to a dead generation, and
    // handing it back to the converter walks a freed std::map of per-tile converters:
    //   placename_detour -> native_reticle_row -> to_map -> project -> seh_fold
    //   -> exe+0x8877D0 -> exe+0x876140 -> exe+0x87762A `mov r8,[rsi+0x10]` -> AV
    // twelve times a session in that report, on the seed and on the hover path both.
    //
    // 300 ms is goblin_maphover's window, and the same reasoning applies: while the map is
    // genuinely live the game converts far more often than that, so a stale stamp means the
    // map is closed or mid-transition - the two states in which we must not call at all.
    std::atomic<uint64_t> g_vm_ms{0};
    constexpr uint64_t VM_FRESH_MS = 300;

    char convert_detour(void *vm, Vec2 *out, uint32_t *packed, Vec3 *world_local)
    {
        g_vm.store(vm, std::memory_order_relaxed);
        g_vm_ms.store(GetTickCount64(), std::memory_order_release);
        return o_convert(vm, out, packed, world_local);
    }

    // 1 = converted, 0 = the converter refused the tile, -1 = the call raised (the report-19
    // class: a view model that died under us). The last two must not be confused - only a
    // refusal says anything about the profile's data.
    int seh_fold(void *vm, uint32_t packed, float px, float pz, float &u, float &v)
    {
        __try
        {
            Vec2 out{0, 0};
            Vec3 wl{px, 0.0f, pz};
            uint32_t p = packed;
            if (!o_convert(vm, &out, &p, &wl)) return 0;
            u = out.x;
            v = out.y;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
}

void goblin::worldmap_probe::setup()
{
    try
    {
        auto *fn = modutils::hook<ConvertFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 "
                    "48 83 EC 20 33 DB 4D 8B F9 4D 8B E0 4C 8B EA 48 8B F1 48 39 99 80 02 00 00"},
            convert_detour, o_convert);
        spdlog::info("[wmprobe] converter hook armed @ 0x{:X}", reinterpret_cast<uintptr_t>(fn));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[wmprobe] setup failed (folded-layer rings disabled): {}", e.what());
    }
}

bool goblin::worldmap_probe::project(uint8_t area, uint16_t gx, uint16_t gz, float px, float pz,
                                     float &map_u, float &map_v, ProjFail *why)
{
    if (why) *why = ProjFail::none;
    void *vm = g_vm.load(std::memory_order_relaxed);
    if (!vm || !o_convert)
    {
        if (why) *why = ProjFail::no_view;
        return false;
    }
    // Validate before calling, never catch after. seh_fold's __except is a net for the case
    // this test cannot see, not the mechanism - reaching it is now a defect, not an answer.
    const uint64_t stamped = g_vm_ms.load(std::memory_order_acquire);
    const uint64_t now = GetTickCount64();
    if (stamped == 0 || now < stamped || now - stamped > VM_FRESH_MS)
    {
        if (why) *why = ProjFail::stale;
        return false;
    }
    const uint32_t packed = (static_cast<uint32_t>(area) << 24) |
                            ((static_cast<uint32_t>(gx) & 0xFF) << 16) |
                            ((static_cast<uint32_t>(gz) & 0xFF) << 8);
    // This is a deliberate call into engine code that can fail, so the crash logger must not
    // record it. Raised HERE and not around seh_fold's __try: that function returns from three
    // points inside the guard, so an in-place decrement would be skipped on two of them and the
    // thread_local depth would ratchet up until crash logging was dead for the session.
    ++goblin::guarded::depth;
    const int r = seh_fold(vm, packed, px, pz, map_u, map_v);
    --goblin::guarded::depth;
    if (r != 1 && why) *why = r == 0 ? ProjFail::declined : ProjFail::faulted;
    return r == 1;
}
