# Retired: the overlay's three on-map visuals

Removed 2026-07-28 on request. After this the overlay draws **only the F10 menu** - everything the
player sees on the map itself is the game's own rendering.

## What went, and what replaced it

| overlay (gone) | native replacement |
|---|---|
| `draw_hover_tooltip()` - ImGui panel right of the reticle, marker name + height vs the player | `drive_own_tip()` -> the **MfgTip** panel (`goblin_maphover.cpp`) |
| `draw_focus_banner_onscreen()` - top-left "Showing only: <category> - <region>" + the clear hint | `drive_own_banner()` -> the **MfgBanner** panel |
| `draw_map_highlights()` - highlight.png ring projected over each focused marker | the **glow-icon swap** in `goblin::apply_focus_highlight()` |

The removed source is kept verbatim in `scratch/retired_overlay_map_visuals.txt` (gitignored) in case
a detail of the old layout is ever needed.

## Deliberate behaviour differences

- **No marker NAME in the hover panel.** The game's own popup already shows the name, so the native
  panel only adds what the game does not have: how far above or below the player the marker sits.
  This was already true before the removal - the native panel was written that way on purpose.
- **No per-point ring.** The glow icon marks the same markers inside the game's own render, so it
  cannot drift from the map view; the ring was projected by us and had to be re-derived on every
  zoom/pan.

## The trap this removal had to avoid

`hover_info` (`enableHoverInfo`) was read **only by the overlay**. Deleting the overlay panel without
touching anything else would have turned a documented ini key into a no-op: the player switches hover
info off, the native panel keeps showing. `drive_own_tip()` now checks that key first and hides the
panel when it is off - it is the only consumer of the key now. There are no ini keys for the banner
or the rings, so nothing else needed re-pointing.

## What fell out of it, both wanted

1. **With the menu closed the overlay has nothing to draw**, so it is simply not visible: `want` is
   now just `open` in both render loops (DComp/layered and sc2), the "menu closed but hovering or
   projecting" branch is gone, and the window's lifetime matches the menu's.
2. **The per-loop hover lookup is gone.** It existed only to feed the tooltip, and its heavy variant
   (`native_hover_row_impl`, which rebuilds the ~9k-row snapshot with per-row event-flag reads) was
   once responsible for a map FPS collapse - hence the 200 ms cached `native_reticle_row()` in the sc2
   loop. Neither runs per-frame any more.

`native_hover_row()` itself STAYS: manual hide (`dllmain.cpp`, hide the marker under the cursor) uses
it when a V3 native marker has no engine pin.

## Left behind on purpose

`g_highlight_tex`, `g_highlight_srv`, `g_highlight_texid`, `g_highlight_uv0/uv1` are still built (a
D3D11 texture plus a custom rect in the ImGui font atlas) and are now written but never read. Removing
the atlas rect changes atlas packing, which is not something to do in the same pass as a behaviour
change - it is a few KB, and a separate two-line follow-up. `goblin_mapproject` also has no overlay
consumer left, but `goblin_inject.cpp` and `goblin_stall_probe.cpp` still use it, so the module stays.
