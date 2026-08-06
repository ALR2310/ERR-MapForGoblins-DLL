#pragma once

#include <cstdint>

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

    // Live VA for a baked anchor RVA, 0 when that anchor is dead on this exe.
    // The first call resolves the whole table (thread-safe, once).
    uintptr_t at(uint32_t baked_rva);

    // Every anchor resolved (possibly rebased). The gate for arming anything that
    // calls or hooks anchored addresses.
    bool all_ok();

    // The Scaleform MemoryFile vtable VA, derived from the resolved memory_file_ctor
    // (the last `lea rax,[rip+..]` before its first call - the derived class's vptr).
    // 0 when the ctor is dead or the derivation fails; own_movie then simply never
    // matches a File and the transform stays off - a safe degradation.
    uintptr_t memfile_vtable();
}
