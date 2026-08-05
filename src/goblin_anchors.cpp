#include "goblin_anchors.hpp"

// NOMINMAX before windows.h: the resolver uses std::min/std::max, and the Windows headers
// define min/max as MACROS, which turns `std::min(a, b)` into a syntax error.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <mutex>

#include <spdlog/spdlog.h>

#include "generated_shared/goblin_anchor_table.hpp"
#include "goblin_anchor_resolve.hpp"

// This file is deliberately thin: finding .text, calling the resolver ONCE, logging what
// happened, and answering at()/all_ok()/memfile_vtable(). The ALGORITHM lives in
// goblin_anchor_resolve.hpp, which has no Windows and no logging dependency precisely so
// that scratch/anchor_resolver_test.cpp can run the SHIPPING code against mapped copies of
// real exe builds. An earlier draft of this file carried its own second copy of the
// algorithm - so the binary would have shipped logic the test never touched.
namespace
{
    using goblin::anchor_resolve::Image;
    using goblin::anchor_table::kCount;
    using goblin::anchor_table::kEntries;

    Image g_image{};
    uint32_t g_resolved[kCount] = {};
    bool g_all_ok = false;
    uintptr_t g_memfile_vt = 0;
    std::once_flag g_once;

    bool find_text_section()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base)
            return false;
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        // eldenring.exe has MORE THAN ONE .text section. The anchored helpers all live in the
        // first (lowest-VA) one, which is also what tools/check_aobs.py scans. Taking the last
        // match instead pointed the offline test at a section ~0x4C0E000 up where nothing
        // matched, and every anchor then read as dead ON THE SUPPORTED EXE - so this is
        // explicit rather than a first-match-wins accident.
        g_image = Image{base, 0, 0};
        const auto *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            if (std::memcmp(sec->Name, ".text", 6) == 0 &&
                (g_image.text_va == 0 || base + sec->VirtualAddress < g_image.text_va))
            {
                g_image.text_va = base + sec->VirtualAddress;
                g_image.text_len = sec->Misc.VirtualSize;
            }
        return g_image.text_va != 0 && g_image.text_len > 0;
    }

    // The scan itself (last `lea rax,[rip+..]` before the ctor's first REAL call - see the
    // 0xE8-operand story at derive_vtable_rva) lives in goblin_anchor_resolve.hpp so the
    // offline test exercises the SHIPPING code: the first version of this function carried
    // its own private copy of the scan, and that untested copy is exactly where the bug
    // lived. What stays here is the live-module part: reading slot 0 through the RELOCATED
    // pointer and requiring it to point back into .text, or it is not a vtable and we keep 0.
    void derive_memfile_vtable()
    {
        uint32_t ctor = 0;
        for (size_t i = 0; i < kCount; ++i)
            if (std::strcmp(kEntries[i].name, "memory_file_ctor") == 0)
                ctor = g_resolved[i];
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_image.base);
        const auto *nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64 *>(g_image.base + dos->e_lfanew);
        const uint32_t vt_rva = goblin::anchor_resolve::derive_vtable_rva(
            g_image, ctor, nt->OptionalHeader.SizeOfImage);
        if (!vt_rva)
            return;
        uintptr_t slot0 = 0;
        std::memcpy(&slot0, reinterpret_cast<const void *>(g_image.base + vt_rva),
                    sizeof(slot0));
        if (slot0 < g_image.text_va || slot0 >= g_image.text_va + g_image.text_len)
            return;
        g_memfile_vt = g_image.base + vt_rva;
    }

    void resolve_once()
    {
        if (!find_text_section())
        {
            spdlog::error("[anchors] no .text section in the host module - every anchored "
                          "helper is off this session");
            return;
        }
        const auto st = goblin::anchor_resolve::resolve_into(g_image, kEntries, kCount,
                                                            g_resolved);
        g_all_ok = st.dead == 0;
        derive_memfile_vtable();
        // Say it when the derivation comes up empty: it returning 0 is a legal answer every
        // caller survives, which is exactly why the one time it was WRONGLY 0 nothing in the
        // log pointed at it - the menu screen just quietly kept the stock movie.
        if (!g_memfile_vt)
            spdlog::warn("[anchors] the memory-file vtable could not be derived from its ctor - "
                         "the menu-movie transform stays off this session");
        if (st.at_baked == kCount)
        {
            spdlog::info("[anchors] all {} helpers verified where this build expects them",
                         kCount);
            return;
        }
        // A different exe build than this DLL was made from. Name what could not be placed:
        // that list is the first thing to want when a player reports a missing feature.
        for (size_t i = 0; i < kCount; ++i)
            if (!g_resolved[i])
                spdlog::warn("[anchors] {} could not be placed on this build (expected at "
                             "0x{:X})",
                             kEntries[i].name, kEntries[i].rva);
        spdlog::info("[anchors] different exe build: {} where expected, {} relocated "
                     "(by {:+#x}..{:+#x}), {} unplaced, {} rejected out of sequence{}",
                     st.at_baked, st.rebased, st.min_shift, st.max_shift, st.dead,
                     st.order_dropped,
                     st.dead ? " - the features that need them stay off" : "");
    }

    void ensure()
    {
        std::call_once(g_once, resolve_once);
    }
}

namespace goblin::anchors
{
    uintptr_t at(uint32_t baked_rva)
    {
        ensure();
        for (size_t i = 0; i < kCount; ++i)
            if (kEntries[i].rva == baked_rva)
                return g_resolved[i] ? g_image.base + g_resolved[i] : 0;
        // A code address with no table entry is a bug in OUR source, not a game difference:
        // it would be called unrelocated on every foreign build. Fail closed and say so.
        spdlog::warn("[anchors] 0x{:X} is not in the table - add it to tools/rva_anchors.py",
                     baked_rva);
        return 0;
    }

    bool all_ok()
    {
        ensure();
        return g_all_ok;
    }

    uintptr_t memfile_vtable()
    {
        ensure();
        return g_memfile_vt;
    }
}
