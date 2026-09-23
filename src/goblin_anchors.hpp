#pragma once

#include <cstddef>
#include <cstdint>

// The anchor each call site wants, by name. The RVA never leaves the table.
#include "generated_shared/goblin_anchor_ids.hpp"

// Runtime resolution of the RVA anchors (tools/rva_anchors.py, generated into
// generated_shared/goblin_anchor_table.hpp by tools/check_aobs.py).
//
// The native-menu / hover-panel code calls small engine helpers by direct `base + RVA`.
// Those RVAs are measured on the build-target exe; on any other build (a downpatch, a
// future patch) they point into unrelated code, and calling - or MinHook-patching - them
// corrupts the game (the report-20 failure mode). The resolver here makes that safe AND
// portable. It counts, in ONE pass over .text, how many places match each anchor's pattern;
// an anchor matching exactly once is identified by its own bytes and accepted wherever it
// is, and anything else may only be placed relative to those. See
// src/goblin_anchor_resolve.hpp for why "the bytes at the baked RVA match" was not enough
// (report 31: it let a 3638-way pattern report itself at home, and the mod called a
// stranger). Verified against 2.6.2 / 2.6.1 / 2.6.0 / 2.2.3 / 2.2.0: 44/44 anchors resolve
// and every one lands on a body that is instruction-for-instruction the anchored function
// (scratch/run_anchor_test.py, scratch/anchor_truth.py).
namespace goblin::anchors
{
    // Resolve the table now. Optional - at()/all_ok() do it on first use - but the pass
    // costs ~50ms, and the call sites are render paths, so init calls this to keep that
    // cost off a frame and to get the "[anchors]" verdict into the log early.
    void warm();

    // Live VA of an anchor, 0 when that anchor is dead on this exe.
    // The first call resolves the whole table (thread-safe, once).
    //
    // Keyed by NAME, never by address. When at() took the baked RVA as its key, re-baking the
    // table onto a new game build changed every key at once: no entry matched, at() answered 0,
    // and every anchored call site called address 0. Nothing in the sources carries an address
    // any more, so that cannot recur.
    uintptr_t at(AnchorId id);

    // Every anchor resolved (possibly rebased). The gate for arming anything that
    // calls or hooks anchored addresses.
    bool all_ok();

    // The Scaleform MemoryFile vtable VA, derived from the resolved memory_file_ctor
    // (the last `lea rax,[rip+..]` before its first call - the derived class's vptr).
    // 0 when the ctor is dead or the derivation fails; own_movie then simply never
    // matches a File and the transform stays off - a safe degradation.
    uintptr_t memfile_vtable();

    // Every class the mod finds by its RTTI name. ONE list, so that a new user of vtable_of
    // cannot be left out of prewarm_vtables(): vtable_of only takes a value of this enum, and
    // a value without its row in kRttiClasses fails the static_assert below. To add a class:
    // a value here (before Count), its decorated name in kRttiClasses at the same position.
    enum class RttiClass : uint8_t
    {
        CSFeAutoHideCtrl,  // stall_probe: the HUD-mode restore and its log (UI thread)
        KeyConfigDialog,   // stall_probe: the settings screen's row builder (UI thread)
        WorldMapDialog,    // stall_probe: the map as the settings screen's host (UI thread)
        CSScaleformSystem, // stall_probe: the parked-movie slot scan (its own thread)
        CSScaleformImp,    // stall_probe: the parked-movie slot scan (its own thread)
        EcTestDistance,    // kindling: spirit liveness (the kindling worker)
        Count
    };

    struct RttiClassName
    {
        RttiClass id;
        const char *decorated; // exactly as the exe spells it
    };

    inline constexpr RttiClassName kRttiClasses[] = {
        {RttiClass::CSFeAutoHideCtrl, ".?AVCSFeAutoHideCtrl@CS@@"},
        {RttiClass::KeyConfigDialog, ".?AVKeyConfigDialog@CS@@"},
        {RttiClass::WorldMapDialog, ".?AVWorldMapDialog@CS@@"},
        {RttiClass::CSScaleformSystem, ".?AVCSScaleformSystem@CS@@"},
        {RttiClass::CSScaleformImp, ".?AVCSScaleformImp@CS@@"},
        {RttiClass::EcTestDistance, ".?AVEcTestDistance@CS@@"},
    };

    constexpr bool rtti_classes_complete()
    {
        if (sizeof(kRttiClasses) / sizeof(kRttiClasses[0]) != static_cast<size_t>(RttiClass::Count))
            return false;
        for (size_t i = 0; i < sizeof(kRttiClasses) / sizeof(kRttiClasses[0]); ++i)
            if (static_cast<size_t>(kRttiClasses[i].id) != i)
                return false;
        return true;
    }
    static_assert(rtti_classes_complete(),
                  "kRttiClasses needs exactly one row per RttiClass value, in enum order");

    // The VA of a class's vtable, found by its RTTI name, 0 when it is not there or the name
    // is ambiguous. After prewarm_vtables() this is a single atomic load and never waits.
    // Before it (or for a class it could not publish) the class is resolved on the calling
    // thread, with no lock held during the walk, and published for every later call.
    //
    // A vtable address cannot be anchored by bytes - it lives in .rdata and holds addresses.
    // Baking one instead pins the check that uses it to a single game build, which is how the
    // map menu lost its host on 1.17. The RTTI name does not move.
    uintptr_t vtable_of(RttiClass cls);

    // Resolve every kRttiClasses entry in one walk (~8 ms on the file; the old walk took
    // 65-90 ms per class) and publish the answers. Called once by init, off the game thread
    // and before anything that calls vtable_of is set up, so no frame ever pays for a lookup:
    // on 2026-09-23 the first open of the settings screen over the map paid for two, 186 ms.
    void prewarm_vtables();

    // A vtable found by the CODE it points at: the one place in .rdata where both anchored
    // functions sit at their own slot index. For classes RTTI cannot name - the Scaleform ones -
    // and the replacement for baking such an address, which is only ever right on the build it
    // was measured on. 0 when it is absent or ambiguous. Cached per (anchor, slot) pair.
    uintptr_t vtable_with(AnchorId a, unsigned slot_a, AnchorId b, unsigned slot_b);
}
