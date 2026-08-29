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
            // Note what carried the verdict, not just the verdict. "All present" used to be
            // printed on the strength of a byte compare at each baked address, which a
            // pattern shared by thousands of functions can pass by accident; {} identified
            // says how many were pinned by a pattern that occurs exactly once.
            spdlog::info("[anchors] all {} helpers verified where this build expects them "
                         "({} identified by their own bytes)",
                         kCount, st.pins);
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
        spdlog::info("[anchors] {} identified by their own bytes, {} placed relative to "
                     "those", st.pins, st.followers);
    }

    void ensure()
    {
        std::call_once(g_once, resolve_once);
    }
}

namespace goblin::anchors
{
    void warm()
    {
        ensure();
    }

    uintptr_t at(AnchorId id)
    {
        ensure();
        const size_t i = static_cast<size_t>(id);
        if (i >= kCount)
            return 0; // unreachable: the enum is generated from the same table
        return g_resolved[i] ? g_image.base + g_resolved[i] : 0;
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

// ---------------------------------------------------------------------------------------
// Finding a class's vtable at runtime.
//
// Three checks in the menu code compare an object's vtable against a .rdata address measured
// on ONE game build (CSFeAutoHideCtrl, KeyConfigDialog, WorldMapDialog). A byte anchor cannot
// follow those: .rdata holds addresses, not code. So they were simply wrong on every other
// build, and the check they guard bowed out - which on 1.17 left the map menu with no host to
// hang its screen on.
//
// MSVC leaves a handle that no patch moves: one qword BEFORE a vtable sits a pointer to a
// _RTTICompleteObjectLocator, and the locator names the class as a string. Walking that
// backwards - name -> TypeDescriptor -> locator -> vtable - finds the class wherever the
// linker put it this time. Verified on 2.6.2 and 2.7.0: all three classes resolve, and the
// answers match what the same vtables were found to be by their slot functions
// (scratch/vtable_by_rtti.py, scratch/find_vtables.py).
namespace
{
    struct VtCache
    {
        const char *name;
        uintptr_t va;
    };
    VtCache g_vt_cache[8] = {};
    std::mutex g_vt_mutex;

    // Every section, so the name string can be found wherever the linker put it (.data on the
    // builds seen here) while the locator and the vtable itself live in .rdata.
    template <typename F>
    void each_section(uintptr_t base, F &&fn)
    {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const auto *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            fn(reinterpret_cast<const uint8_t *>(base + sec->VirtualAddress),
               static_cast<size_t>(sec->Misc.VirtualSize),
               static_cast<uint32_t>(sec->VirtualAddress),
               std::memcmp(sec->Name, ".rdata", 7) == 0);
    }

    // All offsets of `needle` in `hay`, reported through fn.
    template <typename F>
    void scan_for(const uint8_t *hay, size_t len, const void *needle, size_t nlen, F &&fn)
    {
        if (len < nlen)
            return;
        const auto *n0 = static_cast<const uint8_t *>(needle);
        for (size_t i = 0; i + nlen <= len; ++i)
            if (hay[i] == *n0 && std::memcmp(hay + i, n0, nlen) == 0)
                fn(i);
    }

    uintptr_t find_vtable_uncached(uintptr_t base, const char *decorated)
    {
        const size_t nlen = std::strlen(decorated) + 1; // the NUL keeps a prefix from matching

        // 1. the name string -> the TypeDescriptor that owns it (name sits at +0x10)
        uint32_t td_rva = 0;
        unsigned td_hits = 0;
        each_section(base, [&](const uint8_t *p, size_t len, uint32_t va, bool) {
            scan_for(p, len, decorated, nlen, [&](size_t off) {
                if (va + off >= 0x10)
                {
                    td_rva = static_cast<uint32_t>(va + off - 0x10);
                    ++td_hits;
                }
            });
        });
        if (td_hits != 1)
            return 0;

        // 2. every .rdata reference to it, keeping only the ones that really are a locator.
        //    The TypeDescriptor RVA appears TWICE per class: once in the
        //    _RTTICompleteObjectLocator (pTypeDescriptor at +12) and once in the
        //    _RTTIBaseClassDescriptor (pTypeDescriptor at +0). Demanding a single reference
        //    here is what made the first version answer "not found" for all three classes on a
        //    live 2.7.0 while the same walk succeeded offline. The x64 locator identifies
        //    itself: signature 1 at +0, and pSelf at +20 holding its OWN rva.
        // 3. the vtable is one qword after whatever points at that locator.
        uintptr_t vt = 0;
        unsigned vt_hits = 0;
        each_section(base, [&](const uint8_t *p, size_t len, uint32_t va, bool rdata) {
            if (!rdata)
                return;
            scan_for(p, len, &td_rva, sizeof(td_rva), [&](size_t off) {
                if (off < 12 || off - 12 + 24 > len)
                    return; // a locator is 24 bytes; do not read past the section
                const uint32_t col_rva = static_cast<uint32_t>(va + off - 12);
                const auto *col = reinterpret_cast<const uint32_t *>(base + col_rva);
                if (col[0] != 1 || col[5] != col_rva)
                    return; // a base-class descriptor, not the locator
                const uint64_t col_va = static_cast<uint64_t>(base) + col_rva;
                each_section(base, [&](const uint8_t *q, size_t qlen, uint32_t qva, bool qrd) {
                    if (!qrd)
                        return;
                    scan_for(q, qlen, &col_va, sizeof(col_va), [&](size_t qoff) {
                        vt = base + qva + qoff + 8;
                        ++vt_hits;
                    });
                });
            });
        });
        return vt_hits == 1 ? vt : 0;
    }
}

namespace goblin::anchors
{
    uintptr_t vtable_of(const char *decorated_name)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base || !decorated_name)
            return 0;
        std::lock_guard<std::mutex> lock(g_vt_mutex);
        size_t free_slot = SIZE_MAX;
        for (size_t i = 0; i < sizeof(g_vt_cache) / sizeof(g_vt_cache[0]); ++i)
        {
            if (g_vt_cache[i].name == decorated_name)
                return g_vt_cache[i].va;
            if (!g_vt_cache[i].name && free_slot == SIZE_MAX)
                free_slot = i;
        }
        const uintptr_t va = find_vtable_uncached(base, decorated_name);
        if (free_slot != SIZE_MAX)
            g_vt_cache[free_slot] = {decorated_name, va};
        if (va)
            spdlog::info("[anchors] {} -> exe+0x{:X}", decorated_name, va - base);
        else
            spdlog::info("[anchors] {} -> not found", decorated_name);
        return va;
    }
}

namespace
{
    struct VtPairCache
    {
        uint64_t key;
        uintptr_t va;
    };
    VtPairCache g_vt_pair_cache[4] = {};
}

namespace goblin::anchors
{
    uintptr_t vtable_with(AnchorId a, unsigned slot_a, AnchorId b, unsigned slot_b)
    {
        // A vtable identified by the CODE it points at. RTTI answers most classes, but not the
        // Scaleform ones - the snapshot-slot class has no locator - and a baked address is what
        // this replaces: exe+0x2CB9EF0 was right on 2.6.2/2.6.1/2.6.0 and wrong on 2.7.0, 2.2.3
        // and 2.2.0, where it silently switched off the marker generation's release at map close.
        // The functions themselves are anchored, so they are found on any build; the vtable is
        // the one place in .rdata where BOTH sit at their own slot. Two slots, not one: a single
        // function pointer occurs in every vtable of every derived class.
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base)
            return 0;
        const uint64_t key = (static_cast<uint64_t>(a) << 48) ^ (static_cast<uint64_t>(b) << 32) ^
                             (static_cast<uint64_t>(slot_a) << 16) ^ slot_b ^ 0x8000000000000000ull;
        std::lock_guard<std::mutex> lock(g_vt_mutex);
        size_t free_slot = SIZE_MAX;
        for (size_t i = 0; i < sizeof(g_vt_pair_cache) / sizeof(g_vt_pair_cache[0]); ++i)
        {
            if (g_vt_pair_cache[i].key == key)
                return g_vt_pair_cache[i].va;
            if (!g_vt_pair_cache[i].key && free_slot == SIZE_MAX)
                free_slot = i;
        }

        uintptr_t va = 0;
        const uintptr_t fa = at(a), fb = at(b);
        if (fa && fb)
        {
            const uint64_t na = fa, nb = fb;
            unsigned hits = 0;
            each_section(base, [&](const uint8_t *p, size_t len, uint32_t sva, bool rdata) {
                if (!rdata || hits > 1)
                    return;
                scan_for(p, len, &na, sizeof(na), [&](size_t off) {
                    if (off < static_cast<size_t>(slot_a) * 8)
                        return;
                    const size_t start = off - static_cast<size_t>(slot_a) * 8;
                    const size_t want_b = start + static_cast<size_t>(slot_b) * 8;
                    if (want_b + 8 > len)
                        return;
                    uint64_t got = 0;
                    std::memcpy(&got, p + want_b, 8);
                    if (got != nb)
                        return;
                    va = base + sva + start;
                    ++hits;
                });
            });
            if (hits != 1)
                va = 0;
            spdlog::info("[anchors] vtable by slots {}/{}: {} ({} candidate(s))", slot_a, slot_b,
                         va ? "found" : "not found", hits);
            if (va)
                spdlog::info("[anchors]   -> exe+0x{:X}", va - base);
        }
        if (free_slot != SIZE_MAX)
            g_vt_pair_cache[free_slot] = {key, va};
        return va;
    }
}
