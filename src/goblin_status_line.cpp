#include "goblin_status_line.hpp"

#include "modutils.hpp"
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <windows.h>

namespace
{
    // CSFeManImp singleton slot: the store at the tail of the manager's init (the same signature
    // the native menu's HUD-mode restore resolves - tools/aob_signatures.py "feman_slot").
    constexpr const char *kFeManAob = "48 89 05 ?? ?? ?? ?? 48 8B 8B 80 00 00 00 48 85 C9 74 05 E8";
    // The front-end update's status block (unique): `lea rbx,[rdi+3720]; movsxd rax,[rdi+59BC];
    // mov esi,[rdi+rax*4+59A4]`. rdi = CSFeMan.
    constexpr const char *kStatusAnchorAob =
        "48 8D 9F 20 37 00 00 48 63 87 BC 59 00 00 8B B4 87 A4 59 00 00";
    constexpr uintptr_t kTimerOff = 0x3758;  // f32, counts up; the entry is dropped past 3.0
    constexpr uintptr_t kRingOff = 0x59A4;   // int32[6] message ids, -1 = empty
    constexpr uintptr_t kReadOff = 0x59BC;   // the entry being displayed
    constexpr int32_t kOurId = 0x5EAC4B10;   // never a real status id

    using BuildFn = void *(__fastcall *)(void *out, int id); // MenuString from a status id
    using EmptyFn = void *(__fastcall *)(void *out);         // an empty MenuString
    BuildFn o_build = nullptr;
    EmptyFn g_empty = nullptr;
    void **g_feman_slot = nullptr;
    std::atomic<bool> g_ready{false};

    // The text the builder answers with. Two buffers: the front end reads the current one every
    // frame the message is up, so a new message is written into the other and then made current.
    wchar_t g_text[2][128]{};
    std::atomic<int> g_cur{0};
    std::mutex g_show_mtx;

    void *__fastcall build_detour(void *out, int id)
    {
        if (id == kOurId && g_empty)
        {
            g_empty(out);
            *reinterpret_cast<const wchar_t **>(out) = g_text[g_cur.load(std::memory_order_acquire)];
            return out;
        }
        return o_build(out, id);
    }

    // rel32 target of the E8 at `at`.
    uintptr_t call_target(uintptr_t at)
    {
        int32_t rel = 0;
        memcpy(&rel, reinterpret_cast<const void *>(at + 1), sizeof(rel));
        return at + 5 + static_cast<intptr_t>(rel);
    }
}

void goblin::status_line::setup()
{
    try
    {
        g_feman_slot = reinterpret_cast<void **>(modutils::scan<void *>(
            {.aob = kFeManAob, .relative_offsets = {{3, 7}}}));
        const auto anchor = reinterpret_cast<const uint8_t *>(modutils::scan<void>({.aob = kStatusAnchorAob}));
        if (!g_feman_slot || !anchor)
            throw std::runtime_error("front-end pieces not located");
        // The two calls the block makes, by their byte shapes within the next 0x100 bytes:
        //   builder:  8B D6 48 8D 4C 24 30 E8 rel32   (mov edx,esi; lea rcx,[rsp+30]; call)
        //   empty:    48 8D 4C 24 30 E8 rel32          (the ring-empty branch, after the builder)
        static const uint8_t kB[] = {0x8B, 0xD6, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8};
        static const uint8_t kE[] = {0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8};
        size_t ib = SIZE_MAX, ie = SIZE_MAX;
        for (size_t i = 0; i + sizeof kB <= 0x100; ++i)
            if (memcmp(anchor + i, kB, sizeof kB) == 0) { ib = i + 7; break; }
        if (ib != SIZE_MAX)
            for (size_t i = ib + 5; i + sizeof kE <= 0x100; ++i)
                if (memcmp(anchor + i, kE, sizeof kE) == 0) { ie = i + 5; break; }
        if (ib == SIZE_MAX || ie == SIZE_MAX)
            throw std::runtime_error("status calls not located after the anchor");
        const uintptr_t build_fn = call_target(reinterpret_cast<uintptr_t>(anchor) + ib);
        const uintptr_t empty_fn = call_target(reinterpret_cast<uintptr_t>(anchor) + ie);
        g_empty = reinterpret_cast<EmptyFn>(empty_fn);
        modutils::hook(reinterpret_cast<void *>(build_fn), reinterpret_cast<void *>(&build_detour),
                       reinterpret_cast<void **>(&o_build));
        g_ready.store(true, std::memory_order_release);
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        spdlog::info("[status] line ready (builder exe+0x{:X}, empty exe+0x{:X})", build_fn - base,
                     empty_fn - base);
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[status] line unavailable: {}", e.what());
    }
}

namespace
{
    // POD-only (SEH): put our id into the entry being DISPLAYED, not the write cursor, so whatever
    // is showing is replaced at once, and restart the timer for the full 3 s.
    void push_our_id()
    {
        __try
        {
            const uintptr_t fe = reinterpret_cast<uintptr_t>(*g_feman_slot);
            if (!fe)
                return;
            const int32_t rd = *reinterpret_cast<const int32_t *>(fe + kReadOff);
            if (rd < 0 || rd > 5)
                return;
            *reinterpret_cast<int32_t *>(fe + kRingOff + static_cast<uintptr_t>(rd) * 4) = kOurId;
            *reinterpret_cast<float *>(fe + kTimerOff) = 0.0f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

void goblin::status_line::show(const wchar_t *text)
{
    if (!g_ready.load(std::memory_order_acquire) || !text)
        return;
    std::lock_guard<std::mutex> lk(g_show_mtx);
    const int next = g_cur.load(std::memory_order_relaxed) ^ 1;
    wcsncpy_s(g_text[next], 128, text, _TRUNCATE);
    g_cur.store(next, std::memory_order_release);
    push_our_id();
}
