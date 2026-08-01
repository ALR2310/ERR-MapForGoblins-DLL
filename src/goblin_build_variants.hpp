#pragma once

// Build variants: behaviour that is chosen ONCE, at compile time, because carrying two mechanisms in
// one binary is what makes a defect hard to attribute (see the black-tile hunt: nine in-game runs,
// largely because a single ini key silently armed a whole second rendering path).
//
// Each variant here has a shipping value and an alternative that is kept COMPILING but out of the
// default binary, so switching back is a rebuild rather than an archaeology exercise. Nothing here is
// an ini key on purpose: an ini key means both paths ship, which is the thing we are avoiding.
//
// To flip one, define it on the compiler command line (or edit the default below) and rebuild:
//     cmake -DMFG_LEGACY_PIN_MARKERS=1 ...        (or add /D to the msbuild flags)

// ── Map markers: native display children vs the game's WorldMapPointParam pins ──────────────────
// SHIPPING: native. Our icons are Scaleform children we create and position ourselves; the param row
// still exists (it carries position, icon and text) but its dispMask is zeroed so the game does not
// draw a pin for it, and visibility/hiding is decided by us per frame.
//
// LEGACY (MFG_LEGACY_PIN_MARKERS=1): leave dispMask alone and let the ENGINE draw a pin per row. That
// was the original mechanism: fewer moving parts and no display-list surgery, but every marker costs a
// param row the engine relayouts on open (the slow map-open path), and hiding is limited to what the
// param's flags can express. Keep it building - it is the fallback if the native path ever regresses
// badly on a new game patch, and it is the only path that needs no Scaleform hooks at all.
#ifndef MFG_LEGACY_PIN_MARKERS
#define MFG_LEGACY_PIN_MARKERS 0
#endif

namespace goblin::variants
{
    // true = the engine draws pins from our param rows; false = we draw native children.
    inline constexpr bool kLegacyPinMarkers = MFG_LEGACY_PIN_MARKERS != 0;
    inline constexpr bool kNativeMarkers = !kLegacyPinMarkers;
}

// ── Map-open work: the three former BETA ini keys ────────────────────────────────────────────────
// These shipped as `fast_map_open`, `native_self_detach` and `native_viewport_window`, always true,
// with descriptions inviting the player to turn them off if the map misbehaved. The keys are GONE as
// of 2026-07-27: nobody used them, and the mod should work on install rather than ask the player to
// tune it. A map problem is now our bug to fix from a log or a dump, not something the player is
// expected to bisect - and if one of these needs disabling for triage, that is a rebuild, exactly
// like the marker mechanism above.
//
// Each off-path is small, so they stay as variants rather than being deleted outright:
//   MFG_SLOW_MAP_OPEN=1        do the full relayout on every map open instead of skipping the
//                              redundant one and amortizing the first (the old fast_map_open=false).
//   MFG_NO_SELF_DETACH=1       leave our native children attached when the map dialog is destroyed
//                              instead of bulk-detaching them (the old native_self_detach=false).
//   MFG_ATTACH_ALL_MARKERS=1   attach every marker at once instead of only the near-view window
//                              (the old native_viewport_window=false). Costs map stutter with ~9500
//                              markers, but removes all attach/detach churn while panning.
#ifndef MFG_SLOW_MAP_OPEN
#define MFG_SLOW_MAP_OPEN 0
#endif
#ifndef MFG_NO_SELF_DETACH
#define MFG_NO_SELF_DETACH 0
#endif
#ifndef MFG_ATTACH_ALL_MARKERS
#define MFG_ATTACH_ALL_MARKERS 0
#endif

namespace goblin::variants
{
    inline constexpr bool kFastMapOpen = MFG_SLOW_MAP_OPEN == 0;
    inline constexpr bool kSelfDetach = MFG_NO_SELF_DETACH == 0;
    inline constexpr bool kViewportWindow = MFG_ATTACH_ALL_MARKERS == 0;
}

// ── Stall profiler: OFF in every normal build ────────────────────────────────────────────────────
// The map-open/close stall sampler SUSPENDS the map UI thread and reads its context in a loop for the
// whole measurement window (1.5 s on open, 3 s on close), plus sweeps the other ~100 threads. That is
// the thread whose stalls we are measuring, so with it armed a run does not measure the game - it
// measures the game plus our observer. It also carries the hot-path notes that feed it: a probe on
// every display-list add (behind a spinlock) and two per-attach records during the marker build burst,
// thousands of calls per map open.
//
// It used to ride on `debug_logging`, which meant every diagnostic run silently changed the timings it
// was there to investigate. Now it is a deliberate build: rebuild with -DMFG_STALL_PROFILER=1 when you
// actually want a stall trace, and read the numbers knowing the observer is in them.
#ifndef MFG_STALL_PROFILER
#define MFG_STALL_PROFILER 0
#endif

namespace goblin::variants
{
    inline constexpr bool kStallProfiler = MFG_STALL_PROFILER != 0;
}
