#pragma once

// VALIDATE, THEN READ. The mod chases raw pointers out of the game's heap constantly, and the old
// answer to "this pointer might be dead" was a __try around the read. That is wrong, and the reason
// is not performance:
//
//   **A first-chance exception is a PROCESS-WIDE event, not a private control-flow trick.**
//
// Every debugger, every crash reporter and every third-party overlay with a vectored handler sees
// each one and may do arbitrary, non-thread-safe work ON THE THREAD THAT FAULTED. Proven on the rig
// 2026-08-04 (dump 28908) in a single stack: our guarded read in `rq` -> CRT memcpy load -> nine
// ERSS-FG frames (its filter) -> seven dbghelp frames (dbghelp is documented single-threaded) ->
// heap churn -> ntdll's heap validator killed the process with STATUS_HEAP_CORRUPTION. 24 of our
// exceptions did that in ~40 seconds; a player earlier survived 27545 of them in 31 minutes only
// because their setup symbolized more cheaply. Our own log said nothing either time - both storms
// were witnessed only by somebody else's log, which is why refusals are counted and reported here.
//
// So: ask the OS whether the range is mapped, and only then read. A refusal costs one VirtualQuery
// on a miss and a few compares on a hit; an exception costs whatever every handler in the process
// decides to spend. The `__try` stays underneath as a net for the check-then-read race (another
// thread can unmap between the query and the copy).
//
// The two things that make the net a net rather than a new storm - both found in review, both
// cheap, neither optional:
//   * a fault DROPS the cache (forget_all), so a region freed after being cached readable costs
//     ONE exception per unmap event per thread instead of one per call, forever;
//   * cached verdicts EXPIRE (kTtlMs), so a stale verdict cannot outlive the address it
//     describes.
//
// NEGATIVE verdicts are NOT cached - every refusal is a fresh VirtualQuery. The first cut cached
// them (to make a dead-pointer storm cost compares, not syscalls) and it broke marker creation
// wholesale, proven in game 2026-08-05: the icon factory's tags and timeline nodes are committed
// by the engine RIGHT NOW out of reserved movie-heap space, VirtualQuery on a reserved address
// returns the whole multi-megabyte reserved region as one "not committed" answer, and a 1-second
// negative verdict over that span made every fresh allocation invisible. First map open attached
// 3002 of 7137 markers, reopens attached a few hundred, the queue watchdog dropped the rest
// (baseline the day before: 7136 of 7137, every open). The storm this cache was defending
// against runs at ~15 Hz (report 22: 27545 probes in 31 min) - one ~1us syscall per probe is
// nothing, so the negative cache bought nothing and cost the feature. Refusal counts stay
// visible either way (g_refused / the periodic line).
//
// NOT for the deliberate engine calls that also sit inside `__try` elsewhere in this codebase:
// those invoke game code that can fault for reasons no address check can predict, and they cannot
// be pre-validated. They stay SEH.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <spdlog/spdlog.h>

namespace goblin::safemem
{
    inline std::atomic<uint64_t> g_refused{0};      // reads/writes declined before touching memory
    inline std::atomic<uint64_t> g_late_faults{0};  // the race net actually fired (expected: 0)
    inline std::atomic<uint64_t> g_queries{0};      // VirtualQuery calls, i.e. cache misses
    inline std::atomic<uint64_t> g_copies{0};       // total copy() calls - the hot-path volume

    inline bool prot_readable(DWORD p)
    {
        if (p & (PAGE_GUARD | PAGE_NOACCESS))
            return false; // reading a guard page RAISES and also consumes the guard - never touch
        return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    inline bool prot_writable(DWORD p)
    {
        if (p & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        return (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    // ── the cache ────────────────────────────────────────────────────────────────────────────
    // Per THREAD, so it needs no lock and no atomics on the hot path: the pointer chases run on
    // the map/UI thread and on the loader threads independently. POSITIVE verdicts only (see the
    // header comment for why caching negatives broke marker creation): a slot in here always
    // describes a committed, accessible region.
    struct Region
    {
        uintptr_t base = 0;
        uintptr_t end = 0;
        uint64_t stamp = 0; // when the OS was last asked about it
        bool read_ok = false;
        bool write_ok = false;
    };

    // A cached verdict is a snapshot of something the game changes under us, so it expires. The
    // late-fault net catches a positive verdict going stale the hard way; the TTL bounds how long
    // one can keep costing that net. One second means at most a handful of VirtualQuery calls per
    // second per thread for the hot regions, which is nothing next to a syscall per read.
    constexpr uint64_t kTtlMs = 1000;
    // Eight, not four. The hot caller is the per-frame marker walk: thousands of reads per frame,
    // alternating between the game's heap regions and our own stack. If the working set of regions
    // exceeds the slots, round-robin eviction turns every read back into a VirtualQuery - a syscall
    // per read is a worse trade than the exceptions we are removing. Eight is still nothing (two
    // cache lines) and leaves headroom. The periodic log prints the lookup count precisely so this
    // can be SEEN rather than assumed: lookups climbing with frames = thrash, raise this.
    constexpr size_t kSlots = 8;
    inline thread_local Region t_cache[kSlots]{};
    inline thread_local unsigned t_victim = 0;

    inline const Region *cache_find(uintptr_t a, uint64_t now)
    {
        for (size_t i = 0; i < kSlots; ++i)
            if (t_cache[i].end && a >= t_cache[i].base && a < t_cache[i].end)
                return (now - t_cache[i].stamp <= kTtlMs) ? &t_cache[i] : nullptr;
        return nullptr;
    }

    // Ask the OS. A positive verdict (readable or writable) is cached; a negative one is returned
    // but NOT stored - the engine may commit into that very range on the next allocation, and a
    // cached "no" would blind us to it (the marker-creation lesson in the header comment). A
    // negative answer also EVICTS any cached positive it contradicts, so a freed region does not
    // keep passing on a stale slot until the TTL runs out.
    inline Region query_region(uintptr_t a, uint64_t now)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        g_queries.fetch_add(1, std::memory_order_relaxed);
        Region r{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) != sizeof(mbi))
            return r; // r.end == 0: not a queryable address at all
        r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        r.end = r.base + mbi.RegionSize;
        r.stamp = now;
        const bool committed = mbi.State == MEM_COMMIT;
        r.read_ok = committed && prot_readable(mbi.Protect);
        r.write_ok = committed && prot_writable(mbi.Protect);
        // Reuse the slot this address already owns if it has one (it may just be stale), so a
        // refresh does not evict a different hot region.
        Region *slot = nullptr;
        for (size_t i = 0; i < kSlots; ++i)
            if (t_cache[i].end && a >= t_cache[i].base && a < t_cache[i].end)
            {
                slot = &t_cache[i];
                break;
            }
        if (r.read_ok || r.write_ok)
        {
            if (!slot)
                slot = &t_cache[t_victim++ % kSlots];
            *slot = r;
        }
        else if (slot)
            *slot = Region{};
        return r;
    }

    // Drop everything this thread believes about the address space. Called when a validated range
    // faulted anyway, which means the map changed under us and NOTHING cached is trustworthy - not
    // just the range we asked about, since an unmap usually takes a whole region with it.
    inline void forget_all()
    {
        for (size_t i = 0; i < kSlots; ++i)
            t_cache[i] = Region{};
    }

    // Is [p, p+n) entirely mapped with the access we need? Walks region by region, because a
    // legitimate object CAN straddle a boundary between two committed regions and refusing that
    // would break a working read rather than a broken one. Bounded so a pathological range cannot
    // turn into a syscall loop.
    inline bool range_ok(const void *p, size_t n, bool want_write)
    {
        if (!p || n == 0)
            return false;
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        const uintptr_t b = a + n;
        if (b < a) // wrapped: not a range, whatever it is
            return false;
        const uint64_t now = GetTickCount64(); // once per range, not per hop
        uintptr_t at = a;
        for (int hop = 0; hop < 4 && at < b; ++hop)
        {
            const Region *cached = cache_find(at, now);
            const Region r = cached ? *cached : query_region(at, now);
            if (!r.end)
                return false;
            if (!(want_write ? r.write_ok : r.read_ok))
                return false;
            at = r.end;
        }
        return at >= b;
    }

    inline bool readable(const void *p, size_t n) { return range_ok(p, n, false); }
    inline bool writable(const void *p, size_t n) { return range_ok(p, n, true); }

    // One line per minute at most, into the NORMAL log - not behind debug_logging. Both storms so
    // far were invisible to us and visible to a third party; a counter nobody can see is not
    // instrumentation. Called from copy() itself (an atomic load and a tick compare on the hot
    // path), so it fires on schedule whether or not anything was refused - a session with zero
    // refusals still reports its call volume.
    inline void report_periodically()
    {
        static std::atomic<uint64_t> s_next_ms{0};
        const uint64_t now = GetTickCount64();
        uint64_t due = s_next_ms.load(std::memory_order_relaxed);
        if (now < due)
            return;
        if (!s_next_ms.compare_exchange_strong(due, now + 60000, std::memory_order_relaxed))
            return;
        if (due == 0) // first call arms the window; nothing to report yet
            return;
        spdlog::info("[safemem] {} copies, {} region lookups, {} refused, {} late faults",
                     g_copies.load(std::memory_order_relaxed),
                     g_queries.load(std::memory_order_relaxed),
                     g_refused.load(std::memory_order_relaxed),
                     g_late_faults.load(std::memory_order_relaxed));
    }

    // POD-only body: MSVC refuses __try in a frame that also needs C++ unwinding.
    inline bool seh_memcpy(void *dst, const void *src, size_t n)
    {
        __try
        {
            memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The one entry point callers should use. Validates BOTH ends: our own buffer is the
    // destination when reading game memory, and the game's memory is the destination when writing
    // to it, so a single-sided check would leave half the uses unguarded.
    inline bool copy(void *dst, const void *src, size_t n)
    {
        if (n == 0)
            return true;
        g_copies.fetch_add(1, std::memory_order_relaxed);
        report_periodically();
        if (!readable(src, n) || !writable(dst, n))
        {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (seh_memcpy(dst, src, n))
            return true;
        // The net fired: the range was mapped when we asked and gone when we read.
        //
        // INVALIDATE FIRST, and invalidate everything. Without this the file defeats its own
        // purpose: a region cached as readable and then freed by the game - exactly what the map
        // driver does to the marker heap on close - would keep passing cache_find, so a 15 Hz
        // caller would fault on EVERY call and hand a first-chance exception to every handler in
        // the process, forever. Dropping the cache here bounds it to ONE exception per real unmap
        // event per thread: the next call re-asks the OS and refuses cleanly.
        forget_all();
        const uint64_t late = g_late_faults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (late == 1 || (late % 100) == 0)
            spdlog::warn("[safemem] a validated range faulted anyway ({} so far) - unmapped "
                         "between the check and the read; cache dropped", late);
        return false;
    }
}
