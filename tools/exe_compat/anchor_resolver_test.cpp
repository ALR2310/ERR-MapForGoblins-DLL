// Offline verification of the SHIPPING resolver (src/goblin_anchor_resolve.hpp) against real
// exe builds. Maps each exe's .text at its virtual address inside a reserved image-sized block,
// so `base + rva` addresses resolve exactly as they do in the running game, then runs the same
// resolve_into() the DLL runs at startup.
//
// Build (from MapForGoblins/, inside a VS2022 x64 env):
//   py tools\exe_compat\run_anchor_test.py   (compiles into builds\anchor_test\)
// Run: builds\anchor_test\anchor_test.exe <exe> [<exe> ...]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "generated_shared/goblin_anchor_table.hpp"
#include "goblin_anchor_resolve.hpp"

using goblin::anchor_resolve::Image;
using goblin::anchor_resolve::resolve_into;
using namespace goblin::anchor_table;

struct Mapped
{
    uint8_t *base = nullptr;
    uintptr_t text_va = 0;
    size_t text_len = 0;
    uint32_t image_size = 0;   // SizeOfImage - the derive scan's outer bound
    uint64_t image_base = 0;   // preferred base: the mapped copy is NOT relocated, so every
                               // absolute pointer in it (a vtable slot) is ImageBase + rva
};

static bool map_exe(const char *path, Mapped &out)
{
    FILE *f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file(static_cast<size_t>(sz));
    const size_t got = fread(file.data(), 1, file.size(), f);
    fclose(f);
    if (got != file.size())
        return false;

    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(file.data());
    const auto *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64 *>(file.data() + dos->e_lfanew);
    const uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    out.image_size = image_size;
    out.image_base = nt->OptionalHeader.ImageBase;
    auto *base = static_cast<uint8_t *>(
        VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base)
        return false;
    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        const size_t n = sec->SizeOfRawData;
        if (!n || sec->PointerToRawData + n > file.size())
            continue;
        std::memcpy(base + sec->VirtualAddress, file.data() + sec->PointerToRawData, n);
        // eldenring.exe carries MORE THAN ONE section named .text; the anchored code all lives
        // in the FIRST (lowest-VA) one, so keep that and never let a later one overwrite it.
        // Taking the last was this test's own bug: it pointed the resolver at a 0x4C0E000-based
        // section where nothing matched, and every anchor read as dead on the SUPPORTED exe.
        if (std::memcmp(sec->Name, ".text", 6) == 0 && out.text_va == 0)
        {
            out.text_va = reinterpret_cast<uintptr_t>(base) + sec->VirtualAddress;
            out.text_len = sec->Misc.VirtualSize < n ? sec->Misc.VirtualSize : n;
        }
    }
    out.base = base;
    return out.text_va != 0;
}

int main(int argc, char **argv)
{
    int failures = 0;
    for (int a = 1; a < argc; ++a)
    {
        Mapped m{};
        if (!map_exe(argv[a], m))
        {
            std::printf("\n=== %s\n  COULD NOT MAP\n", argv[a]);
            ++failures;
            continue;
        }
        Image im{reinterpret_cast<uintptr_t>(m.base), m.text_va, m.text_len};
        if (getenv("ANCHOR_DEBUG"))
        {
            std::printf("  DBG base=%p text_va=%p len=0x%zX trva=0x%X kCount=%zu\n",
                        (void *)im.base, (void *)im.text_va, im.text_len, im.text_rva(),
                        kCount);
            const auto &e = kEntries[0];
            std::printf("  DBG %s want:", e.name);
            for (uint16_t i = 0; i < e.len; ++i)
                std::printf(" %02X", e.bytes[i]);
            std::printf("\n  DBG %s  got:", e.name);
            const auto *p = reinterpret_cast<const uint8_t *>(im.base + e.rva);
            for (uint16_t i = 0; i < e.len; ++i)
                std::printf(" %02X", p[i]);
            std::printf("\n  DBG len=%u mask0=%u\n", e.len, e.mask[0]);
        }
        std::vector<uint32_t> res(kCount, 0);
        // Timed: resolve_into now walks the whole of .text once to count matches, and that
        // walk runs at DLL startup. If it ever stops being a few tens of milliseconds this
        // is where it shows up.
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const auto st = resolve_into(im, kEntries, kCount, res.data());
        QueryPerformanceCounter(&t1);
        const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        std::printf("\n=== %s   (resolve took %.1f ms)\n", argv[a], ms);
        for (size_t i = 0; i < kCount; ++i)
        {
            if (!res[i])
            {
                std::printf("  [DEAD ] %-28s 0x%X\n", kEntries[i].name, kEntries[i].rva);
                continue;
            }
            const long long s = (long long)res[i] - (long long)kEntries[i].rva;
            std::printf("  [%-5s] %-28s 0x%X -> 0x%X  shift %+lld\n",
                        s == 0 ? "AT" : "REBASE", kEntries[i].name, kEntries[i].rva, res[i],
                        s);
        }
        std::printf("  ---- at-baked %zu, rebased %zu, dead %zu, order-dropped %zu, "
                    "pins %zu, followers %zu, shifts %+lld..%+lld\n",
                    st.at_baked, st.rebased, st.dead, st.order_dropped, st.pins, st.followers,
                    (long long)st.min_shift, (long long)st.max_shift);
        if (st.dead || st.order_dropped)
            ++failures;

        // The derived memory-file vtable, through the SAME shipping scan the DLL runs
        // (derive_vtable_rva). This is the part whose first version lived only in
        // goblin_anchors.cpp and shipped broken on every exe because no test ran it.
        // The slot-0 sanity is mirrored here through ImageBase, since the mapped copy
        // holds preferred-base pointers where the live module holds relocated ones.
        uint32_t ctor = 0;
        for (size_t i = 0; i < kCount; ++i)
            if (std::strcmp(kEntries[i].name, "memory_file_ctor") == 0)
                ctor = res[i];
        if (ctor)
        {
            const uint32_t vt_rva =
                goblin::anchor_resolve::derive_vtable_rva(im, ctor, m.image_size);
            bool ok = vt_rva != 0;
            uint64_t slot0_rva = 0;
            if (ok)
            {
                uint64_t slot0 = 0;
                std::memcpy(&slot0, m.base + vt_rva, sizeof(slot0));
                slot0_rva = slot0 - m.image_base;
                ok = slot0_rva >= im.text_rva() &&
                     slot0_rva < uint64_t(im.text_rva()) + im.text_len;
            }
            if (ok)
                std::printf("  [OK   ] memfile vtable derived: rva 0x%X (ctor 0x%X, "
                            "slot0 rva 0x%llX in .text)\n",
                            vt_rva, ctor, (unsigned long long)slot0_rva);
            else
            {
                std::printf("  [DEAD ] memfile vtable NOT derived (ctor 0x%X, scan rva 0x%X, "
                            "slot0 rva 0x%llX) - the menu-movie transform would be off\n",
                            ctor, vt_rva, (unsigned long long)slot0_rva);
                ++failures;
            }
        }
        else
            std::printf("  [SKIP ] memory_file_ctor unresolved - vtable derivation untestable\n");
        VirtualFree(m.base, 0, MEM_RELEASE);
    }
    std::printf("\n%s\n", failures ? "FAILURES PRESENT" : "all exes fully resolved");
    return failures ? 1 : 0;
}
