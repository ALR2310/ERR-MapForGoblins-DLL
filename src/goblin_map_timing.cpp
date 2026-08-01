#include "goblin_map_timing.hpp"

#include "goblin_config.hpp"
#include "goblin_build_variants.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_stall_probe.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>

#include <intrin.h> // _ReturnAddress

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// World-map open optimization (variants::kFastMapOpen - a build variant, not an ini
// key: `fast_map_open` was retired 2026-07-27 and lives in ini_retired_keys()).
// With ~7000 markers the game's
// per-marker widget relayout on every map open takes a long time (~0.7-1.2s of the
// open stall). We wrap that relayout (refresh fn) at the map's per-marker dispatcher
// call site, gated by return address so other UI is left as-is, and SKIP it there
// unconditionally - the field-proven "Patch D" form: markers render correctly
// without the map-site relayout (validated in shipping across versions).
//
// History (2026-07-14, see docs/research_worldmap_internals.md): the defer-and-
// replay-per-frame variant ("amortize") is REMOVED. Replaying queued relayout args
// across frames crashed (AV in the refresh fn on a freed widget, dump
// eldenring.exe.18176.dmp): the game rebuilds the marker widgets WITHOUT the
// WorldMapDialog dtor whenever WorldMapPointParam rows change under an open map -
// which our own collected/kindling watcher, manual hide and category toggles do
// live - so any queue of raw widget pointers can go stale mid-open. There is no
// non-racy invalidation point; do not resurrect the replay queue. The ce390
// child-list skip on "reopen" is likewise removed: the dialog + widgets are
// recreated on every open (R1), so a reopen IS a fresh build and skipping the
// child-list passes there was never field-validated.
namespace
{
    using Fn = void *(void *, void *, void *, void *);
    using DtorFn = void *(void *);
    Fn *o_refresh = nullptr;
    DtorFn *o_wmd_dtor = nullptr;

    uintptr_t g_map_callsite = 0; // ret addr of the map's per-marker refresh call
    std::atomic<uint64_t> g_last_mapsite_ms{0}; // last map-site refresh call (burst detect)

    void *refresh_detour(void *a, void *b, void *c, void *d)
    {
        uintptr_t ret = (uintptr_t)_ReturnAddress();
        if (ret == g_map_callsite) // the map's per-marker build call (other UI left as-is)
        {
            // Burst start = first map-site call after >2s of silence = a map (re)open
            // or in-place rebuild pass. Profile it when debug_logging is on.
            uint64_t now = GetTickCount64();
            uint64_t prev = g_last_mapsite_ms.exchange(now, std::memory_order_relaxed);
            if (now - prev > 2000)
                goblin::stall_probe::capture("map-open build", 1500);

            // Skip the per-marker relayout on the map path, every pass (Patch D).
            if (goblin::variants::kFastMapOpen)
                return nullptr;
        }
        return o_refresh(a, b, c, d);
    }

    void *wmd_dtor_detour(void *self)
    {
        // The V3 test child belongs to this display tree and is destroyed by the
        // engine below. Drop every cached pointer first and re-arm creation for the
        // next WorldMapDialog instance.
        goblin::stall_probe::on_map_close();

        // Lever C: while the display tree is still alive (before the dtor below),
        // bulk-detach every native marker child we attached, using the engine's own
        // remove-from-container primitive. This unlinks our TreeCacheNodes and drops
        // the container's reference NOW (map UI thread) instead of leaving all ~9k of
        // them for the dtor's blocking close-teardown job (the ~75ms freeze). No-op
        // unless the self-detach variant is off (goblin_build_variants.hpp).
        goblin::stall_probe::v3_detach_all_children();

        // Map closing. Profile the deferred post-close heap-release stall (~1s after
        // close) when debug_logging is on; the dtor runs on the map UI thread we
        // need to sample.
        goblin::stall_probe::capture("map-close teardown", 3000);
        void *ret = o_wmd_dtor(self);
        // Re-arm only AFTER teardown: doing it before could let an ExecuteTag fired
        // by destruction mistake the dying dialog for a fresh map.
        goblin::gfx_probe::v3_on_map_close();
        return ret;
    }
}

void goblin::map_timing::on_map_frame()
{
    // The shipping fast-map path has no deferred per-frame work of its own, but this is
    // NOT an idle callback: stall_probe::on_map_frame() drives the native-marker rollout
    // (v3_native_tick) and Lever B's viewport reconcile on every frame the map is open.
    // Both are shipping behaviour, not a debug experiment.
    goblin::stall_probe::on_map_frame();
}

void goblin::map_timing::setup()
{
    if (!goblin::variants::kFastMapOpen) return;

    // Resolve the per-marker call site by byte pattern (resilient to game updates):
    // the skip keys on the return address of the refresh call in the map's
    // per-marker dispatcher. The call TARGETS (E8 rel32) are wildcarded so a game
    // update that moves functions doesn't break the match; the return address is a
    // fixed offset from the match. On a miss, g_map_callsite stays 0 -> safe no-op.
    try
    {
        uintptr_t m = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 8B 89 18 01 00 00 E8 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? "
                    "BA 01 00 00 00 48 8B CF E8 ?? ?? ?? ?? BA 03 00 00 00 48 8B CF "
                    "E8 ?? ?? ?? ?? 48 8B 5C 24 30 B0 01"}));
        g_map_callsite = m + 0x0C; // ret of `call refresh`
        spdlog::info("[fastmap] resolved map call site @ 0x{:X}", m);
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[fastmap] map call site not found ({}); fast map open off", e.what());
        return;
    }

    try
    {
        modutils::hook<Fn>(
            {.aob = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 20 "
                    "48 8B D9 48 8B 50 10 48 8B"},
            refresh_detour, o_refresh);
        // Map-close hook. NOT diagnostics: wmd_dtor_detour carries three load-bearing
        // duties before the engine's dtor runs - stall_probe::on_map_close() (drops every
        // cached V3 pointer), v3_detach_all_children() (Lever C, the bulk detach that
        // removes the ~75ms close freeze) and gfx_probe::v3_on_map_close() (re-arms
        // creation for the next dialog). Only the capture() call is diagnostics. Do not
        // remove this hook while trimming logs.
        modutils::hook<DtorFn>(
            {.aob = "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 30 "
                    "48 C7 45 F0 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B F1 48 8D 05 "
                    "B7 A3 16 02"},
            wmd_dtor_detour, o_wmd_dtor);
        spdlog::info("[fastmap] fast map open ready");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[fastmap] setup failed: {}", e.what());
    }
}
