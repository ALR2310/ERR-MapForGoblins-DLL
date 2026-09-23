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
// So: ask the OS whether the range is mapped, and only then read. A refusal costs one page probe
// plus one VirtualQuery on a miss and a few compares on a hit; an exception costs whatever every handler in the process
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
    // TIME spent inside VirtualQuery, in QueryPerformanceCounter ticks. The call COUNT was
    // always logged and always looked harmless; the cost per call was the thing nobody could
    // see. On this author's machine a single VirtualQuery measured 2+ ms in the TwoHandToggle
    // mod, and on one tester's 4.4-4.7 ms - roughly 40x normal, consistent with a memory-API
    // filter (an anti-virus). Three calls a second is therefore not "nothing next to a
    // syscall per read"; it is a visible hitch. Report this alongside the count.
    // (The cost turned out to be the RegionSize walk, not a filter - see page_probe below.)
    inline std::atomic<uint64_t> g_query_qpc{0};
    // Answers that came from the one-page probe instead of VirtualQuery, and what the probe costs:
    // its QPC ticks (answered or not) and the calls it could not answer (they went on to
    // VirtualQuery). Its 1.3-9 us is an idle-process figure; in the game it shares the memory
    // manager with the streaming threads, so it is timed, not assumed.
    inline std::atomic<uint64_t> g_page_hits{0};
    inline std::atomic<uint64_t> g_probe_qpc{0};
    inline std::atomic<uint64_t> g_probe_misses{0};
    // The same tallies for the CALLING thread only: the process-wide ones above mix the map thread
    // with the watcher and loader threads (ERR 2026-09-23: a 41 ms lookup in a window whose map
    // frames never passed 9 ms could not be placed). t_queries / t_query_qpc = VirtualQuery calls
    // and their QPC ticks, t_copies = copy() calls, t_refused = copy() refusals, t_page_hits =
    // answered by page_probe.
    inline thread_local uint64_t t_queries = 0, t_query_qpc = 0, t_copies = 0, t_refused = 0,
                                 t_page_hits = 0, t_probe_qpc = 0;

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
    // one can keep costing that net.
    //
    // FIVE seconds, not one. At one second the map tick's two liveness checks fell off the cache
    // every single second and the refresh cost 8-10 ms IN ONE FRAME - measured on ERR 2.3.2.2,
    // where the per-second `head` total matched the worst frame to within 20 us, every window.
    // That is a syscall on a render path whose price nobody had measured: VirtualQuery has to
    // take the process address-space lock, and while the game streams it waits there. The same
    // number was found the same way in the TwoHandToggle mod (see its notes), where raising this
    // TTL to five seconds and counting the milliseconds was what closed the CPU story.
    constexpr uint64_t kTtlMs = 5000;
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

    // The slot this address already owns, if any (it may just be stale).
    inline Region *owning_slot(uintptr_t a)
    {
        for (size_t i = 0; i < kSlots; ++i)
            if (t_cache[i].end && a >= t_cache[i].base && a < t_cache[i].end)
                return &t_cache[i];
        return nullptr;
    }

    // ── the one-page probe ───────────────────────────────────────────────────────────────────
    // VirtualQuery's price is not the syscall. Its RegionSize is the run of identical pages from
    // the queried page to the end of the run, and the kernel walks every page of it to report
    // that. Measured on the author's machine 2026-09-23 (scratch/tick_gate): 1.3 us at the last
    // page of a run, 0.64 ms for 256 MB, 8.5-10 ms for 1 GB and 11-19 ms for 2 GB of resident
    // pages, 17-19 ms for 1 GB of trimmed ones. On ERR the map anchor sits early in a multi-GB
    // committed game heap: 29-34 ms per cache miss, on 7 of 13 opens plus once per kTtlMs while
    // the map stayed up, each inside the open's longest frame.
    //
    // QueryWorkingSetEx answers for ONE page in 1.3-9 us whatever the run: bit 0 (Valid) says the
    // page is in the working set, bits 4-14 carry its protection. Resident and readable is proof
    // enough for a positive. Anything else - not resident, guard, no-access, not committed, or
    // the call unavailable - goes to VirtualQuery exactly as before, so every refusal is still the
    // OS's full answer and negatives are still never cached.
    //
    // The positive covers that page and nothing more. range_ok walks region by region, so the
    // next page of a range gets its own probe: one page can never vouch for the next, and a stale
    // positive no longer speaks for the gigabytes a VirtualQuery run did.
    //
    // SHORT ranges only (kProbePages). Every per-frame caller reads a few hundred bytes at most;
    // a bulk copy (tag and frame arrays at icon load) keeps the single VirtualQuery whose one
    // answer covers the whole run, instead of paying a probe per page.
    constexpr uintptr_t kPage = 0x1000;
    constexpr uintptr_t kProbePages = 2;

    // PSAPI_WORKING_SET_EX_INFORMATION, declared here so psapi.h is not pulled in.
    struct WsExInfo
    {
        void *VirtualAddress;
        uint64_t VirtualAttributes; // bit 0 Valid, bits 1-3 ShareCount, bits 4-14 Win32Protection
    };
    using QueryWsExFn = BOOL(WINAPI *)(HANDLE, void *, DWORD);

    // Resolved at runtime from kernel32, like crashdiag's K32GetProcessMemoryInfo: no psapi.lib and
    // no new import-table entry. Null where it does not exist, which just means VirtualQuery.
    inline QueryWsExFn ws_query_fn()
    {
        static const QueryWsExFn fn = [] {
            const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            return k32 ? reinterpret_cast<QueryWsExFn>(
                             reinterpret_cast<void *>(GetProcAddress(k32, "K32QueryWorkingSetEx")))
                       : nullptr;
        }();
        return fn;
    }

    inline bool page_probe(uintptr_t a, uint64_t now, Region &out)
    {
        const QueryWsExFn fn = ws_query_fn();
        if (!fn)
            return false;
        const uintptr_t page = a & ~(kPage - 1);
        WsExInfo w{};
        w.VirtualAddress = reinterpret_cast<void *>(page);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const BOOL ok = fn(GetCurrentProcess(), &w, sizeof(w));
        QueryPerformanceCounter(&t1);
        const uint64_t dt = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        g_probe_qpc.fetch_add(dt, std::memory_order_relaxed);
        t_probe_qpc += dt;
        if (!ok || !(w.VirtualAttributes & 1))
        {
            g_probe_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const DWORD prot = static_cast<DWORD>((w.VirtualAttributes >> 4) & 0x7FF);
        Region r{};
        r.base = page;
        r.end = page + kPage;
        r.stamp = now;
        r.read_ok = prot_readable(prot);
        r.write_ok = prot_writable(prot);
        if (!r.read_ok && !r.write_ok)
        {
            g_probe_misses.fetch_add(1, std::memory_order_relaxed);
            return false; // resident but not accessible: let VirtualQuery give the full answer
        }
        out = r;
        return true;
    }

    // Ask the OS. A positive verdict (readable or writable) is cached; a negative one is returned
    // but NOT stored - the engine may commit into that very range on the next allocation, and a
    // cached "no" would blind us to it (the marker-creation lesson in the header comment). A
    // negative answer also EVICTS any cached positive it contradicts, so a freed region does not
    // keep passing on a stale slot until the TTL runs out.
    //
    // want_end is the end of the range the caller is validating: it decides whether the one-page
    // probe may answer (see above).
    inline Region query_region(uintptr_t a, uint64_t now, uintptr_t want_end)
    {
        if (want_end > a && (want_end - 1) / kPage - a / kPage < kProbePages)
        {
            Region p{};
            if (page_probe(a, now, p))
            {
                g_page_hits.fetch_add(1, std::memory_order_relaxed);
                ++t_page_hits;
                // Reuse the slot this address already owns, so a refresh does not evict a
                // different hot region.
                Region *slot = owning_slot(a);
                if (!slot)
                    slot = &t_cache[t_victim++ % kSlots];
                *slot = p;
                return p;
            }
        }
        MEMORY_BASIC_INFORMATION mbi{};
        g_queries.fetch_add(1, std::memory_order_relaxed);
        ++t_queries;
        Region r{};
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const SIZE_T got = VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi));
        QueryPerformanceCounter(&t1);
        const uint64_t dt = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        g_query_qpc.fetch_add(dt, std::memory_order_relaxed);
        t_query_qpc += dt;
        if (got != sizeof(mbi))
            return r; // r.end == 0: not a queryable address at all
        r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        r.end = r.base + mbi.RegionSize;
        r.stamp = now;
        const bool committed = mbi.State == MEM_COMMIT;
        r.read_ok = committed && prot_readable(mbi.Protect);
        r.write_ok = committed && prot_writable(mbi.Protect);
        // Reuse the slot this address already owns if it has one (it may just be stale), so a
        // refresh does not evict a different hot region.
        Region *slot = owning_slot(a);
        if (r.read_ok || r.write_ok)
        {
            if (!slot)
                slot = &t_cache[t_victim++ % kSlots];
            *slot = r;
        }
        else
        {
            // Every slot containing the address, not just the first: one-page probe entries and
            // multi-GB VirtualQuery entries now overlap, and a surviving overlapping positive would
            // answer the very next call against this refusal.
            for (auto &c : t_cache)
                if (c.end && a >= c.base && a < c.end)
                    c = Region{};
        }
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
        // 4 region hops, plus kProbePages: a range whose first pages are cached as one-page probe
        // entries spends a hop per page before it reaches a multi-page region.
        for (int hop = 0; hop < 4 + static_cast<int>(kProbePages) && at < b; ++hop)
        {
            const Region *cached = cache_find(at, now);
            const Region r = cached ? *cached : query_region(at, now, b);
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

    // How many of the first max_n bytes at p are readable. 0 = none. This is the primitive for
    // reads of UNKNOWN length (a string that may end anywhere): `readable()` is all-or-nothing
    // over the whole requested range, so a perfectly good short string near the end of a region
    // is refused at every requested size - the caller here CLAMPS the copy to what exists
    // instead. Same region walk and cache as range_ok, same hop bound.
    inline size_t readable_extent(const void *p, size_t max_n)
    {
        if (!p || max_n == 0)
            return 0;
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        const uintptr_t b = a + max_n;
        if (b < a)
            return 0;
        const uint64_t now = GetTickCount64();
        uintptr_t at = a;
        for (int hop = 0; hop < 4 + static_cast<int>(kProbePages) && at < b; ++hop)
        {
            const Region *cached = cache_find(at, now);
            const Region r = cached ? *cached : query_region(at, now, b);
            if (!r.end || !r.read_ok)
                break;
            at = r.end;
        }
        if (at <= a)
            return 0;
        return static_cast<size_t>((at < b ? at : b) - a);
    }

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
        LARGE_INTEGER qf;
        QueryPerformanceFrequency(&qf);
        spdlog::info("[safemem] {} copies, {} region lookups, {} page probes ({} unanswered, {} us), "
                     "{} refused, {} late faults",
                     g_copies.load(std::memory_order_relaxed),
                     g_queries.load(std::memory_order_relaxed),
                     g_page_hits.load(std::memory_order_relaxed),
                     g_probe_misses.load(std::memory_order_relaxed),
                     qf.QuadPart ? g_probe_qpc.load(std::memory_order_relaxed) * 1000000 /
                                       static_cast<uint64_t>(qf.QuadPart)
                                 : 0,
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
        ++t_copies;
        report_periodically();
        if (!readable(src, n) || !writable(dst, n))
        {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            ++t_refused;
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
