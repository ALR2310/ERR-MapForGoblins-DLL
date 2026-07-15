#include "goblin_stall_probe.hpp"
#include "goblin_config.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <intrin.h> // _ReturnAddress

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <tlhelp32.h>

// Sampling profiler for one game thread. capture() is called on the thread we
// want to observe (map UI thread); the actual sampling runs on a detached helper
// thread so the observed thread keeps running the stall we want to measure.
// While the target is paused we only read its context and scan its frozen stack
// for return addresses into the exe image - no allocation, no logging, so the
// helper can never block on a lock the paused thread holds.
namespace
{
    std::atomic<bool> g_running{false};

    // ── pin-registration cost counters ──────────────────────────────────
    // Pass-through wraps on the three per-marker map-widget virtual methods
    // (vtable slots at 0x142cbc840/848/850; entries 0x1410dbb70 / 0x1410dbea0 /
    // 0x1410dc260 in v1.16) and the typed-find walk (0x14113feb0) they drive.
    // These fns are virtual-dispatched (no static xrefs), so recording the REAL
    // return addresses here is the only reliable way to identify the per-marker
    // driver loop for the soft-populate design. Counting only happens while a
    // capture window is open; otherwise each wrap is one relaxed load + branch.
    // Slots 4/5 (2026-07-14 round 2): the per-frame while-map-open hot spots the
    // sampler surfaced - the child-step loop (0x1411d3980, walks every marker
    // widget per frame calling vtbl+0x348/+0x340 on visible ones) and the
    // transform getter (0x14117e140, full matrix decompose with sqrt+atan2 when
    // its cache ptr [this+0x50] is null, cheap copy when cached).
    // Slots 6-8 (release-in-fade expedition): the GFx render batch internals.
    // item-proc (0x140d716a0) runs per movie-slot on a WORKER; next-capture
    // (0x1411577b0) applies the accumulated display-tree changelist (teardown of
    // ~9k objects lands here per the working theory); display (0x14115cde0) draws.
    // The timings decide whether the close freeze is changelist-apply or draw.
    // Slot 9: the batch job entry itself (0x140d7d720, runs on a worker). Its max
    // vs the ~140ms scheduler wait separates "long job" from "job stuck in queue"
    // (priority inversion in the task system).
    constexpr int N_REG = 10;
    const char *const REG_NAME[N_REG] = {"widget-a",   "widget-b",     "widget-c",
                                         "typed-find", "child-step",   "xform-get",
                                         "item-proc",  "next-capture", "movie-display",
                                         "batch-job"};
    using Fn4 = void *(void *, void *, void *, void *);
    Fn4 *o_reg[N_REG] = {};
    std::atomic<bool> g_count{false};
    std::atomic<uint64_t> g_reg_calls[N_REG];
    std::atomic<uint64_t> g_reg_ticks[N_REG];
    std::atomic<uint64_t> g_reg_max[N_REG]; // worst single call (attributes one-off stalls)

    constexpr size_t RET_SLOTS = 32; // per-fn return-address histogram (linear probe)
    struct RetSlot
    {
        std::atomic<uintptr_t> addr{0};
        std::atomic<uint32_t> cnt{0};
    };
    RetSlot g_ret[N_REG][RET_SLOTS];

    // ── typed-find pointer-scan accelerator ("sequential predictor") ─────────
    // At the widget-b call site (ret = widget-b entry + 0x178) the engine does a
    // pointer-find over the map widget's entry array: an O(N) scan per pin makes
    // the register burst at open and the unregister burst at close O(N^2) -
    // ~280 ms each at ~9k markers (measured 2026-07-15). Both bursts visit pins
    // in container order, so the NEXT query's entry is almost always at the last
    // hit index + 1. We pre-seed the container's own result-cache slot
    // (container[4], which the engine REVALIDATES on every call by locking the
    // entry and comparing the object pointer - see FUN_14113feb0) with that
    // guess: a correct guess returns through the engine's own validated fast
    // path, a wrong one falls back to the normal scan. Either way only engine
    // code decides the result - the predictor cannot change semantics.
    uintptr_t g_wb_find_ret = 0;  // call-site gate; 0 = accelerator off
    uintptr_t g_pred_base = 0;    // container array base we are synced to
    size_t g_pred_idx = SIZE_MAX; // index of the last confirmed hit
    uintptr_t g_pred_guess = 0;   // entry we seeded this call
    size_t g_pred_guess_idx = 0;
    std::atomic<uint64_t> g_pred_hit{0}, g_pred_miss{0};
    std::atomic<uint64_t> g_pred_gate_seen{0}; // ret matched, before the other gate terms
                                               // (diagnoses the round-4 coverage anomaly)

    // ── close-wait job spy ───────────────────────────────────────────────────
    // FUN_140e811c0 = the generic "async job done?" poll (waits on job+0x10); the
    // map UI thread sits in it ~140ms after map close. Recording (caller ret, job
    // vtable) pairs during capture windows identifies WHICH job the close waits
    // on - its vtable RVA is the key to decompiling the job body.
    Fn4 *o_jobpoll = nullptr;
    struct JobSeen
    {
        std::atomic<uintptr_t> ret{0};
        std::atomic<uintptr_t> vt{0};
        std::atomic<uint32_t> cnt{0};
        std::atomic<uint64_t> ticks{0};    // total time INSIDE the poll (incl. blocking)
        std::atomic<uint64_t> max_ticks{0}; // worst single poll - names the freeze site
    };
    JobSeen g_jobs[24];

    void record_job(uintptr_t ret, uintptr_t vt, uint64_t ticks)
    {
        size_t h = ((ret >> 4) ^ (vt >> 4)) % 24;
        for (size_t i = 0; i < 24; ++i)
        {
            JobSeen &s = g_jobs[(h + i) % 24];
            uintptr_t cur = s.ret.load(std::memory_order_relaxed);
            if (cur == 0)
            {
                uintptr_t expected = 0;
                if (!s.ret.compare_exchange_strong(expected, ret, std::memory_order_relaxed))
                {
                    cur = expected;
                }
                else
                {
                    s.vt.store(vt, std::memory_order_relaxed);
                    cur = ret;
                }
            }
            if (cur == ret && s.vt.load(std::memory_order_relaxed) == vt)
            {
                s.cnt.fetch_add(1, std::memory_order_relaxed);
                s.ticks.fetch_add(ticks, std::memory_order_relaxed);
                uint64_t prev = s.max_ticks.load(std::memory_order_relaxed);
                while (ticks > prev &&
                       !s.max_ticks.compare_exchange_weak(prev, ticks, std::memory_order_relaxed))
                {
                }
                return;
            }
        }
    }

    void *jobpoll_detour(void *a, void *b, void *c, void *d)
    {
        if (!g_count.load(std::memory_order_relaxed) || !a)
            return o_jobpoll(a, b, c, d);
        uintptr_t obj = *reinterpret_cast<uintptr_t *>(a);
        uintptr_t vt = obj ? *reinterpret_cast<uintptr_t *>(obj) : 0;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void *r = o_jobpoll(a, b, c, d);
        QueryPerformanceCounter(&t1);
        record_job(reinterpret_cast<uintptr_t>(_ReturnAddress()), vt,
                   static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
        return r;
    }

    void pred_seed(uintptr_t *container)
    {
        g_pred_guess = 0;
        uintptr_t base = container[0];
        size_t cnt = container[1];
        if (!base || base != g_pred_base)
        {
            g_pred_base = base;
            g_pred_idx = SIZE_MAX; // new container: resync on the first result
            return;
        }
        size_t guess = g_pred_idx + 1; // SIZE_MAX + 1 wraps to 0 = first slot
        if (guess >= cnt) return;
        uintptr_t e = *reinterpret_cast<uintptr_t *>(base + guess * 0x10);
        // Same raw guards the engine's own scan applies before locking an entry.
        if (e && (*reinterpret_cast<uint8_t *>(e + 0x6b) & 1))
        {
            container[4] = e; // engine revalidates before trusting it
            g_pred_guess = e;
            g_pred_guess_idx = guess;
        }
    }

    void pred_update(uintptr_t *container, uintptr_t result)
    {
        if (!result)
        {
            g_pred_idx = SIZE_MAX;
            return;
        }
        if (g_pred_guess && result == g_pred_guess)
        {
            g_pred_idx = g_pred_guess_idx;
            g_pred_hit.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Miss: locate the result so the next guess is its neighbor (one O(N)
        // resync per miss keeps the burst ~O(N) overall).
        uintptr_t base = container[0];
        size_t cnt = container[1];
        g_pred_idx = SIZE_MAX;
        if (base == g_pred_base)
            for (size_t i = 0; i < cnt; ++i)
                if (*reinterpret_cast<uintptr_t *>(base + i * 0x10) == result)
                {
                    g_pred_idx = i;
                    break;
                }
        g_pred_miss.fetch_add(1, std::memory_order_relaxed);
    }

    void record_ret(int fn, uintptr_t ret)
    {
        size_t h = (ret >> 4) % RET_SLOTS;
        for (size_t i = 0; i < RET_SLOTS; ++i)
        {
            RetSlot &s = g_ret[fn][(h + i) % RET_SLOTS];
            uintptr_t cur = s.addr.load(std::memory_order_relaxed);
            if (cur == ret)
            {
                s.cnt.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (cur == 0)
            {
                uintptr_t expected = 0;
                if (s.addr.compare_exchange_strong(expected, ret, std::memory_order_relaxed))
                {
                    s.cnt.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (expected == ret)
                {
                    s.cnt.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        }
        // table full: drop (histogram is best-effort)
    }

    template <int I> void *reg_detour(void *a, void *b, void *c, void *d)
    {
        bool accel = false;
        if constexpr (I == 3) // typed-find: sequential predictor at the gated site
        {
            if (g_wb_find_ret &&
                reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_wb_find_ret)
            {
                g_pred_gate_seen.fetch_add(1, std::memory_order_relaxed);
                if (goblin::config::fastMapOpen && a)
                {
                    accel = true;
                    pred_seed(reinterpret_cast<uintptr_t *>(a));
                }
            }
        }
        if (!g_count.load(std::memory_order_relaxed))
        {
            void *r = o_reg[I](a, b, c, d);
            if constexpr (I == 3)
                if (accel)
                    pred_update(reinterpret_cast<uintptr_t *>(a),
                                reinterpret_cast<uintptr_t>(r));
            return r;
        }
        record_ret(I, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void *r = o_reg[I](a, b, c, d);
        QueryPerformanceCounter(&t1);
        if constexpr (I == 3)
            if (accel)
                pred_update(reinterpret_cast<uintptr_t *>(a), reinterpret_cast<uintptr_t>(r));
        uint64_t dt = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        g_reg_calls[I].fetch_add(1, std::memory_order_relaxed);
        g_reg_ticks[I].fetch_add(dt, std::memory_order_relaxed);
        uint64_t prev = g_reg_max[I].load(std::memory_order_relaxed);
        while (dt > prev &&
               !g_reg_max[I].compare_exchange_weak(prev, dt, std::memory_order_relaxed))
        {
        }
        return r;
    }

    void counters_reset()
    {
        for (int i = 0; i < N_REG; ++i)
        {
            g_reg_calls[i].store(0, std::memory_order_relaxed);
            g_reg_ticks[i].store(0, std::memory_order_relaxed);
            g_reg_max[i].store(0, std::memory_order_relaxed);
            for (auto &s : g_ret[i])
            {
                s.addr.store(0, std::memory_order_relaxed);
                s.cnt.store(0, std::memory_order_relaxed);
            }
        }
    }

    void counters_log(const std::string &tag, uintptr_t exe_base)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        for (int i = 0; i < N_REG; ++i)
        {
            uint64_t calls = g_reg_calls[i].load(std::memory_order_relaxed);
            if (!calls) continue;
            double ms = double(g_reg_ticks[i].load(std::memory_order_relaxed)) * 1000.0 /
                        double(f.QuadPart);
            double max_ms = double(g_reg_max[i].load(std::memory_order_relaxed)) * 1000.0 /
                            double(f.QuadPart);
            std::vector<std::pair<uint32_t, uintptr_t>> tops;
            for (auto &s : g_ret[i])
                if (s.addr.load(std::memory_order_relaxed))
                    tops.push_back({s.cnt.load(std::memory_order_relaxed),
                                    s.addr.load(std::memory_order_relaxed)});
            std::sort(tops.rbegin(), tops.rend());
            std::string line;
            for (size_t k = 0; k < tops.size() && k < 6; ++k)
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "exe+0x%llX x%u  ",
                         (unsigned long long)(tops[k].second - exe_base), tops[k].first);
                line += buf;
            }
            spdlog::info("[stallprobe] {}: {} = {} calls / {:.1f} ms (max {:.1f}); ret: {}",
                         tag, REG_NAME[i], calls, ms, max_ms, line);
        }
    }

    struct ModRange
    {
        uintptr_t base = 0, end = 0;
    };

    ModRange module_range(const wchar_t *name)
    {
        HMODULE h = GetModuleHandleW(name);
        if (!h) return {};
        auto base = reinterpret_cast<uintptr_t>(h);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        return {base, base + nt->OptionalHeader.SizeOfImage};
    }

    // First executable section (.text) of a module. Frames are only accepted from
    // here: whole-image filtering let .data globals (e.g. exe+0x3D8xxxx singleton
    // pointers sitting on the stack) pollute the caller histograms in round 1.
    ModRange text_range(const wchar_t *name)
    {
        HMODULE h = GetModuleHandleW(name);
        if (!h) return {};
        auto base = reinterpret_cast<uintptr_t>(h);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        auto *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                return {base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        return {base, base + nt->OptionalHeader.SizeOfImage};
    }

    struct Sample
    {
        uintptr_t rip = 0;
        uintptr_t exe_frames[4] = {};
        int n_exe = 0;
    };

    void log_hist(const std::string &tag, const char *what,
                  const std::map<uintptr_t, int> &hist, uintptr_t exe_base, int total)
    {
        if (hist.empty() || !total) return;
        // top 10 by count
        std::vector<std::pair<uintptr_t, int>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        std::string line;
        int n = 0;
        for (auto &[addr, cnt] : v)
        {
            if (n++ >= 10) break;
            char buf[64];
            snprintf(buf, sizeof(buf), "exe+0x%llX x%d  ",
                     (unsigned long long)(addr - exe_base), cnt);
            line += buf;
        }
        spdlog::info("[stallprobe] {}: {} ({} samples): {}", tag, what, total, line);
    }

    void run_capture(std::string tag, DWORD tid, unsigned duration_ms)
    {
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION,
                               FALSE, tid);
        if (!th)
        {
            g_running.store(false);
            return;
        }
        ModRange exe = module_range(nullptr);
        ModRange ntdll = module_range(L"ntdll.dll");
        ModRange xtext = text_range(nullptr); // frames accepted from exe .text ONLY

        // All-thread sweep (every ~20ms): the target (map UI) thread turned out to
        // WAIT on a DLConditionSignal during the post-close stall while a worker
        // thread does the actual work - the sweep finds that worker. One thread is
        // paused at a time and only its context/stack is read while paused.
        struct SweepSample
        {
            DWORD tid;
            uintptr_t rip, frame0;
            bool active; // rip moved since this thread's previous sweep pass
        };
        std::vector<DWORD> sw_tids;
        std::vector<HANDLE> sw_handles;
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
                if (Thread32First(snap, &te)) do
                    {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self &&
                            sw_tids.size() < 256)
                        {
                            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                      THREAD_QUERY_INFORMATION,
                                                  FALSE, te.th32ThreadID);
                            if (h)
                            {
                                sw_tids.push_back(te.th32ThreadID);
                                sw_handles.push_back(h);
                            }
                        }
                    } while (Thread32Next(snap, &te));
                CloseHandle(snap);
            }
        }
        std::vector<SweepSample> sweeps;
        sweeps.reserve((duration_ms / 20 + 2) * (sw_tids.empty() ? 1 : sw_tids.size()));
        std::vector<uintptr_t> sw_last(sw_tids.size(), 0); // parked-thread filter

        counters_reset();
        g_count.store(true, std::memory_order_relaxed);

        std::vector<Sample> samples;
        samples.reserve(duration_ms + 64);

        // Scan a paused thread's stack for return addresses into the exe .text.
        auto scan_first_text_frame = [&](uintptr_t sp, uintptr_t *out, int max_out) -> int {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!sp || !VirtualQuery(reinterpret_cast<void *>(sp), &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                return 0;
            uintptr_t lim = std::min<uintptr_t>(
                reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, sp + 48 * 1024);
            int n = 0;
            for (uintptr_t p = sp; p + 8 <= lim && n < max_out; p += 8)
            {
                uintptr_t v = *reinterpret_cast<uintptr_t *>(p);
                if (v >= xtext.base && v < xtext.end) out[n++] = v;
            }
            return n;
        };

        ULONGLONG t0 = GetTickCount64();
        alignas(16) CONTEXT ctx;
        unsigned iter = 0;
        unsigned stall_run = 0; // consecutive samples with the target parked in ntdll
        while (GetTickCount64() - t0 < duration_ms)
        {
            if (SuspendThread(th) == (DWORD)-1) break;
            ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            Sample s{};
            bool ok = GetThreadContext(th, &ctx) != 0;
            if (ok)
            {
                s.rip = static_cast<uintptr_t>(ctx.Rip);
                s.n_exe = scan_first_text_frame(static_cast<uintptr_t>(ctx.Rsp), s.exe_frames, 4);
            }
            ResumeThread(th);
            if (ok) samples.push_back(s);

            // Adaptive burst: while the target sits in ntdll for >20ms straight (a
            // long wait = the stall we hunt), sweep every pass so the thread doing
            // the actual work during the stall is captured densely.
            bool stalled = ok && s.rip >= ntdll.base && s.rip < ntdll.end;
            stall_run = stalled ? stall_run + 1 : 0;

            if ((iter++ % 20) == 0 || (stall_run > 20 && sweeps.size() < 400000))
            {
                for (size_t k = 0; k < sw_handles.size(); ++k)
                {
                    if (SuspendThread(sw_handles[k]) == (DWORD)-1) continue;
                    ctx = {};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    SweepSample ss{sw_tids[k], 0, 0, false};
                    if (GetThreadContext(sw_handles[k], &ctx))
                    {
                        ss.rip = static_cast<uintptr_t>(ctx.Rip);
                        // A parked thread (blocked in a wait) reports the same rip
                        // every pass; only a MOVING rip means the thread works.
                        ss.active = (ss.rip != sw_last[k]);
                        sw_last[k] = ss.rip;
                        // Only active ntdll-time threads get the (pricier) stack scan.
                        if (ss.active && ss.rip >= ntdll.base && ss.rip < ntdll.end)
                        {
                            uintptr_t f = 0;
                            if (scan_first_text_frame(static_cast<uintptr_t>(ctx.Rsp), &f, 1))
                                ss.frame0 = f;
                        }
                    }
                    ResumeThread(sw_handles[k]);
                    if (ss.rip) sweeps.push_back(ss);
                }
            }
            Sleep(1);
        }
        CloseHandle(th);
        for (HANDLE h : sw_handles) CloseHandle(h);
        g_count.store(false, std::memory_order_relaxed);

        // Aggregate. rip location tells WHERE time went (exe vs ntdll = heap ops);
        // for non-exe rips the first stack frame into the exe is the caller we
        // need for a targeted patch. "other" rips are attributed by MODULE NAME
        // (GPU driver vs other mods vs D3D runtime decides the close-freeze story).
        auto module_of = [](uintptr_t rip) -> std::string {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void *>(rip), &mbi, sizeof(mbi)) ||
                !mbi.AllocationBase)
                return "<jit/unmapped>";
            char path[MAX_PATH] = {};
            if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), path,
                                    MAX_PATH))
                return "<jit/unmapped>";
            const char *base = strrchr(path, '\\');
            return base ? base + 1 : path;
        };
        int in_exe = 0, in_ntdll = 0, other = 0;
        std::map<uintptr_t, int> rip_exe, caller0, caller1;
        std::map<std::string, int> other_mods;
        for (const auto &s : samples)
        {
            if (s.rip >= exe.base && s.rip < exe.end)
            {
                in_exe++;
                rip_exe[s.rip]++;
            }
            else
            {
                if (s.rip >= ntdll.base && s.rip < ntdll.end)
                {
                    in_ntdll++;
                }
                else
                {
                    other++;
                    other_mods[module_of(s.rip)]++;
                }
                if (s.n_exe > 0) caller0[s.exe_frames[0]]++;
                if (s.n_exe > 1) caller1[s.exe_frames[1]]++;
            }
        }
        int total = static_cast<int>(samples.size());
        spdlog::info("[stallprobe] {}: {} samples over {} ms: exe {} / ntdll {} / other {}",
                     tag, total, duration_ms, in_exe, in_ntdll, other);
        if (!other_mods.empty())
        {
            std::vector<std::pair<int, std::string>> om;
            for (auto &[n, c] : other_mods) om.push_back({c, n});
            std::sort(om.rbegin(), om.rend());
            std::string line;
            for (size_t j = 0; j < om.size() && j < 6; ++j)
                line += om[j].second + " x" + std::to_string(om[j].first) + "; ";
            spdlog::info("[stallprobe] {}: other-module rip: {}", tag, line);
        }
        log_hist(tag, "exe rip hot spots", rip_exe, exe.base, in_exe);
        log_hist(tag, "ntdll-time exe callers (frame0)", caller0, exe.base, in_ntdll + other);
        log_hist(tag, "ntdll-time exe callers (frame1)", caller1, exe.base, in_ntdll + other);

        // Sweep result: which threads spent the window inside ntdll (heap/waits),
        // ranked; each with its top exe-.text callers. This is what identifies the
        // worker that performs the deferred post-close release work.
        {
            // Rank threads by ACTIVE samples only (moving rip): parked worker-pool
            // threads sit in ntdll waits with a frozen rip and are not work.
            std::map<DWORD, int> tid_act_ntdll, tid_act_exe, tid_act_other, tid_total;
            std::map<DWORD, std::map<uintptr_t, int>> tid_frames, tid_ntoff;
            std::map<std::string, int> sweep_other_mods;
            for (const auto &ss : sweeps)
            {
                tid_total[ss.tid]++;
                if (!ss.active) continue;
                if (ss.rip >= ntdll.base && ss.rip < ntdll.end)
                {
                    tid_act_ntdll[ss.tid]++;
                    tid_ntoff[ss.tid][ss.rip - ntdll.base]++;
                    if (ss.frame0) tid_frames[ss.tid][ss.frame0]++;
                }
                else if (ss.rip >= exe.base && ss.rip < exe.end)
                {
                    tid_act_exe[ss.tid]++;
                }
                else
                {
                    tid_act_other[ss.tid]++;
                    sweep_other_mods[module_of(ss.rip)]++;
                }
            }
            auto top_of = [](std::map<uintptr_t, int> &m, const char *pfx, uintptr_t rebase) {
                std::vector<std::pair<int, uintptr_t>> v;
                for (auto &[a, c] : m) v.push_back({c, a});
                std::sort(v.rbegin(), v.rend());
                std::string s;
                for (size_t j = 0; j < v.size() && j < 3; ++j)
                {
                    char b[48];
                    snprintf(b, sizeof(b), "%s+0x%llX x%d ", pfx,
                             (unsigned long long)(v[j].second - rebase), v[j].first);
                    s += b;
                }
                return s;
            };
            std::map<DWORD, int> tid_act_all;
            for (auto &[t, c] : tid_act_ntdll) tid_act_all[t] += c;
            for (auto &[t, c] : tid_act_exe) tid_act_all[t] += c;
            for (auto &[t, c] : tid_act_other) tid_act_all[t] += c;
            std::vector<std::pair<int, DWORD>> rank;
            for (auto &[t, c] : tid_act_all) rank.push_back({c, t});
            std::sort(rank.rbegin(), rank.rend());
            std::string line;
            int shown = 0;
            for (auto &[cnt, t] : rank)
            {
                if (shown++ >= 6) break;
                char buf[240];
                snprintf(buf, sizeof(buf),
                         "tid %lu act %d (nt %d exe %d oth %d) of %d [%s| %s]; ",
                         (unsigned long)t, cnt, tid_act_ntdll[t], tid_act_exe[t],
                         tid_act_other[t], tid_total[t],
                         top_of(tid_ntoff[t], "nt", 0).c_str(),
                         top_of(tid_frames[t], "exe", exe.base).c_str());
                line += buf;
            }
            spdlog::info("[stallprobe] {}: sweep {} threads (active only): {}", tag,
                         sw_tids.size(), line.empty() ? "no active threads" : line);
            if (!sweep_other_mods.empty())
            {
                std::vector<std::pair<int, std::string>> om;
                for (auto &[n, c] : sweep_other_mods) om.push_back({c, n});
                std::sort(om.rbegin(), om.rend());
                std::string ml;
                for (size_t j = 0; j < om.size() && j < 6; ++j)
                    ml += om[j].second + " x" + std::to_string(om[j].first) + "; ";
                spdlog::info("[stallprobe] {}: sweep other-module rip: {}", tag, ml);
            }
        }

        counters_log(tag, exe.base);
        // Sequential-predictor effectiveness (accumulated since the last window).
        {
            uint64_t ph = g_pred_hit.exchange(0, std::memory_order_relaxed);
            uint64_t pm = g_pred_miss.exchange(0, std::memory_order_relaxed);
            uint64_t gs = g_pred_gate_seen.exchange(0, std::memory_order_relaxed);
            if (ph + pm + gs)
                spdlog::info("[stallprobe] {}: find-predictor {} hits / {} misses / {} gate-seen",
                             tag, ph, pm, gs);
        }
        // Job-poll spy: which async jobs were waited on during this window and for
        // how long. The site with the big max is the one the close-freeze blocks in.
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            const double to_ms = 1000.0 / double(f.QuadPart);
            std::string line;
            for (auto &s : g_jobs)
            {
                uintptr_t r = s.ret.exchange(0, std::memory_order_relaxed);
                uintptr_t v = s.vt.exchange(0, std::memory_order_relaxed);
                uint32_t c = s.cnt.exchange(0, std::memory_order_relaxed);
                uint64_t tk = s.ticks.exchange(0, std::memory_order_relaxed);
                uint64_t mx = s.max_ticks.exchange(0, std::memory_order_relaxed);
                if (!r || !c) continue;
                // Only report sites that actually spent time (>0.5ms total) or ran
                // often - keeps the line readable.
                double total_ms = double(tk) * to_ms, max_ms = double(mx) * to_ms;
                if (total_ms < 0.5 && c < 50) continue;
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "ret exe+0x%llX vt exe+0x%llX x%u %.1fms (max %.1f); ",
                         (unsigned long long)(r - exe.base),
                         (unsigned long long)(v ? v - exe.base : 0), c, total_ms, max_ms);
                line += buf;
            }
            if (!line.empty())
                spdlog::info("[stallprobe] {}: job-poll: {}", tag, line);
        }
        g_running.store(false);
    }
} // namespace

void goblin::stall_probe::setup()
{
    // AOB-resolved entries (v1.16 file VAs in the comment above). Each pattern was
    // verified unique in the exe; a miss disables just that counter (feature is
    // diagnostics-only, the game runs unchanged).
    struct Target
    {
        int idx;
        const char *aob;
    };
    const Target targets[] = {
        {0, "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC "
            "A0 00 00 00 48 8B"},
        {1, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D "
            "6C 24 D1 48 81 EC 90 00 00 00 48 8B 41 08"},
        {2, "48 89 5C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC"},
        {3, "40 53 41 55 41 57 48 83 EC 30 33 DB 4C 8B F9 89 5C 24 58 4C 8B EA 48 8B"},
        {4, "48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 20 8B A9 B8 00 00 00 4C 8B F1 "
            "48 8B B1 E0 00 00 00 C1 ED 03 40 80 E5 01 48"},
        {5, "48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 51 50 48 85 D2 0F"},
        {6, "40 53 56 57 48 83 EC 20 48 8B F9 83 CA FF 48 81 C1 88 00 00 00"},
        {7, "48 89 5C 24 20 57 41 56 41 57 48 83 EC 20 48 8B 19 4D 8B F8 4C 8B F2 48"},
        {8, "48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 80 00 00 00 48 8B"},
        {9, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 48 8B F2 "
            "E8 01 C4 FE FF 83"},
    };
    static Fn4 *detours[N_REG] = {reg_detour<0>, reg_detour<1>, reg_detour<2>,
                                  reg_detour<3>, reg_detour<4>, reg_detour<5>,
                                  reg_detour<6>, reg_detour<7>, reg_detour<8>,
                                  reg_detour<9>};
    int armed = 0;
    for (const auto &t : targets)
    {
        try
        {
            auto *fn = modutils::hook<Fn4>({.aob = t.aob}, *detours[t.idx], o_reg[t.idx]);
            // widget-b: its typed-find call site (entry+0x173, E8 rel32) returns to
            // entry+0x178 - the gate for the sequential predictor above.
            if (t.idx == 1)
                g_wb_find_ret = reinterpret_cast<uintptr_t>(fn) + 0x178;
            armed++;
        }
        catch (const std::exception &e)
        {
            spdlog::warn("[stallprobe] counter '{}' unavailable: {}", REG_NAME[t.idx], e.what());
        }
    }
    spdlog::info("[stallprobe] {} of {} cost counters armed", armed, N_REG);

    // Close-wait job spy (diagnostics only; pass-through outside capture windows).
    try
    {
        modutils::hook<Fn4>(
            {.aob = "48 83 EC 28 48 8B 09 48 85 C9 74 16 48 83 C1 10 E8 FB A6 08 01 85 C0 0F"},
            jobpoll_detour, o_jobpoll);
        spdlog::info("[stallprobe] job-poll spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] job-poll spy unavailable: {}", e.what());
    }
}

void goblin::stall_probe::capture(const char *tag, unsigned duration_ms)
{
    if (!goblin::config::debugLogging) return;
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    DWORD tid = GetCurrentThreadId();
    try
    {
        std::thread(run_capture, std::string(tag ? tag : "capture"), tid, duration_ms).detach();
    }
    catch (...)
    {
        g_running.store(false);
    }
}
