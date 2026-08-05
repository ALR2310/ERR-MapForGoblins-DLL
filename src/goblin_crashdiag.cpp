#include "goblin_crashdiag.hpp"

#include "goblin_config.hpp"
#include "goblin_safemem.hpp" // validate-then-read for the survivor walk

#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>

namespace
{
    // ── always-on state ─────────────────────────────────────────────────────────────────────────
    std::atomic<uint32_t> g_opens{0};      // map opens that reached CATEGORIES READY
    std::atomic<uint32_t> g_closes{0};
    std::atomic<uint32_t> g_generation{0}; // manager re-anchors, i.e. abandoned generations
    std::atomic<uint32_t> g_tracked{0};
    std::atomic<uint32_t> g_created{0};
    std::atomic<uint32_t> g_failed{0};
    std::atomic<int> g_layer{-1};
    std::atomic<uint64_t> g_parent{0};
    std::atomic<uint64_t> g_wrapper{0};
    std::atomic<uint8_t> g_map_open{0};

    // ── probe 1 plumbing ────────────────────────────────────────────────────────────────────────
    // PROCESS_MEMORY_COUNTERS_EX without pulling in psapi.h, and resolved dynamically so the
    // import table is unchanged (the DLL's Defender profile is sensitive to what shows up there).
    struct PMCEX
    {
        DWORD cb;
        DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
        SIZE_T PrivateUsage;
    };
    using GetPMI = BOOL(WINAPI *)(HANDLE, PMCEX *, DWORD);

    GetPMI resolve_pmi()
    {
        static GetPMI fn = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
                fn = reinterpret_cast<GetPMI>(
                    reinterpret_cast<void *>(GetProcAddress(k32, "K32GetProcessMemoryInfo")));
        }
        return fn;
    }

    uint64_t g_last_private = 0; // so each line can carry the delta, not just the absolute

    // ── probe 2 storage ─────────────────────────────────────────────────────────────────────────
    constexpr size_t SAMPLE_MAX = 16;
    uintptr_t g_sample[SAMPLE_MAX] = {};
    size_t g_sample_n = 0;
    uint64_t g_sample_vtable = 0;
    uint32_t g_sample_generation = 0;
    const char *g_sample_site = "?";
    bool g_sample_pending = false;

    // Read a qword without trusting the address. Separate function because MSVC will not compile
    // __try in a frame that also holds objects needing unwinding.
    // Validate-then-read (goblin_safemem.hpp). This one walks ABANDONED generations on purpose -
    // every address it is handed is expected to be dead half the time - so it was among the
    // loudest sources of first-chance exceptions in the process.
    bool probe_read64(uintptr_t addr, uint64_t &out)
    {
        return goblin::safemem::copy(&out, reinterpret_cast<const void *>(addr), sizeof(out));
    }

    bool probe_read32(uintptr_t addr, uint32_t &out)
    {
        return goblin::safemem::copy(&out, reinterpret_cast<const void *>(addr), sizeof(out));
    }

    // Hex without the CRT, for format_state (which can run inside an exception handler).
    int hex64(char *buf, uint64_t v)
    {
        static const char digits[] = "0123456789ABCDEF";
        char tmp[16];
        int n = 0;
        do
        {
            tmp[n++] = digits[v & 0xF];
            v >>= 4;
        } while (v && n < 16);
        int len = 0;
        while (n)
            buf[len++] = tmp[--n];
        return len;
    }
}

void goblin::crashdiag::note_map_open_completed(int layer, uint32_t created, uint32_t failed,
                                                uint32_t tracked)
{
    g_opens.fetch_add(1, std::memory_order_relaxed);
    g_layer.store(layer, std::memory_order_relaxed);
    g_created.store(created, std::memory_order_relaxed);
    g_failed.store(failed, std::memory_order_relaxed);
    g_tracked.store(tracked, std::memory_order_relaxed);
    g_map_open.store(1, std::memory_order_relaxed);
}

void goblin::crashdiag::note_map_closed(uint32_t tracked)
{
    g_closes.fetch_add(1, std::memory_order_relaxed);
    g_tracked.store(tracked, std::memory_order_relaxed);
    g_map_open.store(0, std::memory_order_relaxed);
}

void goblin::crashdiag::note_generation(uint64_t parent, uint64_t wrapper)
{
    g_generation.fetch_add(1, std::memory_order_relaxed);
    g_parent.store(parent, std::memory_order_relaxed);
    g_wrapper.store(wrapper, std::memory_order_relaxed);
}

int goblin::crashdiag::format_state(char *buf, int cap)
{
    // wsprintfA is the only formatter safe here (no CRT, no heap) and it has no 64-bit specifier,
    // so pointers go through hex64 by hand - the same reason dllmain's crash writer does.
    if (cap < 220)
        return 0;
    int len = wsprintfA(buf, "  [state] opens=%u closes=%u gen=%u tracked=%u created=%u failed=%u "
                             "layer=%d mapOpen=%u parent=",
                        g_opens.load(std::memory_order_relaxed),
                        g_closes.load(std::memory_order_relaxed),
                        g_generation.load(std::memory_order_relaxed),
                        g_tracked.load(std::memory_order_relaxed),
                        g_created.load(std::memory_order_relaxed),
                        g_failed.load(std::memory_order_relaxed),
                        g_layer.load(std::memory_order_relaxed),
                        static_cast<unsigned>(g_map_open.load(std::memory_order_relaxed)));
    len += hex64(buf + len, g_parent.load(std::memory_order_relaxed));
    buf[len++] = ' ';
    buf[len++] = 'w';
    buf[len++] = 't';
    buf[len++] = '=';
    len += hex64(buf + len, g_wrapper.load(std::memory_order_relaxed));
    buf[len++] = '\n';
    return len;
}

void goblin::crashdiag::memory(const char *tag, uint32_t tracked)
{
    if (!goblin::config::debugLogging)
        return;
    GetPMI fn = resolve_pmi();
    if (!fn)
        return;
    PMCEX pmc{};
    pmc.cb = sizeof(pmc);
    if (!fn(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return;
    const uint64_t priv = static_cast<uint64_t>(pmc.PrivateUsage);
    const int64_t delta = g_last_private ? static_cast<int64_t>(priv - g_last_private) : 0;
    g_last_private = priv;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    // The number that decides the leak question is `deltaKB` across consecutive opens. A commit
    // charge that climbs by tens of MB every open and never comes back down is accumulation; one
    // that returns to its previous level is the engine reclaiming with the movie.
    spdlog::info("[mem] {} #{} commitMB={} deltaKB={} wsMB={} availMB={} tracked={}", tag,
                 g_opens.load(std::memory_order_relaxed), priv / (1024 * 1024), delta / 1024,
                 static_cast<uint64_t>(pmc.WorkingSetSize) / (1024 * 1024),
                 ms.ullAvailPhys / (1024 * 1024), tracked);
}

void goblin::crashdiag::sample_generation(uint64_t child_vtable, const uintptr_t *children,
                                          size_t count, size_t total, const char *site)
{
    if (!goblin::config::debugLogging)
        return;
    if (!children || count == 0 || child_vtable == 0)
        return;
    // The caller already picked a spread rather than the first sixteen: children created early and
    // late in a burst can land in different parts of the heap, and "the whole generation is gone"
    // must not be concluded from one neighbourhood.
    g_sample_n = 0;
    for (size_t i = 0; i < count && g_sample_n < SAMPLE_MAX; ++i)
        if (children[i])
            g_sample[g_sample_n++] = children[i];
    g_sample_vtable = child_vtable;
    g_sample_generation = g_generation.load(std::memory_order_relaxed);
    g_sample_site = site ? site : "?";
    g_sample_pending = g_sample_n != 0;
    if (g_sample_pending)
        spdlog::info("[survivor] generation {} abandoned by {}: sampled {} of {} children, "
                     "vtable=0x{:X}",
                     g_sample_generation, g_sample_site, g_sample_n, total, child_vtable);
}

void goblin::crashdiag::probe_survivors()
{
    if (!goblin::config::debugLogging || !g_sample_pending)
        return;
    g_sample_pending = false;
    uint32_t committed = 0, freed = 0, vtable_ok = 0, vtable_other = 0, unreadable = 0;
    uint32_t refs_1 = 0, refs_other = 0;
    for (size_t i = 0; i < g_sample_n; ++i)
    {
        const uintptr_t c = g_sample[i];
        MEMORY_BASIC_INFORMATION mbi{};
        const bool queried = VirtualQuery(reinterpret_cast<LPCVOID>(c), &mbi, sizeof(mbi)) != 0;
        const bool live = queried && mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) &&
                          !(mbi.Protect & PAGE_GUARD);
        if (!live)
        {
            ++freed;
            continue;
        }
        ++committed;
        uint64_t vt = 0;
        if (!probe_read64(c, vt))
        {
            ++unreadable;
            continue;
        }
        if (vt == g_sample_vtable)
        {
            ++vtable_ok;
            uint32_t refs = 0;
            if (probe_read32(c + 8, refs))
            {
                if (refs == 1)
                    ++refs_1;
                else
                    ++refs_other;
            }
        }
        else
        {
            ++vtable_other;
        }
    }
    // How to read this:
    //   vtableOk high            -> the objects outlived their movie: the leak is real, and giving
    //                               our reference back would be touching memory that is still ours.
    //   decommitted/vtableOther  -> the engine reclaimed the generation with the movie's own heap:
    //                               there is nothing to free, and releasing would be a write into
    //                               memory that has already been handed to somebody else.
    spdlog::info("[survivor] generation {} (abandoned by {}) rechecked after the next "
                 "build: sampled={} "
                 "committed={} decommitted={} vtableOk={} vtableOther={} unreadable={} "
                 "refs1={} refsOther={}",
                 g_sample_generation, g_sample_site, g_sample_n, committed, freed,
                 vtable_ok, vtable_other,
                 unreadable, refs_1, refs_other);
}

// ── probe 3: the Scaleform arena ────────────────────────────────────────────────────────────────
namespace
{
    // Layout, all re-derived from eldenring.exe and read back out of the full-memory dump:
    //   slot   = exe+0x3D87350            a static holding the DL memory manager
    //   impl   = *(*slot + 8) + 0x28      the arena bookkeeping struct
    //   impl+0x00 capacity, +0x08 free, +0x10 live large allocations,
    //   impl+0x20 free-block tree root, +0x38 / +0x48 the arena's low and high bound.
    // Nothing here is trusted on its own: the resolve below refuses unless every field agrees with
    // every other, and a refusal simply turns the probe off. It only ever reads and logs.
    uintptr_t g_arena = 0;
    bool g_arena_tried = false;
    uint64_t g_arena_lo = 0, g_arena_hi = 0, g_arena_cap = 0;
    uint64_t g_arena_last_free = 0;

    uintptr_t arena_resolve()
    {
        if (g_arena_tried) return g_arena;
        g_arena_tried = true;
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!exe) return 0;
        // Every refusal below SAYS SO. The first version returned 0 in silence, and two whole test
        // runs produced no [arena] line at all with no way to tell "the probe is not in this build"
        // from "the probe could not resolve" - a diagnostic that cannot report its own failure is
        // worse than none.
        uint64_t mgr = 0, inner = 0;
        if (!probe_read64(exe + 0x3D87350, mgr) || mgr < 0x10000)
        {
            spdlog::info("[arena] disabled: manager slot at exe+0x3D87350 reads 0x{:X}", mgr);
            return 0;
        }
        if (!probe_read64(static_cast<uintptr_t>(mgr) + 8, inner) || inner < 0x10000)
        {
            spdlog::info("[arena] disabled: manager+8 reads 0x{:X} (manager 0x{:X})", inner, mgr);
            return 0;
        }
        const uintptr_t impl = static_cast<uintptr_t>(inner) + 0x28;
        uint64_t cap = 0, lo = 0, hi = 0, free_bytes = 0;
        if (!probe_read64(impl + 0x00, cap) || !probe_read64(impl + 0x08, free_bytes) ||
            !probe_read64(impl + 0x38, lo) || !probe_read64(impl + 0x48, hi))
        {
            spdlog::info("[arena] disabled: impl 0x{:X} not readable", impl);
            return 0;
        }
        // Every one of these has to hold at once. Any single coincidence is cheap; all of them
        // together are not, and getting this wrong would print confident nonsense.
        // Validate on capacity and free ONLY. The first version also demanded a low/high pair at
        // +0x38/+0x48, and a live run proved that wrong: cap read 0x9FFF900 - 167,770,368, the exact
        // arena capacity measured in the full dump - and free read a sane 0x57F2F20, while lo and hi
        // both read the same value, i.e. the bounds are simply not at those offsets. Two correct
        // fields plus one wrong assumption should not disable a read-only probe.
        //
        // The bounds were only ever used to walk the free-block tree, so that walk now sanity-checks
        // its own pointers instead.
        if (cap < 0x04000000 || cap > 0x20000000 || free_bytes > cap)
        {
            spdlog::info("[arena] disabled: impl 0x{:X} cap=0x{:X} free=0x{:X} lo=0x{:X} hi=0x{:X}"
                         " - layout does not validate",
                         impl, cap, free_bytes, lo, hi);
            return 0;
        }
        g_arena = impl;
        g_arena_lo = lo;
        g_arena_hi = hi;
        g_arena_cap = cap;
        spdlog::info("[arena] resolved: capacity={} bytes ({} MiB), span 0x{:X}..0x{:X}", cap,
                     cap / (1024 * 1024), lo, hi);
        return g_arena;
    }

    // The largest free block, walked down the right spine of the free-block tree. This is the
    // number that separates the two ways an allocation can fail: exhaustion (free itself is small)
    // from fragmentation (free is large but no single block is). The engine cannot tell them apart
    // - both return 0 from the same instruction - so we have to.
    uint64_t arena_largest_free(uintptr_t impl)
    {
        uint64_t node = 0;
        if (!probe_read64(impl + 0x20, node)) return 0;
        for (int hops = 0; hops < 64; ++hops)
        {
            if (!node || (node & 7) || node < 0x10000) break;
            uint64_t right = 0;
            if (!probe_read64(static_cast<uintptr_t>(node) + 8, right)) break;
            if (!right || (right & 7) || right < 0x10000)
            {
                uint64_t size = 0;
                return probe_read64(static_cast<uintptr_t>(node) + 0x18, size) ? size : 0;
            }
            node = right;
        }
        return 0;
    }
}

void goblin::crashdiag::arena(const char *tag)
{
    if (!goblin::config::debugLogging) return;
    const uintptr_t impl = arena_resolve();
    if (!impl) return;
    uint64_t free_bytes = 0, live = 0;
    if (!probe_read64(impl + 0x08, free_bytes) || !probe_read64(impl + 0x10, live)) return;
    if (free_bytes > g_arena_cap) return;
    const uint64_t largest = arena_largest_free(impl);
    const int64_t delta = g_arena_last_free ? static_cast<int64_t>(free_bytes - g_arena_last_free) : 0;
    g_arena_last_free = free_bytes;
    // How to read this across a session:
    //   freeKB steady            -> the close-time release is returning the generation. Fixed.
    //   freeKB falling ~2.7 MB per open, largest falling with it -> the leak is back.
    //   freeKB large, largest small -> fragmentation is the proximate failure, not exhaustion,
    //                                  and freeKB has stopped being a useful predictor.
    spdlog::info("[arena] {} usedMB={} freeKB={} deltaKB={} largestKB={} liveAllocs={}", tag,
                 (g_arena_cap - free_bytes) / (1024 * 1024), free_bytes / 1024, delta / 1024,
                 largest / 1024, live);
}
