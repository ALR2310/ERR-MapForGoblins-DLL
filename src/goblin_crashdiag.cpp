#include "goblin_crashdiag.hpp"

#include "goblin_config.hpp"
#include "goblin_safemem.hpp" // validate-then-read for the survivor walk
#include "version.h"          // PROJECT_VERSION / BUILD_NAME / GIT_HASH, for format_env

#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

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

    // ── when things happened ────────────────────────────────────────────────────────────────────
    // "mapOpen=1" says the map was up; it does not say whether the crash landed one second into
    // the open or twenty minutes in, and a tear-down race looks like neither. Ticks are stored
    // raw and turned into ages only when a record is written.
    const uint64_t g_t0 = GetTickCount64();
    std::atomic<uint64_t> g_t_open{0};  // last completed map open
    std::atomic<uint64_t> g_t_close{0}; // last map close

    // ── our own threads ─────────────────────────────────────────────────────────────────────────
    constexpr size_t OWN_TID_MAX = 8;
    std::atomic<uint32_t> g_own_tids[OWN_TID_MAX] = {};

    bool tid_is_ours(uint32_t tid)
    {
        for (size_t i = 0; i < OWN_TID_MAX; ++i)
            if (g_own_tids[i].load(std::memory_order_relaxed) == tid)
                return true;
        return false;
    }

    // ── the running game build, resolved once ───────────────────────────────────────────────────
    std::atomic<uint64_t> g_game_ver{0};
    std::atomic<uint32_t> g_game_img{0};
    std::atomic<uint32_t> g_game_ts{0};

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
    // Carries the "0x" prefix, the same as dllmain's crash_hex64. It did not until 2026-08-07,
    // which put two formats in one record: `[regs] rax=0x1` next to `parent=1BC1E35D5A0`, and
    // `gimg=5E01800` next to `base=0x7FFA377F0000` on the line above it.
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
        buf[len++] = '0';
        buf[len++] = 'x';
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
    g_t_open.store(GetTickCount64(), std::memory_order_relaxed);
}

void goblin::crashdiag::note_own_thread()
{
    const uint32_t tid = GetCurrentThreadId();
    for (size_t i = 0; i < OWN_TID_MAX; ++i)
    {
        uint32_t empty = 0;
        if (g_own_tids[i].compare_exchange_strong(empty, tid, std::memory_order_relaxed))
            return;
        if (empty == tid)
            return; // already registered
    }
}

void goblin::crashdiag::note_map_closed(uint32_t tracked)
{
    g_closes.fetch_add(1, std::memory_order_relaxed);
    g_tracked.store(tracked, std::memory_order_relaxed);
    g_map_open.store(0, std::memory_order_relaxed);
    g_t_close.store(GetTickCount64(), std::memory_order_relaxed);
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
    if (cap < 384)
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

    // Ages, in seconds, of the three moments that decide how to read everything above: how long
    // the session ran, and how long ago the map last opened and last closed. "-1" = never
    // happened. A crash a second after a close is a tear-down race; the same record twenty
    // minutes after one is not, and until now the two were indistinguishable.
    const uint64_t now = GetTickCount64();
    const uint64_t t_open = g_t_open.load(std::memory_order_relaxed);
    const uint64_t t_close = g_t_close.load(std::memory_order_relaxed);
    const uint32_t tid = GetCurrentThreadId();
    len += wsprintfA(buf + len, " up=%us tOpen=%ds tClose=%ds tid=%u own=%u",
                     static_cast<unsigned>((now - g_t0) / 1000),
                     t_open ? static_cast<int>((now - t_open) / 1000) : -1,
                     t_close ? static_cast<int>((now - t_close) / 1000) : -1, tid,
                     tid_is_ours(tid) ? 1u : 0u);
    buf[len++] = '\n';
    return len;
}

void goblin::crashdiag::resolve_game_build()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe)
        return;

    // SizeOfImage and TimeDateStamp straight out of the loaded headers. These two are the honest
    // build identity - they are what separates 2.6.2 from a repacked or downpatched 2.6.2, and a
    // report whose anchors misbehaved is a report where these are the first numbers wanted.
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
            reinterpret_cast<const uint8_t *>(exe) + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE)
        {
            g_game_img.store(nt->OptionalHeader.SizeOfImage, std::memory_order_relaxed);
            g_game_ts.store(nt->FileHeader.TimeDateStamp, std::memory_order_relaxed);
        }
    }

    // FileVersion out of the image's own VERSIONINFO. The resource blob is walked for the
    // VS_FIXEDFILEINFO signature rather than parsed structurally: the string tables in front of it
    // vary by build and locale, and a search for one 32-bit constant cannot be tripped up by that.
    // RT_VERSION is MAKEINTRESOURCE(16), i.e. the ANSI form unless the whole build is UNICODE -
    // this one is not, so the type is spelled out rather than taken from the macro.
    HRSRC res = FindResourceW(exe, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
    if (!res)
        return;
    const DWORD size = SizeofResource(exe, res);
    HGLOBAL h = LoadResource(exe, res);
    if (!h || size < 16)
        return;
    const auto *p = static_cast<const uint32_t *>(LockResource(h));
    if (!p)
        return;
    for (DWORD i = 0; i + 4 <= size / 4; ++i)
    {
        if (p[i] != 0xFEEF04BDu || (p[i + 1] & 0xFFFF0000u) != 0x00010000u)
            continue;
        g_game_ver.store((static_cast<uint64_t>(p[i + 2]) << 32) | p[i + 3],
                         std::memory_order_relaxed);
        break;
    }
}

uint64_t goblin::crashdiag::game_version()
{
    return g_game_ver.load(std::memory_order_relaxed);
}

uint32_t goblin::crashdiag::game_image_size()
{
    return g_game_img.load(std::memory_order_relaxed);
}

uint32_t goblin::crashdiag::game_timestamp()
{
    return g_game_ts.load(std::memory_order_relaxed);
}

int goblin::crashdiag::format_env(char *buf, int cap)
{
    if (cap < 256)
        return 0;
    int len = wsprintfA(buf, "  [env] mfg=%s %s %s game=", PROJECT_VERSION, BUILD_NAME, GIT_HASH);
    const uint64_t v = g_game_ver.load(std::memory_order_relaxed);
    if (v)
        len += wsprintfA(buf + len, "%u.%u.%u.%u", static_cast<unsigned>((v >> 48) & 0xFFFF),
                         static_cast<unsigned>((v >> 32) & 0xFFFF),
                         static_cast<unsigned>((v >> 16) & 0xFFFF),
                         static_cast<unsigned>(v & 0xFFFF));
    else
        buf[len++] = '?';
    len += wsprintfA(buf + len, " gimg=");
    len += hex64(buf + len, g_game_img.load(std::memory_order_relaxed));
    len += wsprintfA(buf + len, " gts=");
    len += hex64(buf + len, g_game_ts.load(std::memory_order_relaxed));
    buf[len++] = '\n';
    return len;
}

// ── the module inventory ────────────────────────────────────────────────────────────────────────
namespace
{
    goblin::crashdiag::RawSink g_raw_sink = nullptr;

    // VERSIONINFO of any loaded module, by the same signature scan resolve_game_build() uses on
    // the exe. 0 when the module carries no version resource (plenty of mod DLLs do not).
    uint64_t fixed_version_of(HMODULE m)
    {
        HRSRC res = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
        if (!res)
            return 0;
        const DWORD size = SizeofResource(m, res);
        HGLOBAL h = LoadResource(m, res);
        if (!h || size < 16)
            return 0;
        const auto *p = static_cast<const uint32_t *>(LockResource(h));
        if (!p)
            return 0;
        for (DWORD i = 0; i + 4 <= size / 4; ++i)
            if (p[i] == 0xFEEF04BDu && (p[i + 1] & 0xFFFF0000u) == 0x00010000u)
                return (static_cast<uint64_t>(p[i + 2]) << 32) | p[i + 3];
        return 0;
    }

    uint32_t image_size_of(HMODULE m)
    {
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m);
        if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
            reinterpret_cast<const uint8_t *>(m) + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
    }

    // The OS's own DLLs are noise - a hundred of them, identical on every machine. What matters is
    // what ELSE somebody put in the process.
    bool is_system_path(const std::wstring &lower)
    {
        static const wchar_t *dirs[] = {L"\\windows\\", L"\\system32\\", L"\\syswow64\\",
                                        L"\\winsxs\\"};
        for (const wchar_t *d : dirs)
            if (lower.find(d) != std::wstring::npos)
                return true;
        return false;
    }

    // The module's file name as UTF-8, and NOTHING it can throw.
    //
    // This used to be `std::filesystem::path(full).filename().string()`, which converts to the
    // system ANSI code page and throws std::system_error when a character has no mapping there.
    // Report 43 (2026-09-17, a Traditional Chinese / cp950 machine) hit exactly that on the t+30s
    // inventory pass, and the cost was completely out of proportion to a log line: the throw
    // unwound out of setup_mod, the DllMain catch called modutils::deinitialize(), MinHook freed
    // its trampoline blocks, and the gamepad poll thread - still running - called through a freed
    // trampoline and took an execute fault on a 64 KB FREE region. The whole mod went down, twice,
    // 30 seconds into the session, because one late-loading DLL had a character in its file name
    // that cp950 cannot represent. Both sessions in that report died at init+30s to the second.
    //
    // CP_UTF8 with no flags never fails this way (no WC_ERR_INVALID_CHARS), the log file is UTF-8
    // anyway, and an unconvertible name now costs its own characters rather than the session.
    std::string module_name_utf8(const wchar_t *full)
    {
        const wchar_t *base = wcsrchr(full, L'\\');
        base = base ? base + 1 : full;
        const int n = WideCharToMultiByte(CP_UTF8, 0, base, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1)
            return std::string("<unnamed>");
        std::string out(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, base, -1, out.data(), n, nullptr, nullptr);
        return out;
    }

    using EnumModulesFn = BOOL(WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD);

    EnumModulesFn resolve_enum_modules()
    {
        static EnumModulesFn fn = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
                fn = reinterpret_cast<EnumModulesFn>(
                    reinterpret_cast<void *>(GetProcAddress(k32, "K32EnumProcessModules")));
        }
        return fn;
    }
}

void goblin::crashdiag::set_raw_sink(RawSink sink)
{
    g_raw_sink = sink;
}

void goblin::crashdiag::log_modules(const char *tag)
{
    EnumModulesFn enum_modules = resolve_enum_modules();
    if (!enum_modules)
        return;

    std::vector<HMODULE> mods(512);
    DWORD needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods.data(),
                      static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed))
        return;
    const size_t count = needed / sizeof(HMODULE);
    mods.resize(count < mods.size() ? count : mods.size());

    std::vector<std::string> lines;
    for (HMODULE m : mods)
    {
        wchar_t full[MAX_PATH] = {0};
        if (!GetModuleFileNameW(m, full, MAX_PATH))
            continue;
        std::wstring lower(full);
        for (auto &c : lower)
            c = static_cast<wchar_t>(towlower(c));
        if (is_system_path(lower))
            continue;
        const uint64_t v = fixed_version_of(m);
        char buf[512];
        // wsprintfA does not bound-check, and a file name is only bounded by MAX_PATH - so the
        // one field that comes from outside gets clipped before it reaches the buffer.
        std::string name = module_name_utf8(full);
        if (name.size() > 96)
            name.resize(96);
        int len = wsprintfA(buf, "  %-32s v%u.%u.%u.%u base=", name.c_str(),
                            static_cast<unsigned>((v >> 48) & 0xFFFF),
                            static_cast<unsigned>((v >> 32) & 0xFFFF),
                            static_cast<unsigned>((v >> 16) & 0xFFFF),
                            static_cast<unsigned>(v & 0xFFFF));
        len += hex64(buf + len, reinterpret_cast<uintptr_t>(m));
        len += wsprintfA(buf + len, " size=");
        len += hex64(buf + len, image_size_of(m));
        lines.emplace_back(buf, static_cast<size_t>(len));
    }

    // The second pass exists to catch what loaded late; if nothing did, saying so once is enough.
    static size_t s_last_count = 0;
    if (!lines.empty() && lines.size() == s_last_count)
        return;
    s_last_count = lines.size();

    char head[128];
    const int head_len =
        wsprintfA(head, "\n  [modules] %s: %u non-system\n", tag ? tag : "?",
                  static_cast<unsigned>(lines.size()));
    spdlog::info("[modules] {}: {} non-system", tag ? tag : "?", lines.size());
    if (g_raw_sink)
        g_raw_sink(head, head_len);
    for (const auto &l : lines)
    {
        spdlog::info("{}", l);
        if (g_raw_sink)
        {
            g_raw_sink(l.c_str(), static_cast<int>(l.size()));
            g_raw_sink("\n", 1);
        }
    }
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

    // Find the manager by WHAT IT IS, not by where it was.
    //
    // This used to read a baked exe+0x3D87350, measured on 2.6.2. That address is wrong on 1.17
    // and on 2.2.x, so the one instrument that would have shown the Scaleform arena filling up
    // was blind on exactly the builds where the arena filled (the DL_PANIC player report). The
    // probe already carries a strong validator - a capacity in the right order of magnitude with
    // free <= cap - so use it as the search key: walk the pointer-sized words of the writable
    // data sections, treat each as a candidate manager, and accept the ONE whose chain validates.
    // Reads are guarded; a candidate that faults is simply not it.
    uintptr_t arena_scan(uint64_t &out_cap, uint64_t &out_free, uint64_t &out_lo, uint64_t &out_hi)
    {
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!exe)
            return 0;
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(exe + dos->e_lfanew);
        const auto *sec = IMAGE_FIRST_SECTION(nt);
        uintptr_t found = 0;
        unsigned hits = 0;
        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections && hits < 2; ++s, ++sec)
        {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;                              // singleton slots live in writable data
            const uintptr_t begin = exe + sec->VirtualAddress;
            const size_t len = sec->Misc.VirtualSize;
            for (size_t off = 0; off + 8 <= len && hits < 2; off += 8)
            {
                uint64_t mgr = 0, inner = 0;
                if (!probe_read64(begin + off, mgr) || mgr < 0x10000 || (mgr & 7))
                    continue;
                if (!probe_read64(static_cast<uintptr_t>(mgr) + 8, inner) || inner < 0x10000 ||
                    (inner & 7))
                    continue;
                const uintptr_t impl = static_cast<uintptr_t>(inner) + 0x28;
                uint64_t cap = 0, free_bytes = 0, lo = 0, hi = 0;
                if (!probe_read64(impl + 0x00, cap) || !probe_read64(impl + 0x08, free_bytes))
                    continue;
                if (cap < 0x04000000 || cap > 0x20000000 || free_bytes > cap)
                    continue;
                probe_read64(impl + 0x38, lo);
                probe_read64(impl + 0x48, hi);
                ++hits;
                found = impl;
                out_cap = cap; out_free = free_bytes; out_lo = lo; out_hi = hi;
                spdlog::info("[arena] manager slot found at exe+0x{:X} (capacity {} MiB)",
                             (uint64_t)(begin + off - exe), cap / (1024 * 1024));
            }
        }
        if (hits != 1)
        {
            spdlog::info("[arena] disabled: {} candidate manager(s) in writable data - "
                         "not guessing between them", hits);
            return 0;
        }
        return found;
    }

    uintptr_t arena_resolve()
    {
        if (g_arena_tried) return g_arena;
        g_arena_tried = true;
        // Every refusal below SAYS SO. The first version returned 0 in silence, and two whole test
        // runs produced no [arena] line at all with no way to tell "the probe is not in this build"
        // from "the probe could not resolve" - a diagnostic that cannot report its own failure is
        // worse than none.
        uint64_t cap = 0, free_bytes = 0, lo = 0, hi = 0;
        const uintptr_t impl = arena_scan(cap, free_bytes, lo, hi);
        if (!impl)
            return 0;                       // arena_scan said why
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

// ---- first-chance fault counter --------------------------------------------------------------
// See the header for why this exists. The handler runs on EVERY exception in the process, so it
// does a range compare and two relaxed atomics and nothing else - no formatting, no locks, no
// reads through the faulting pointer. It never handles anything.
namespace
{
    uintptr_t g_self_lo = 0, g_self_hi = 0;
    std::atomic<uint64_t> g_faults{0};
    // The busiest faulting sites, as module-relative addresses. A tiny fixed table: a read loop
    // that stalls the game faults from one or two places, and those are the ones worth naming.
    // Keyed by (reader, caller): the reader is the guarded primitive that faulted (v3_read64 and
    // its kin - the same handful every time), the caller is the frame above it, which is the one a
    // fix has to look at. One RtlVirtualUnwind step, only for faults inside our own image.
    // Two frames above the fault, not one: the guarded primitives are tiny and the linker folds
    // identical ones together (goblin_collected's safe_read and goblin_markers' seh_copy are one
    // function in the image), so "memcpy <- safe_read" named no caller at all - measured 2026-09-11,
    // a steady 20 faults per report that could have come from either file.
    struct FaultSite
    {
        std::atomic<uint32_t> rva;
        std::atomic<uint32_t> caller;
        std::atomic<uint32_t> caller2;
        std::atomic<uint32_t> hits;
    };
    FaultSite g_sites[12];

    // One unwind step from `c`; false when the frame cannot be established.
    bool unwind_one(CONTEXT &c)
    {
        ULONG64 image_base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (rf)
        {
            void *handler_data = nullptr;
            ULONG64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler_data, &establisher, nullptr);
            return c.Rip != 0;
        }
        // Leaf function: the return address is at the top of the stack.
        __try
        {
            c.Rip = *reinterpret_cast<const ULONG64 *>(c.Rsp);
            c.Rsp += 8;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return c.Rip != 0;
    }

    uint32_t own_rva(ULONG64 rip)
    {
        return (rip >= g_self_lo && rip < g_self_hi) ? static_cast<uint32_t>(rip - g_self_lo) : 0;
    }

    // The two frames above the faulting instruction, as our module-relative addresses; 0 when a
    // frame is not ours (an engine callback into a guarded helper) or cannot be established.
    void caller_rvas(const CONTEXT *ctx, uint32_t &caller, uint32_t &caller2)
    {
        caller = caller2 = 0;
        CONTEXT c = *ctx;
        if (!unwind_one(c))
            return;
        caller = own_rva(c.Rip);
        if (caller && unwind_one(c))
            caller2 = own_rva(c.Rip);
    }

    LONG CALLBACK count_fault(EXCEPTION_POINTERS *info)
    {
        const uintptr_t at = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        if (at >= g_self_lo && at < g_self_hi)
        {
            g_faults.fetch_add(1, std::memory_order_relaxed);
            const uint32_t rva = static_cast<uint32_t>(at - g_self_lo);
            uint32_t caller = 0, caller2 = 0;
            if (info->ContextRecord)
                caller_rvas(info->ContextRecord, caller, caller2);
            for (auto &s : g_sites)
            {
                const uint32_t have = s.rva.load(std::memory_order_relaxed);
                if (have == rva && s.caller.load(std::memory_order_relaxed) == caller &&
                    s.caller2.load(std::memory_order_relaxed) == caller2)
                {
                    s.hits.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (have == 0)
                {
                    uint32_t expect = 0;
                    if (s.rva.compare_exchange_strong(expect, rva, std::memory_order_relaxed))
                    {
                        s.caller.store(caller, std::memory_order_relaxed);
                        s.caller2.store(caller2, std::memory_order_relaxed);
                        s.hits.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;   // count only; never handle, never alter the chain
    }
}

void goblin::crashdiag::arm_fault_counter()
{
    if (g_self_lo)
        return;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&count_fault), &self) || !self)
        return;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(self);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        reinterpret_cast<const uint8_t *>(self) + dos->e_lfanew);
    g_self_lo = reinterpret_cast<uintptr_t>(self);
    g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, count_fault);   // first, so nothing can hide a fault from us
}

std::string goblin::crashdiag::fault_report()
{
    const uint64_t n = g_faults.exchange(0, std::memory_order_relaxed);
    if (n == 0)
        return {};
    std::string where;
    for (auto &s : g_sites)
    {
        const uint32_t rva = s.rva.load(std::memory_order_relaxed);
        const uint32_t caller = s.caller.load(std::memory_order_relaxed);
        const uint32_t caller2 = s.caller2.load(std::memory_order_relaxed);
        const uint32_t hits = s.hits.exchange(0, std::memory_order_relaxed);
        if (rva && hits)
        {
            char b[64];
            if (caller && caller2)
                _snprintf_s(b, sizeof b, _TRUNCATE, "mod+0x%X<-mod+0x%X<-mod+0x%X x%u", rva, caller,
                            caller2, hits);
            else if (caller)
                _snprintf_s(b, sizeof b, _TRUNCATE, "mod+0x%X<-mod+0x%X x%u", rva, caller, hits);
            else
                _snprintf_s(b, sizeof b, _TRUNCATE, "mod+0x%X<-(engine) x%u", rva, hits);
            where += (where.empty() ? "" : ", ") + std::string(b);
        }
    }
    return "first-chance faults raised by our own reads: " + std::to_string(n) +
           (where.empty() ? std::string() : " (" + where + ")");
}
