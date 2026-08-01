#pragma once
#include <string>
#include <cstdint>

// Lightweight map-icon status registry: each load step reports its outcome here and report()
// renders a human-readable summary a player could screenshot into a bug report.
//
// NOT COMPILED as of 2026-07-31 (goblin_diag.cpp is commented out of CMakeLists.txt). The idea did
// not survive contact with how bug reports actually arrive: nobody ever sent a screenshot of it -
// players send the LOG file. Its reader, the overlay Debug tab readout, went on 2026-07-29, and
// fourteen of the sixteen setters only repeated a spdlog line standing right next to them. The two
// that carried something unique - the logo plaque re-point outcome and the live worldmap sprite
// pointer (which reveals the game's heap region, the thing that identified the looks_heap bound bug
// in 2026-06) - were moved into the log before the module left, so nothing was lost.
//
// To build it back in, restore the two lines in CMakeLists.txt, re-add the setter calls and give
// report() a reader; the native menu's Debug page already has a copy-to-clipboard row to model on.
// All setters are thread-safe: load steps run on the game's loader thread, the
// report is read from the Present/overlay thread.
namespace goblin::diag
{
    enum class OverlayState { OffByConfig, Pending, Active, Failed };

    // Load-time gfx hooks armed (ctor + registrar + lossless + sprite-loader).
    // reason = MinHook/AOB failure text when ok is false.
    void set_hooks(bool ok, const char *reason);

    // Worldmap sprite-171 injection result. base = runtime injected charId base,
    // placed/total = icon frames appended, reason = failure note (empty on ok).
    void set_sprite171(bool ok, uint32_t base, int placed, int total, const char *reason);

    // Icon bitmap (lossless tag) registration count.
    void set_bitmaps(int registered, int total);

    // MapForGoblins logo plaque re-point (cosmetic).
    void set_logo(bool ok, const char *reason);

    // Marker iconId remap (markers pointed at our added frames).
    void set_remap(int count);

    // A live game-heap pointer sample (the located worldmap sprite). Lets a copied
    // report reveal the heap region (e.g. 0x7FF2.. under ME2) for diagnosing
    // pointer-range issues like the 2026-06 looks_heap bound bug.
    void set_heap_sample(uint64_t ptr);

    // DX12 overlay backend state.
    void set_overlay(OverlayState state, const char *reason);

    // Note a self-heal re-base event (a charId collision corrected at runtime).
    void note_selfheal(uint32_t newbase);

    // Build the multi-line status report for the overlay / clipboard.
    std::string report();
}
