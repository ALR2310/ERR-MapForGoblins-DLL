# Retired: the F6 host (the graphics screen as a second home for our menu)

Removed from the code on 2026-07-28. Everything worth keeping is here; the shipping native menu is the
F8 path (`02_160_KeyConfiguration`), see `research_native_menu_screens.md`.

## What it was

`open_graphic_form()` opened the game's own graphics-settings screen (`02_042_PC_GraphicSetting`) over the
live map and let the engine populate it, with our populate detour swapping in OUR rows via the same
`build_our_rows()` the F8 path uses. It needed the map open (the host came from the map window) and the
screen went into the child slot rather than the sequence slot, so it drew on top of the map instead of
replacing it.

Engine details, verified while it existed:

- Opener `FUN_1408079E0(out, owner, flag)` builds an `OptionSettingDialog` job through the generic
  builder `FUN_140808630` with the movie block `{8, 1, L"02_042_PC_GraphicSetting"}` and factory
  `FUN_140807B10`; `owner` is the same `host + 0x50` the game's own call site at `0x14094FB02` passes.
  Its tail is `FUN_1407418D0`, a DLRefPtr assign, so `out` already holds an owned reference (unlike the
  key-binding opener, which needs two conversions).
- Rows come from populate `FUN_14095A5D0`. The page is built by the same page factory
  (`FUN_14095EB90`) our settings tab borrows, with clip index **0xD = "GraphicOption"** instead of
  **3 = "CameraSetting"**.

## Why it was retired: it does NOT scroll, and the row cap is the same

This was the whole reason to try it, so it is the important result. Measured in game 2026-07-28 by
padding our page to 30 rows (recycled labels, filler value pointers):

- **No crash** - the appends past the end are absorbed, not fatal.
- The page showed **15 rows, a 16th below the fold, and focus wrapped to the top** when scrolling past it.
- So the list is capped at **sixteen items** - the `BasicViewItemList<EditProperty,16>` the interface
  audit named (`docs/audit_2026-07-28_interface_contracts.md`) - and it does **not** scroll.

The key-binding host caps at the same sixteen; it merely shows fewer at once (12 vs 15). So the graphics
screen buys **visible rows, not scrolling**. More than sixteen settings has to be PAGED, which the F8
screen-per-page model already does - so there was nothing to gain by developing this host further, and
two hosts would have been two things to keep working across game patches.

## What it was still better at, and what to harvest

1. **It is centred.** The key-binding screen is a two-column screen (action + bound key), so its grid
   lives in the left half; the graphics screen is centred. That is a layout property of the host, and the
   F8 path is being centred instead by moving its row section - the root's named children in
   `02_160_keyconfiguration.gfx` are: depth 1 `T`BG` (background), 6 `pHelp`, 176 `SelectKey`, 338
   `ActionHelp`, **344 `KeySetting` (the row list)**, 672 `MenuTitle`. Shifting the movie ROOT moves the
   background with it; shifting `KeySetting` moves only the rows.
2. **Real sliders and combo boxes.** The graphics screen carries widgets the key-binding screen does not.
   When we need a numeric setting (font scale, opacity, icon size), the question to answer is whether
   such a widget can be built INSIDE the F8 host - the row builders are shared
   (`p_append_combo` = `0x948FA0`, label pack ctor `0x760970`, help ctor `0x760790`, combo item list ctor
   `0x9543B0`, pack dtor `0x742C90`), so the widget construction is likely portable. Only if it is not
   should this host come back, and then with the knowledge above.

## Costs it would have carried

Icons would have needed the same icon-strip splice into `02_042` that `02_160` gets from
`goblin_own_movie.cpp`. That mechanism works, but movie rewriting is the area that produced the
black-map-tile defect (`project_black_tiles_investigation` memory), so it is not free.
