#include "goblin_map_timing.hpp"

#include "goblin_config.hpp"
#include "goblin_build_variants.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_stall_probe.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

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
    // The TWIN of that call: the [opentime] histogram (2026-08-31, ERR 2.3.3.0) showed a
    // steady second stream of relayout calls during every open - x115 per open, x1152 in a
    // heavy scroll window - and the exe has exactly three call sites into the refresh fn.
    // This one is a handler directly above the known dispatcher, byte-identical except the
    // final `mov rbx,[rsp+0x38]` (the known one restores from +0x30). Same skip, same
    // reasoning; 0 when the pattern is absent, which only costs the extra skip.
    uintptr_t g_map_callsite_twin = 0;
    // The thin wrapper just before the refresh fn (state update + refresh + post-set). Its
    // OWN return address says nothing about who asked, so the histogram unwraps one level:
    // with its `push rdi; sub rsp,0x20` frame the wrapper's caller sits at +0x30 above our
    // return-address slot. Diagnostics only - never a skip decision.
    uintptr_t g_wrapper_ra = 0;
    std::atomic<uint64_t> g_last_mapsite_ms{0}; // last map-site refresh call (burst detect)

    // The open-latency probe (printed by stall_probe at seed READY as [opentime]): when
    // the user feels the open slow down, the logs must say WHOSE time it is. t0 is the
    // first map-site relayout call after silence; the split between map-site calls
    // (skipped by Patch D) and refresh calls arriving from any OTHER return address is
    // the direct test for a second, unskipped call site.
    std::atomic<uint64_t> g_open_t0{0};
    std::atomic<uint64_t> g_site_calls{0};
    std::atomic<uint64_t> g_site_skipped{0};
    std::atomic<uint64_t> g_other_calls{0};

    // WHO the other-site calls are: a tiny top-N histogram of their return addresses,
    // reset at each burst start. This is the piece that tells an uncovered MAP relayout
    // path (one hot RA next to the known call site) apart from ordinary non-map UI
    // traffic (a spread of small counts). Eight slots; anything past them lands in the
    // overflow bucket, which staying at 0 is itself information.
    struct OtherSite
    {
        std::atomic<uintptr_t> ra{0};
        std::atomic<uint64_t> n{0};
    };
    std::array<OtherSite, 8> g_other_sites{};
    std::atomic<uint64_t> g_other_overflow{0};

    void note_other_site(uintptr_t ra)
    {
        for (auto &s : g_other_sites)
        {
            uintptr_t cur = s.ra.load(std::memory_order_relaxed);
            if (cur == 0)
            {
                if (s.ra.compare_exchange_strong(cur, ra, std::memory_order_relaxed))
                    cur = ra; // claimed the slot; fall through to count
                // cur now holds whoever owns the slot (us or a racing caller)
            }
            if (cur == ra)
            {
                s.n.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        g_other_overflow.fetch_add(1, std::memory_order_relaxed);
    }

    void *refresh_detour(void *a, void *b, void *c, void *d)
    {
        uintptr_t ret = (uintptr_t)_ReturnAddress();
        // the map's per-marker build calls (other UI left as-is)
        if (ret == g_map_callsite || (g_map_callsite_twin && ret == g_map_callsite_twin))
        {
            // Burst start = first map-site call after >2s of silence = a map (re)open
            // or in-place rebuild pass. Profile it when debug_logging is on.
            uint64_t now = GetTickCount64();
            uint64_t prev = g_last_mapsite_ms.exchange(now, std::memory_order_relaxed);
            if (now - prev > 2000)
            {
                goblin::stall_probe::capture("map-open build", 1500);
                g_open_t0.store(now, std::memory_order_relaxed);
                g_site_calls.store(0, std::memory_order_relaxed);
                g_site_skipped.store(0, std::memory_order_relaxed);
                g_other_calls.store(0, std::memory_order_relaxed);
                for (auto &s : g_other_sites)
                {
                    s.ra.store(0, std::memory_order_relaxed);
                    s.n.store(0, std::memory_order_relaxed);
                }
                g_other_overflow.store(0, std::memory_order_relaxed);
            }
            g_site_calls.fetch_add(1, std::memory_order_relaxed);

            // Skip the per-marker relayout on the map path, every pass (Patch D).
            if (goblin::variants::kFastMapOpen)
            {
                g_site_skipped.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        }
        else if (g_open_t0.load(std::memory_order_relaxed) != 0)
        {
            g_other_calls.fetch_add(1, std::memory_order_relaxed);
            if (g_wrapper_ra && ret == g_wrapper_ra)
            {
                // Unwrap the wrapper: charge its CALLER, read from our own stack (the
                // wrapper's frame is push rdi + 0x20). A wrong offset merely puts a junk
                // address in a diagnostic histogram.
                const uintptr_t caller = *reinterpret_cast<const uintptr_t *>(
                    reinterpret_cast<const char *>(_AddressOfReturnAddress()) + 0x30);
                note_other_site(caller);
            }
            else
            {
                note_other_site(ret);
            }
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

uint64_t goblin::map_timing::open_burst_t0()
{
    return g_open_t0.load(std::memory_order_relaxed);
}

std::string goblin::map_timing::open_other_sites()
{
    struct Row
    {
        uintptr_t ra;
        uint64_t n;
    };
    Row rows[8];
    size_t cnt = 0;
    for (auto &s : g_other_sites)
    {
        const uintptr_t ra = s.ra.load(std::memory_order_relaxed);
        const uint64_t n = s.n.load(std::memory_order_relaxed);
        if (ra && n)
            rows[cnt++] = {ra, n};
    }
    std::sort(rows, rows + cnt, [](const Row &a, const Row &b) { return a.n > b.n; });
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::string out;
    char buf[64];
    for (size_t i = 0; i < cnt; ++i)
    {
        if (!out.empty())
            out += ", ";
        if (exe && rows[i].ra >= exe)
            snprintf(buf, sizeof buf, "exe+0x%llX x%llu",
                     static_cast<unsigned long long>(rows[i].ra - exe),
                     static_cast<unsigned long long>(rows[i].n));
        else
            snprintf(buf, sizeof buf, "0x%llX x%llu",
                     static_cast<unsigned long long>(rows[i].ra),
                     static_cast<unsigned long long>(rows[i].n));
        out += buf;
    }
    const uint64_t over = g_other_overflow.load(std::memory_order_relaxed);
    if (over)
    {
        snprintf(buf, sizeof buf, ", +%llu past the 8 slots",
                 static_cast<unsigned long long>(over));
        out += buf;
    }
    return out.empty() ? "none" : out;
}

void goblin::map_timing::open_counters(uint64_t &site, uint64_t &skipped, uint64_t &other)
{
    site = g_site_calls.load(std::memory_order_relaxed);
    skipped = g_site_skipped.load(std::memory_order_relaxed);
    other = g_other_calls.load(std::memory_order_relaxed);
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

    // The twin dispatcher's call into the same refresh fn (see the histogram note at the
    // top): byte-identical to the site above except it restores rbx from +0x38. Optional -
    // a build without it just keeps the single-site skip.
    try
    {
        uintptr_t m2 = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 8B 89 18 01 00 00 E8 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? "
                    "BA 01 00 00 00 48 8B CF E8 ?? ?? ?? ?? BA 03 00 00 00 48 8B CF "
                    "E8 ?? ?? ?? ?? 48 8B 5C 24 38 B0 01"}));
        g_map_callsite_twin = m2 + 0x0C;
        spdlog::info("[fastmap] twin call site resolved @ 0x{:X}", m2);
    }
    catch (const std::exception &e)
    {
        spdlog::info("[fastmap] no twin call site on this build ({}); single-site skip",
                     e.what());
    }

    // The wrapper's return address, for the histogram's one-level unwrap. Found from the
    // refresh fn itself (scanned BEFORE the hook overwrites its first bytes): the one
    // direct call into it within the 0x60 bytes just above it.
    try
    {
        const uintptr_t fn = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 20 "
                    "48 8B D9 48 8B 50 10 48 8B"}));
        for (uintptr_t p = fn - 0x60; p + 5 <= fn; ++p)
        {
            if (*reinterpret_cast<const uint8_t *>(p) != 0xE8)
                continue;
            const int32_t rel = *reinterpret_cast<const int32_t *>(p + 1);
            if (p + 5 + static_cast<intptr_t>(rel) == static_cast<intptr_t>(fn))
            {
                g_wrapper_ra = p + 5;
                break;
            }
        }
        if (g_wrapper_ra)
            spdlog::info("[fastmap] wrapper return address resolved @ 0x{:X}", g_wrapper_ra);
    }
    catch (const std::exception &)
    {
        // no unwrap; the histogram then shows the wrapper's own address, which is still data
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
        // The tail used to end on the raw displacement of that `lea rax,[rip+..]`
        // (B7 A3 16 02). A displacement is a different number on every build, so this hook -
        // and with it every duty above - was found ONLY on the exe the mod was built from.
        // Wildcarded and grown by four bytes instead: measured a single match on 2.6.2,
        // 2.6.1, 2.6.0, 2.2.3 and 2.2.0 (scratch/compat_matrix.py).
        modutils::hook<DtorFn>(
            {.aob = "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 30 "
                    "48 C7 45 F0 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B F1 48 8D 05 "
                    "?? ?? ?? ?? 48 89 01 48"},
            wmd_dtor_detour, o_wmd_dtor);
        spdlog::info("[fastmap] fast map open ready");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[fastmap] setup failed: {}", e.what());
    }
}
