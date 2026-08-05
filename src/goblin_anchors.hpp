#pragma once

#include <cstdint>

// Runtime resolution of the RVA anchors (tools/rva_anchors.py, generated into
// generated_shared/goblin_anchor_table.hpp by tools/check_aobs.py).
//
// The native-menu / hover-panel code calls small engine helpers by direct `base + RVA`.
// Those RVAs are measured on the build-target exe; on any other build (a downpatch, a
// future patch) they point into unrelated code, and calling - or MinHook-patching - them
// corrupts the game (the report-20 failure mode). The resolver here makes that safe AND
// portable:
//   R1: every anchor's expected bytes are verified at its baked RVA - on the supported
//       exe this is the whole cost (one memcmp per anchor, no scanning);
//   R2: on a shifted exe, a pattern that is unique within +/-0x4000 of the baked RVA
//       is taken as the anchor's new home;
//   R3: the rest are predicted by the shift of their nearest already-resolved neighbour
//       (function order and local spacing survive game patches) and matched within
//       +/-0x2000 of that prediction;
//   R4: whole-.text scan, nearest hit to the prediction, as the last resort.
// Resolved anchors must keep their baked-RVA ORDER; a violator is demoted to dead.
// Validated offline against 2.6.2 (all R1), 2.6.0, 2.2.3 and 2.2.0 (44/44 each,
// scratch/anchor_rebase_feasibility.py).
namespace goblin::anchors
{
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
