#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include <memory>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <windows.h>

#include "from/params.hpp"
#include "modutils.hpp"

#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "goblin_guarded.hpp"  // "we asked for this fault": what the crash logger must not record
#include "goblin_inject.hpp"
#include "goblin_kindling.hpp"
#include "goblin_logic.hpp"
#include "goblin_markers.hpp"
#include "goblin_messages.hpp"
#include "goblin_overlay.hpp"
#include "goblin_map_timing.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_maphover.hpp"
#include "goblin_stall_probe.hpp"
#include "goblin_worldmap_probe.hpp"

#include "version.h"

static std::thread mod_thread;

// SEH wrapper - catches access violations from refresh() during multiplayer transitions
static int safe_refresh_seh()
{
    __try
    {
        return goblin::collected::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int safe_kindling_refresh_seh()
{
    __try
    {
        return goblin::kindling::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void safe_flag_or_pairs_seh()
{
    __try
    {
        goblin::apply_flag_or_pairs();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void safe_apply_category_visibility_seh()
{
    __try
    {
        goblin::apply_category_visibility();
        goblin::apply_focus_highlight();  // keep focus glow/labels in sync as the collected set changes
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void safe_gfx_tick_seh()
{
    __try
    {
        goblin::gfx_probe::tick(); // charId collision self-heal + (debug_logging) diagnostics
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ── SEH-guarded init-phase wrappers ──
// MSVC's /EHsc disallows __try in functions that contain C++ objects with
// destructors, so each init step goes through a plain C-style adapter +
// a shared invoker. If any step access-violates (e.g. another mod shifted
// the game's memory map mid-init), we log and continue - losing that
// feature is better than the DLL crashing the entire game.

using InitFn = void (*)();

static bool seh_invoke_void(InitFn fn)
{
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void init_modutils()         { modutils::initialize(); }
static void init_from_params()      { from::params::initialize(); }
static void init_collected()        { goblin::collected::initialize(); }
static void init_kindling()         { goblin::kindling::initialize(); }
static void init_inject_entries()   { goblin::inject_map_entries(); }
static void init_apply_map_logic()  { goblin::apply_map_logic(); goblin::apply_worldmap_fragment_bypass(); }
static void init_tutorial_popup()   { goblin::inject_tutorial_popup_rows(); }
static void init_setup_messages()   { goblin::setup_messages(); }
static void init_live_loot()        { goblin::refresh_loot_from_itemlot(); }
static void init_overlay()          { goblin::overlay::setup(); }
static void init_map_timing()       { goblin::map_timing::setup(); }
static void init_gfx_probe()        { goblin::gfx_probe::setup(); }
static void init_maphover()         { goblin::maphover::setup(); }
static void init_stall_probe()      { goblin::stall_probe::setup(); }
static void init_worldmap_probe()   { goblin::worldmap_probe::setup(); }

static void safe_init_step(InitFn fn, const char *name)
{
    if (!seh_invoke_void(fn))
        spdlog::error("SEH exception in init step '{}' - feature may be degraded", name);
}

// ── crash breadcrumbs ────────────────────────────────────────────────────────────────
// Under Proton there is no Windows minidump, so a crash report from a Linux player carries no
// fault address at all - the DLC map crash we are chasing arrived as "the log just stops". This
// records the fault ourselves: a vectored handler appends code, module+RVA and a short backtrace to
// its own file.
// HEAP-FREE on purpose - a stack buffer and WriteFile, no CRT, no spdlog. One of the codes we want
// to catch is heap corruption (0xC0000374), where any allocation could deadlock or fault again.
// It only RECORDS and returns CONTINUE_SEARCH: the process still crashes exactly as it would have,
// and nothing about behaviour changes.
static HANDLE g_crash_file = INVALID_HANDLE_VALUE;
static uintptr_t g_self_base = 0, g_self_size = 0;

// 64-bit hex, written by hand. wsprintfA does NOT support %llX (its format set is a small subset of
// printf's), so the first version of this logger recorded every address as the literal text "0xlX".
// Doing it manually keeps the handler heap-free, which is the whole point of this path.
static int crash_hex64(char *buf, unsigned long long v)
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

// addr -> "module+0xRVA", or bare hex when no module owns it. No heap use.
static int crash_fmt_addr(char *buf, uintptr_t addr)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) &&
        mod)
    {
        char full[MAX_PATH];
        DWORD n = GetModuleFileNameA(mod, full, MAX_PATH);
        const char *base = full;
        for (DWORD i = 0; i < n; ++i)
            if (full[i] == '\\' || full[i] == '/')
                base = full + i + 1;
        int len = wsprintfA(buf, "%s+", base);
        len += crash_hex64(buf + len, addr - reinterpret_cast<uintptr_t>(mod));
        return len;
    }
    return crash_hex64(buf, addr);
}

// ── write watch (HUD investigation) - DEV BUILDS ONLY ────────────────────────────────────────────
// Compiled out of shipping builds since 2026-08-01. What it does - OpenThread, SuspendThread,
// GetThreadContext(CONTEXT_DEBUG_REGISTERS), write Dr0/Dr7, SetThreadContext, then a vectored
// handler reading Dr6 - is a textbook anti-debug/rootkit shape sitting in .text, and VirusTotal's
// behaviour tab tags the DLL `detect-debug-environment` (report 13). It answered its question (who
// clears the menu-state bytes) long ago and has ONE caller. Imports do not change either way -
// MinHook already pulls Suspend/Get/SetThreadContext - so this is about the code pattern, not the
// import table. Rebuild with -DMFG_STALL_PROFILER=1 to get it back for an investigation.
#if MFG_STALL_PROFILER
// Static searching could not name the code that clears the menu-state bytes: the offsets are generic and
// there are a thousand candidates. So catch the writer in the act - a hardware data breakpoint on the one
// byte we know the game clears, and log the instruction that trips it. Debug only, armed on request from
// goblin_stall_probe, one address at a time.
namespace goblin::watch
{
    std::atomic<uintptr_t> g_addr{0};
    // A g_hits counter was incremented in the handler and read by nobody; the watch is one-shot
    // (g_addr is exchanged to 0 on the first hit), so it could only ever have held 0 or 1.

    // A request is QUEUED by the game thread and carried out by another thread. Setting debug registers
    // means SuspendThread + Get/SetThreadContext, and doing that to your OWN thread suspends you with
    // nobody left to resume it - which is exactly how the first version hung the game the instant our
    // screen closed, before any write could even happen. So arm() must never run on the target thread.
    std::atomic<uintptr_t> g_pending_addr{0};
    std::atomic<unsigned long> g_pending_tid{0};

    void request(uintptr_t address, unsigned long thread_id)
    {
        g_pending_tid.store(thread_id, std::memory_order_relaxed);
        g_pending_addr.store(address, std::memory_order_release);
    }

    bool arm(uintptr_t address, DWORD thread_id)
    {
        if (thread_id == GetCurrentThreadId())
        {
            spdlog::warn("[watch] refusing to arm on the calling thread - queue it instead");
            return false;
        }
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                               thread_id);
        if (!th)
            return false;
        bool ok = false;
        if (SuspendThread(th) != (DWORD)-1)
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx))
            {
                ctx.Dr0 = address;
                // DR7: L0 (bit 0) enables Dr0; bits 16-17 = 01 (write), bits 18-19 = 00 (1 byte)
                ctx.Dr7 = (ctx.Dr7 & ~0xFULL) | 0x1ULL;
                ctx.Dr7 = (ctx.Dr7 & ~(0xFULL << 16)) | (0x1ULL << 16);
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                ok = SetThreadContext(th, &ctx) != 0;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        if (ok)
        {
            g_addr.store(address, std::memory_order_release);
            spdlog::info("[watch] armed a write watch on 0x{:X} (thread {})", address, thread_id);
        }
        else
            spdlog::warn("[watch] could not arm the write watch on 0x{:X}", address);
        return ok;
    }
}

namespace goblin::watch
{
    // Called from OUR background thread; performs any queued arming.
    void pump()
    {
        const uintptr_t addr = g_pending_addr.load(std::memory_order_acquire);
        if (!addr)
            return;
        const unsigned long tid = g_pending_tid.load(std::memory_order_relaxed);
        g_pending_addr.store(0, std::memory_order_release);
        arm(addr, tid);
    }
}
#else
// Shipping: the callers are compiled out too (goblin_gfx_probe's tick and one site in
// goblin_stall_probe), so nothing here needs a stub.
#endif // MFG_STALL_PROFILER

// One record: a header line with the label, the code and the faulting address, then the stack.
static void crash_write_record(const char *label, DWORD code, uintptr_t fault)
{
    if (g_crash_file == INVALID_HANDLE_VALUE)
        return;
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int len = wsprintfA(line, "\n[%04d-%02d-%02d %02d:%02d:%02d] [%s] code=0x%08X fault=", st.wYear,
                        st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, label,
                        static_cast<unsigned>(code));
    len += crash_fmt_addr(line + len, fault);
    line[len++] = '\n';
    DWORD wr = 0;
    WriteFile(g_crash_file, line, static_cast<DWORD>(len), &wr, nullptr);
    void *frames[20];
    const USHORT n = RtlCaptureStackBackTrace(0, 20, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        len = wsprintfA(line, "  #%02d ", i);
        len += crash_fmt_addr(line + len, reinterpret_cast<uintptr_t>(frames[i]));
        line[len++] = '\n';
        WriteFile(g_crash_file, line, static_cast<DWORD>(len), &wr, nullptr);
    }
    FlushFileBuffers(g_crash_file);
}

static LONG NTAPI crash_veh(PEXCEPTION_POINTERS ep)
{
    // The watch fires as a single-step with the DR6 hit bit set. Log who wrote, then carry on.
    // Only this BRANCH is dev-gated - the crash logging below it is what players send us and must
    // stay in every build. Reading Dr6 here is half of the debug-register pattern the shipping
    // binary no longer carries.
#if MFG_STALL_PROFILER
    if (ep->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP &&
        goblin::watch::g_addr.load(std::memory_order_acquire) &&
        (ep->ContextRecord->Dr6 & 0xF) != 0)
    {
        // ONE SHOT. The first version stayed armed and the byte turns out to be written from a hot path,
        // so every write raised an exception and the thread drowned in them - the game hung. Disarm inside
        // the handler (this context belongs to the trapping thread, so clearing DR here takes effect on
        // continue), log once, and let everything run at full speed again.
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t rip = ep->ContextRecord->Rip;
        const uintptr_t watched = goblin::watch::g_addr.exchange(0, std::memory_order_acq_rel);
        ep->ContextRecord->Dr0 = 0;
        ep->ContextRecord->Dr7 = 0;
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        if (watched)
        {
            spdlog::info("[watch] 0x{:X} written by exe+0x{:X} (rip 0x{:X}) - watch disarmed",
                         watched, rip > exe ? rip - exe : rip, rip);
            // The writing instruction alone is not enough: the menu-state bytes are written by ONE
            // generic writer, so what we actually need is who asked for it. Walk the stack
            // conservatively and print every value that looks like a return address into the exe.
            // No unwind info is consulted, so some of these are stale slots rather than real frames -
            // that is fine, it is a lead list to check in the disassembler, not a call stack.
            __try
            {
                const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(ep->ContextRecord->Rsp);
                const uintptr_t lo = exe;
                const uintptr_t hi = exe + 0x6000000; // past the last .text of eldenring.exe
                int printed = 0;
                for (int i = 0; i < 128 && printed < 10; ++i)
                {
                    const uintptr_t v = sp[i];
                    if (v <= lo || v >= hi)
                        continue;
                    // A return address is preceded by a call, so require the previous bytes to look
                    // like one: E8 rel32 (5 bytes) or FF /2 indirect (2-7 bytes). Cheap filter, kills
                    // most of the noise.
                    const uint8_t *p = reinterpret_cast<const uint8_t *>(v);
                    const bool after_call = (p[-5] == 0xE8) || (p[-2] == 0xFF) || (p[-3] == 0xFF) ||
                                            (p[-6] == 0xFF) || (p[-7] == 0xFF);
                    if (!after_call)
                        continue;
                    spdlog::info("[watch]   caller candidate exe+0x{:X} (stack +0x{:X})", v - exe,
                                 i * 8);
                    ++printed;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif // MFG_STALL_PROFILER
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) // genuinely fatal codes only
    {
    case 0xC0000005: // access violation
    case 0xC0000374: // heap corruption
    case 0xC0000409: // fast fail / stack buffer
    case 0xC000001D: // illegal instruction
    case 0xC0000096: // privileged instruction
    case 0xC00000FD: // stack overflow
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t fault = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    // An access violation faulting INSIDE this DLL is almost always one of our own guarded read
    // probes (v3_read64 and friends), which its own __except handles - recording those would bury
    // the real thing. Heap corruption is recorded wherever it lands, because our probes never
    // raise it. Latch after a handful so a fault loop cannot fill the disk.
    if (code == 0xC0000005 && fault >= g_self_base && fault < g_self_base + g_self_size)
        return EXCEPTION_CONTINUE_SEARCH;
    // Same thing one step out: a fault inside an ENGINE function we called on purpose, from a frame
    // that handles it. The faulting address is the engine's, so the test above cannot see it - the
    // caller says so instead (goblin_guarded.hpp). Twelve such records in one ERR session, with the
    // game alive throughout, are what this removes.
    if (code == 0xC0000005 && goblin::guarded::inside())
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) > 12)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_crash_file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    crash_write_record("EXCEPTION", code, fault);
    return EXCEPTION_CONTINUE_SEARCH;
}

// [EXCEPTION] vs [CRASH]. A vectored handler runs on EVERY fault, long before anything decides whether
// it is fatal, so it cannot honestly call one a crash - and while it did, a session with a live game
// carried a dozen "CRASH" lines and the one real crash looked no different. So the vectored handler
// says what it knows ("this fault happened"), and the record below is written only from the
// unhandled-exception filter, i.e. when nothing handled it and the process IS going down.
//
// Both are kept because either can be missing: a game that installs its own filter after ours shadows
// the second, and then the first-chance line is all there is.
static LONG WINAPI crash_ueh(PEXCEPTION_POINTERS ep)
{
    crash_write_record("CRASH", ep->ExceptionRecord->ExceptionCode,
                       reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress));
    // Hand it on: whatever wrote the process dumps before still does.
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_crash_logger(HINSTANCE dll_instance, std::filesystem::path log_file)
{
    g_self_base = reinterpret_cast<uintptr_t>(dll_instance);
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(dll_instance);
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uint8_t *>(dll_instance) +
                                                  dos->e_lfanew);
    g_self_size = nt->OptionalHeader.SizeOfImage;

    g_crash_file = CreateFileW(log_file.wstring().c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    AddVectoredExceptionHandler(1, crash_veh);
    SetUnhandledExceptionFilter(crash_ueh);
}

static void setup_logger(std::filesystem::path log_file)
{
    auto logger = std::make_shared<spdlog::logger>("mapforgoblins");
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] %^[%l]%$ %v");
    logger->sinks().push_back(
        std::make_shared<spdlog::sinks::daily_file_sink_st>(log_file.string(), 0, 0, false, 5));
    logger->flush_on(spdlog::level::info);

#if _DEBUG
    AllocConsole();
    FILE *stream;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    logger->sinks().push_back(std::make_shared<spdlog::sinks::stdout_color_sink_st>());
    logger->set_level(spdlog::level::trace);
#endif

    spdlog::set_default_logger(logger);
}

static std::filesystem::path g_mod_folder;

// Manual per-marker hide: on hide_marker_key, hide/unhide the marker under the map
// cursor (goblin::maphover::hovered_row() -> our WorldMapPointParam row). Applies live
// and persists. Runs on its own thread (like the other hotkeys).
static void manual_hide_hotkey_loop()
{
    bool prev = false;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!goblin::config::enableManualHide) { prev = false; continue; }
        bool down = goblin::overlay::key_down(static_cast<int>(goblin::config::hideMarkerKey)) ||
                    goblin::overlay::gamepad_mask_down(goblin::config::hideMarkerGamepad);
        if (down && !prev)
        {
            void *row = goblin::maphover::hovered_row();
            if (!row)
                // V3 native markers have no engine pin; use the overlay's
                // reticle-distance hover so hide-under-cursor keeps working.
                row = goblin::overlay::native_hover_row();
            if (row)
            {
                goblin::ManualHideResult res = goblin::toggle_hovered_marker(row);
                if (res.matched)
                {
                    goblin::reapply_live_settings();
                    // If that was the last shown marker of the focused category, drop focus.
                    if (goblin::prune_focus_if_empty())
                        goblin::reapply_live_settings();
                    goblin::persist_manual_hidden();
                    spdlog::info("[hide] marker textId={} -> {} ({} hidden total)",
                                 res.textId, res.now_hidden ? "HIDDEN" : "shown",
                                 goblin::manual_hidden_count());
                }
            }
        }
        prev = down;
    }
}

static void setup_mod()
{
    safe_init_step(&init_modutils,    "modutils::initialize");

    // Arm + ENABLE the icon-injection hooks FIRST - before the params wait and before any other work. They
    // are passive (they fire when the worldmap movie loads its DefineSprite-171) and depend only on the
    // loaded exe, not on the game's params/world. The worldmap movie CANNOT be re-injected after it loads
    // (its bitmap-register load-context is transient), so these hooks must be live before the player first
    // opens the world map. Arming them at the earliest possible moment - not behind from::params or any
    // delay - makes icon injection robust to WHEN the DLL itself gets injected, as long as that is before
    // the first map open (always true for a process-start loader).
    safe_init_step(&init_gfx_probe, "gfx_probe::setup");
    try { modutils::enable_hooks(); }  // apply just the gfx_probe hooks queued so far (MH_ApplyQueued)
    catch (const std::exception &e) { spdlog::error("handler setup (gfx) FAILED: {}", e.what()); }

    // Blocks (polls internally) until the game's param tables are fully loaded. THIS is the real
    // "wait for game init" - no fixed startup sleep is used (a sleep would also push the hook-arming
    // above past the worldmap movie load on fast Proton boots, which breaks icons).
    safe_init_step(&init_from_params, "from::params::initialize");

    // Manual hides are PER CHARACTER: the set lives in MapForGoblins_hidden_s<slot>.txt.
    // We only register the folder here; the watcher loop loads the active character's file
    // (and reloads on a character switch) via goblin::sync_hidden_slot(), because the save
    // slot is unknown until a character is loaded (the first map open is always after that).
    if (goblin::config::enableManualHide)
        goblin::set_hidden_dir(g_mod_folder);

    safe_init_step(&init_collected,       "collected::initialize");
    safe_init_step(&init_kindling,        "kindling::initialize");
    safe_init_step(&init_inject_entries,  "add_map_entries");
    safe_init_step(&init_apply_map_logic, "apply_map_logic");
    // setup_messages MUST precede inject_tutorial_popup_rows: it allocates the
    // dynamic codex-toast FMG ids (goblin::g_toast_fmg_id) that the popup rows
    // point their textId at. (It also builds the PlaceName textId remap used by
    // the marker rows injected above.)
    safe_init_step(&init_setup_messages,  "setup_messages");
    safe_init_step(&init_tutorial_popup,  "add_tutorial_rows");
    safe_init_step(&init_live_loot,       "refresh_loot_from_itemlot");
    safe_init_step(&init_overlay,         "overlay::setup");
    safe_init_step(&init_map_timing,      "map_timing::setup");
    safe_init_step(&init_maphover,        "maphover::setup");  // marker hover detection (hide/overlay)
    safe_init_step(&init_stall_probe,     "stall_probe::setup");  // debug-only map stall cost counters
    safe_init_step(&init_worldmap_probe,  "worldmap_probe::setup");  // fold non-overworld markers for overlay rings

    try
    {
        modutils::enable_hooks();  // apply the remaining hooks
    }
    catch (const std::exception &e)
    {
        spdlog::error("handler setup FAILED: {}", e.what());
    }

    spdlog::info("Initialization complete");

    if (goblin::config::enableMarkerDump)
    {
        goblin::markers::set_output_path(g_mod_folder / "logs" / "MapForGoblins_markers.log");
        std::thread(goblin::markers::hotkey_loop).detach();
        spdlog::info("Marker dump hotkey: VK 0x{:X}", goblin::config::markerDumpKey);
    }

    if (goblin::config::enableToggleHotkey)
    {
        std::thread(goblin::toggle_hotkey_loop).detach();
        spdlog::info("Icon toggle hotkey: VK 0x{:X}", goblin::config::toggleInjectionKey);
    }

    if (goblin::config::enableManualHide)
    {
        std::thread(manual_hide_hotkey_loop).detach();
        spdlog::info("Manual marker-hide hotkey: VK 0x{:X}", goblin::config::hideMarkerKey);
    }

    // The watcher is the single owner of the WorldMapPointParam state - it
    // applies the master-off flag (set by the toggle hotkey OR the overlay's
    // "Show map icons" checkbox). Run it whenever EITHER path can set that flag,
    // so the overlay's master switch works even if the toggle hotkey is disabled.
    if (goblin::config::enableToggleHotkey || goblin::config::menuEnabled)
    {
        std::thread(goblin::menu_auto_toggle_loop).detach();
        spdlog::info("Icon-state watcher started (icons EXPANDED always; master show/hide via hotkey or overlay)");
    }

    bool first_read = true;
    int prev_collected = -1, prev_kindling = -1;
    auto start = std::chrono::steady_clock::now();
    while (true)
    {
        // Fast polling (100ms) for first 30 seconds to catch NonActive GEOF data
        // before it transitions to WGM. Then slow down to 2 seconds.
        auto elapsed = std::chrono::steady_clock::now() - start;
        bool fast_phase = elapsed < std::chrono::seconds(30);
        std::this_thread::sleep_for(fast_phase ? std::chrono::milliseconds(100) : std::chrono::seconds(2));

        try
        {
            int newly = safe_refresh_seh();
            if (first_read && newly > 0)
            {
                spdlog::info("Initial state: {} pieces hidden",
                             goblin::collected::collected_count());
                first_read = false;
            }
        }
        catch (...)
        {
        }

        try
        {
            safe_kindling_refresh_seh();
        }
        catch (...)
        {
        }

        try
        {
            safe_gfx_tick_seh(); // icon collision self-heal + diagnostics (overlay-independent)
        }
        catch (...)
        {
        }

        try
        {
            safe_flag_or_pairs_seh();
        }
        catch (...)
        {
        }

        // When the collected set changes, refresh the live visibility gate so
        // collected pieces/nodes/kindling hide (and revealed ones reappear) on
        // the OPEN map without a reopen. Category-toggle changes come in via the
        // overlay (reapply_live_settings); this covers in-world collection.
        int cc = goblin::collected::collected_count();
        int kc = goblin::kindling::collected_count();
        if (cc != prev_collected || kc != prev_kindling)
        {
            prev_collected = cc;
            prev_kindling = kc;
            safe_apply_category_visibility_seh();
        }

        // Auto-clear a category focus once its last shown marker is gone (in-world pickup,
        // flag, GEOF, etc.) so a stale "showing only ..." highlight doesn't stick around.
        if (goblin::prune_focus_if_empty())
            safe_apply_category_visibility_seh();

        // Per-character manual hides: on a save-slot (character) switch, load that
        // character's hidden set and reapply visibility.
        if (goblin::config::enableManualHide)
        {
            try
            {
                if (goblin::sync_hidden_slot())
                    safe_apply_category_visibility_seh();
            }
            catch (...)
            {
            }
        }
    }
}

bool WINAPI DllMain(HINSTANCE dll_instance, unsigned int fdw_reason, void *lpv_reserved)
{
    if (fdw_reason == DLL_PROCESS_ATTACH)
    {
        wchar_t dll_filename[MAX_PATH] = {0};
        GetModuleFileNameW(dll_instance, dll_filename, MAX_PATH);
        auto folder = std::filesystem::path(dll_filename).parent_path();
        g_mod_folder = folder;

        setup_logger(folder / "logs" / "MapForGoblins.log");
        install_crash_logger(dll_instance, folder / "logs" / "MapForGoblins_crash.log");

        spdlog::info("Map For Goblins DLL v{} [{}] ({})", PROJECT_VERSION, BUILD_NAME, GIT_HASH);
        goblin::load_config(folder / "MapForGoblins.ini");

        if (goblin::config::debugLogging)
            spdlog::default_logger()->set_level(spdlog::level::debug);

        mod_thread = std::thread([]()
                                 {
            try
            {
                setup_mod();
            }
            catch (std::runtime_error const &e)
            {
                spdlog::error("mod init failed: {}", e.what());
                modutils::deinitialize();
                spdlog::shutdown();
            } });
    }
    else if (fdw_reason == DLL_PROCESS_DETACH && lpv_reserved != nullptr)
    {
        try
        {
            mod_thread.join();
            modutils::deinitialize();
        }
        catch (std::runtime_error const &e)
        {
            spdlog::error("teardown failed: {}", e.what());
        }
        spdlog::shutdown();
    }
    return true;
}
