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
// ONE known regression in this mode since 2026-09-18: de-overlap. Spreading stacked icons apart is
// done live, on the native children (refresh_deoverlap in goblin_inject.cpp), and there used to be a
// second, generation-time pass that spiralled the baked param positions. The baked one only ever
// duplicated work the live pass redid from scratch, so it was retired - which leaves THIS path with
// coincident markers drawn on top of each other. Cosmetic, and the trade was deliberate.
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
//   MFG_NO_FRAME_PUMP=1        build every marker inside the engine's open burst, as before 2.1.5,
//                              instead of only the visible ones there and the rest over the next
//                              frames through a hidden sprite of our own (goblin_stall_probe.cpp,
//                              [v3pump]). Costs one long frame at every map open.
#ifndef MFG_SLOW_MAP_OPEN
#define MFG_SLOW_MAP_OPEN 0
#endif
#ifndef MFG_NO_SELF_DETACH
#define MFG_NO_SELF_DETACH 0
#endif
#ifndef MFG_ATTACH_ALL_MARKERS
#define MFG_ATTACH_ALL_MARKERS 0
#endif
#ifndef MFG_NO_FRAME_PUMP
#define MFG_NO_FRAME_PUMP 0
#endif

namespace goblin::variants
{
    inline constexpr bool kFastMapOpen = MFG_SLOW_MAP_OPEN == 0;
    inline constexpr bool kSelfDetach = MFG_NO_SELF_DETACH == 0;
    inline constexpr bool kViewportWindow = MFG_ATTACH_ALL_MARKERS == 0;
    inline constexpr bool kFramePump = MFG_NO_FRAME_PUMP == 0;
}

// ── Overlay backend: ONE is built, and it is the in-swapchain one ────────────────────────────────
// The overlay had four ways to reach the screen, all shipping in one binary and picked by the
// `menu_render_mode` ini key (then named overlay_render_mode):
//   layered    WS_EX_LAYERED + UpdateLayeredWindow - GDI, CPU readback per frame, our own window
//   surface    a DirectComposition SURFACE + an intermediate RT, our own window
//   swapchain  a DirectComposition composition SWAPCHAIN, our own window
//   swapchain_2  our ImGui drawn INTO THE GAME'S OWN frame - no window, no D3D11 device, no DComp
//
// SHIPPING as of 2026-07-28: swapchain_2 only. The three own-window backends exist because DComp is
// unreliable under Proton and because capture/overlay tools react differently to a real swapchain -
// problems that having no window of our own removes rather than works around. Keeping all four also
// meant three of them were never exercised while the fourth was the one under development.
//
// ALTERNATIVE (MFG_OVERLAY_OWN_WINDOW=1): build the three own-window backends back in. Which of
// the three is chosen is NOT an ini key any more - it is MFG_OWN_WINDOW_MODE below, because the key
// that used to pick was repurposed as `menu_render_mode` and load_config() rewrites anything outside
// {native, imgui, dev}. (This paragraph claimed the ini key still chose "exactly as it used to",
// directly above the block that says it does not; corrected 2026-07-31.) The Wine/Proton override
// that forces `layered` does still apply at runtime - that is a property of the machine, not of the
// build.
// VERIFIED 2026-07-30: this variant compiles again (it had silently stopped, reading a config
// field that was renamed away). Checked by building with the default flipped to 1 - the DLL grew
// 51200 bytes and gained the own-window-only literals (D3D11CreateDevice failed, Wine/Proton
// detected, forcing layered), so the code really was compiled in and not skipped.
#ifndef MFG_OVERLAY_OWN_WINDOW
#define MFG_OVERLAY_OWN_WINDOW 0
#endif

// Which own-window backend the variant above builds: 0 = layered, 1 = surface, 2 = swapchain
// (the values of overlay.cpp's RenderMode, in that order). This used to be an ini string, which
// stopped working when that key was repurposed as `menu_render_mode` - load_config() rewrites
// anything outside {native, imgui, dev}, so the drawing modes became unreachable and the backend
// was silently always layered. It is a build choice now, like every other variant here. Ignored
// entirely when MFG_OVERLAY_OWN_WINDOW is 0. (Wine/Proton still forces layered at runtime: DComp
// misbehaves under gamescope, and that is a property of the machine, not of the build.)
#ifndef MFG_OWN_WINDOW_MODE
#define MFG_OWN_WINDOW_MODE 1
#endif

namespace goblin::variants
{
    // true = our own window (layered/surface/swapchain) is built and selectable by ini;
    // false = the in-swapchain backend is the only one compiled in.
    inline constexpr bool kOverlayOwnWindow = MFG_OVERLAY_OWN_WINDOW != 0;
    inline constexpr bool kOverlayInSwapchainOnly = !kOverlayOwnWindow;
}

// ── Add-on SDK host: NOT BUILT into this DLL ─────────────────────────────────────────────────────
// The MCM-style add-on SDK (sdk/mfg_menu_api.h + src/goblin_menu_addons.*) lets another mod add its
// own page to our native menu. The HOST side of it - walking every module loaded in the process and
// asking each for an exported MfgMenuAddonInit, then listing and routing the pages it gets back -
// is being released as a SEPARATE mod, so it has no place in the map mod's binary.
//
// With this at 0 the sources are still in the tree (and still maintained for that separate mod) but
// goblin_menu_addons.cpp is not in CMakeLists.txt and every call site in the menu is compiled out,
// so nothing of it reaches the DLL: no module walk, no "MfgMenuAddonInit" string, no page rows.
// Set to 1 (and put goblin_menu_addons.cpp back in CMakeLists.txt) to build the host in again.
#ifndef MFG_MENU_ADDON_HOST
#define MFG_MENU_ADDON_HOST 0
#endif

namespace goblin::variants
{
    inline constexpr bool kMenuAddonHost = MFG_MENU_ADDON_HOST != 0;
}

// ── CommandList child prototype: kept as reference, not built ────────────────────────────────────
// The RE prototype that opens the game's own CommandList screen as a child of a MenuWindow, with
// hand-made game-ABI std::function objects for the command entries. It has no callers: the native
// menu went a different way (one screen per page off the key-binding movie). It stays in the tree
// because scratch/endgame_research/cmdlist_notes.md points at it as the entry point for the
// unfinished part of that work - but reference material does not need to be in the shipped binary.
#ifndef MFG_CMDLIST_PROTO
#define MFG_CMDLIST_PROTO 0
#endif

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
