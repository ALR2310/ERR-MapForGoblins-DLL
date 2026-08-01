# Native menu: one real screen per page

How the mod's in-game menu (F8) is built out of the game's own key-binding screen, and the engine
mechanics that decide whether it works. Everything below is verified - by disassembly, by the
Scaleform GFx SDK 4.0 sources, or by a measured in-game run. Dead ends are marked as such, because
each one cost a round.

Code: `src/goblin_stall_probe.cpp` (host layer: screen stack, hooks, drawing) and
`src/goblin_native_menu.*` (the model: pages, rows, navigation). Icon sprite bytes:
`tools/generate_menu_icon_tags.py`. Chronological log with the failed attempts:
`scratch/endgame_research/cmdlist_notes.md`.

RVAs are for eldenring.exe 1.16 (the build in `tools/config.ini`); all of them are read through
`GetModuleHandleW(nullptr) + rva`, never hardcoded as absolute addresses.

## 1. What the menu is

Movie `02_160_KeyConfiguration`, dialog class `CS::KeyConfigDialog` (ctor `0x93D290`, vtable
`0x2B0AC40`). The mod does not ship or edit the movie: its bytes are rebuilt at load time
(`goblin_own_movie.cpp`) and its rows are replaced at runtime by three hooks:

| hook | RVA | what it is |
|---|---|---|
| row list build | `0x868590` | builds the item vector for `dlg+0x1268` (`MenuViewItemList`) |
| row draw | `0x8674E0` | the item's `vt+0x8`; we paint label, value, style frame, icon |
| decide | `0x9411A0` | confirm on the highlighted row |
| dialog update | `0x93F540` | the dialog's `vt+0x10`, once per frame; used as a heartbeat |

Every page of the mod menu is its OWN screen (its own dialog and movie instance), stacked in
`g_screens`. That is what makes Q/Esc, the scroll position and the back stack the engine's job
instead of ours.

## 2. Input ownership is the sequence slot (proven)

`FUN_140745570` is the window input dispatcher. It is a VIRTUAL method (30 vtables carry it) and
each window calls it from its own per-frame update - e.g. from `0x14093F5E3`, inside the
key-binding dialog's update. **There is no central loop that picks one window.** A window runs its
own commands only when all three gates pass (`0x1407455D5..0x14074560D`):

```
job not done            FUN_1407A9200(&win[0x1E8])        -> false
sequence slot EMPTY     FUN_1407A9230(win + 0x10)         -> true
vt[0x50](win) == 0      (that method is *(u64*)(win+0x98) > 1)
```

Consequences, all confirmed in game:

- A screen must be stored in the host's **sequence slot `win+0x10`** or the host keeps reacting to
  the same presses. At the title screen a pushed screen took input AND the main menu behind it
  opened its own pages under ours.
- The child-job holder `win+0xA28` does NOT gate input. It draws a screen over the host (which is
  why it looked right over the map) and leaves the host fully interactive.
- Vanilla does the same thing: its "Settings -> Key Assignments" opener `FUN_14094FD40` ends in
  `p_seq(window + 0x10, job)`.

### 2a. A command is THREE callables, not one (proven 2026-07-28)

Inside that dispatcher, the command vector is `win+0x1F8 .. win+0x200`, stride `0x140`, and each
entry carries three `std::function` objects that are consulted in this order
(`0x140745623..0x14074567A`):

| offset | role | called as |
|---|---|---|
| `+0x38`  | **matcher** - does this command match the current input? | `_Do_call(self, &inputState)` -> bool |
| `+0x138` | **guard** - is it allowed right now? | `_Do_call(self)` -> bool |
| `+0xF8`  | **action** - do the thing | `_Do_call(self)` |

`_Do_call` is vtable slot 2 (`+0x10`) of MSVC's `_Func_base`. The engine runs the action **only when
the matcher matches AND the guard allows**; a non-matching or disallowed entry is skipped with
`rbx += 0x140`.

**This is why `invoke_cancel()` does not close a screen.** It finds an entry by reading the action id
out of the matcher's captured state and then calls the action at `+0xF8` directly, skipping the guard.
Measured in game: ESC routed to that path logged a successful cancel, the screen stayed up, and the
player still had to press Q. So a direct action call is NOT equivalent to a keypress, and the comment
that used to claim "calling it directly is what the player pressing Q does" was wrong.

### 2b. ESC is not reachable through commands at all (measured, negative result)

The player's semantics: **Q = one step back, ESC = close all the way to gameplay**, and every native
screen closes on ESC. Three measurements, in order, and each killed a theory:

1. **The command table is identical over gameplay and over the map** (7 entries, same matchers, same
   ids), and ESC fails in both. So hosting is NOT the cause - the map path already uses the native
   sequence slot with no descriptor patch.
2. **Our screen's table does contain a two-action command**: matcher vtable `exe+0x2A997D8` is a
   lambda that captures TWO action ids at `+0x8`/`+0xC` and matches either (`_Do_call` at
   `exe+0x75D2F0`), reading **`0x25` or `0x35`**, guard=allows. The full set the screen listens for is
   `0x18` (Back/Q), `0x19`, `0x22`, `0x25`, `0x29`, `0x2A`, `0x2B`, `0x34`, `0x35`.
3. **Hooking the engine's own action predicate `exe+0x758500`** (`bool(InputData*, actionId, state)`,
   which every matcher calls) while ESC was physically down: the predicate is asked about all nine
   ids every frame and answers **no to every one**. And in PLAIN GAMEPLAY, where ESC opens the system
   menu, the predicate is never asked anything that answers yes either.

So ESC is handled by a path that is **outside the per-screen command mechanism entirely** - our screen
cannot see it, and this is by construction, not a bug in our code. Making ESC close our menu therefore
means integrating with that higher path (the menu manager), which is the same work as being a properly
registered menu; it is not a repair. `poll_escape_close()` is retired in place with this reasoning, and
the `0x758500` hook is only installed when debug logging is on (it is a per-frame path).

Two consequences worth remembering before building on this:
- Our over-gameplay screen is not a registered menu (it is a job pushed onto `CS::CSPopupMenu` with a
  patched descriptor byte), so everything a real menu gets from registration - the HUD mode
  transition, the Back binding, the close/unwind path - has to be replicated by hand, and each
  replication is a separate thing that can be subtly wrong.
- ESC specifically is not bound to Back on `02_160_KeyConfiguration` (Q and the pad's circle are).
  That is a property of the host we borrowed, not a general engine behaviour - on the key-binding
  screen ESC is deliberately not a binding.

Store calls: sequence slot `FUN_1407A9250(win+0x10, refWrappedJob)`, child holder
`FUN_1407A9460(win+0xA28, &old, &ref)`. The sequence store MOVES the reference out of the DLRefPtr
it is handed, so the source slot reads back as 0 afterwards (record the job pointer before the
call, and do not treat the later release as a double free).

## 3. Not every "menu" is a window (proven)

`*(CSMenuMan+0x80)` - what the code used to call the active menu - is **not** of the window family:
its vtable (RVA `0x2AB7958`) reads back as string data (`+0x38 = 65704F6D21435F3A`). Compare with
`WorldMapDialog` (`0x2B2D7D8`) and `KeyConfigDialog` (`0x2B0AC40`), which share
`vt[0]=0x7342B0`, `vt[0x28]=0x744790`, `vt[0x38]=0x745BD0`.

- Storing a job into its `+0x10` faults (SEH at the store).
- PUSHING a job onto it (`FUN_1407EDFA0`) is legal and is the only way in over plain gameplay. That
  path needs descriptor byte 2 (see below) and cannot take input away from anything - which is
  fine there, because no other window is asking for menu input; only the character keeps moving,
  exactly as under the vanilla Esc menu.
- `window_like()` (vt[0] + vt[0x38] + a sane `+0x10`) gates the slot paths only.

## 4. The descriptor byte (proven)

Both screen openers call the same core `FUN_1407ACB00(out, owner, desc, factory)` and differ only
in the descriptor:

```
F11 settings  FUN_1408087E0: { u32 8, u8 2, L"02_040_OptionSetting" }      input works
key binding   FUN_1408078F0: { u32 8, u8 1, L"02_160_KeyConfiguration" }   pushed: no input
```

The byte is written by one instruction, `0x140807967 MOV byte [RSP+0x34], 1`, so the immediate at
RVA `0x80796B` is patched to 2 around our own call and restored immediately. Needed for the PUSHED
path only; a screen in a sequence slot takes input with the 1 the game itself uses.

## 5. A script-set transform is not safe; a frame is (SDK, proven in game)

`DisplayList::MoveDisplayObject` (SDK `Src/GFx/GFx_DisplayList.cpp:363`) re-applies, on every
timeline re-place of an object at the same depth:

```
Cxform, Matrix, BlendMode, Filters, Ratio        (the playhead is NOT in this list)
```

A script-set transform survives only if the object rejects anim moves, and it does not:
`SetDisplayInfo` with x/y clears that flag (`Src/GFx/AS3/AS3_GFxValueImpl.cpp:952`), but the
per-object `continueAnimation` flag - inherited from the movie
(`Src/GFx/GFx_DisplayObject.cpp:1175`) - makes the engine set it back and apply the tag matrix.

Measured consequence: the row icon strip was shifted by `setPos`, with the right cell, on the right
object (row handle and path resolve to the SAME display object), re-applied every frame - and the
icons still vanished after a row was toggled, returning only when the list was scrolled (a scroll
re-renders rows through the path where our write is last).

**Fix that holds:** place the strip with **no matrix in the tag**
(`place2(strip_cid, 17, b'', 'MfgIcon')`), so `if (pos.HasMatrix())` never fires, and fold the row
offset (`ICON_X/ICON_Y`) into the strip's own children. An untouched instance (the right column,
the player's own key-binding screen) then sits at the origin and shows the empty cell, as before.
The shift is `(-cell * ICON_CELL_PX, 0)`.

Dead ends on this one: repainting every frame from our tick; repainting at the tail of the dialog
update; three ticks after a refresh. All of them write before the engine's re-place.

## 6. Frame labels are a protocol (proven)

Root timeline of 02_160: `f2 'FadeIn'` -> `f9 'Loop'` -> `f12 'FadeOut'` (to f19). The engine plays
FadeIn on open, leaves the screen on Loop while it is up, and plays FadeOut on close.

- Jumping to FadeIn without stopping runs through Loop into FadeOut: in game the page appeared for
  a frame or two and faded itself away, leaving only the backdrop.
- Revealing a page = `setVisible(1)` + `gotoAndStop("Loop")` (`0x7499E0`, pointer level
  `resolve_result + 0x18`). Its own FadeOut still works later - the engine starts that itself.
- `setVisible` (`0x733340`) is accepted on the movie root proxy `dlg+0x120` directly, and hiding
  the ROOT leaves every per-clip decision intact. Walking the named top-level clips instead is not
  equivalent: `prepare_form_layout` deliberately hides `Help` and `SelectKey` (the "press a key"
  overlay), and turning the whole list back on flashed that overlay for a frame.

Root-level named clips, for reference: `BG`, `StatusBar`, `Help`, `SelectKey`, `ActionHelp`,
`KeySetting`, `MenuTitle`, plus one unnamed at depth 342 (cid 165, the `MENU_TitleBase` plate) that
can only be reached through the root.

## 7. Parent hand-off without vanilla's transition jobs

Vanilla wraps the screen in a SEQUENCE of three jobs (`FUN_14094FD40`): a transition lambda, the
screen, a transition lambda. Both lambdas (vtables `0x142B120C0` / `0x142B12088`, invoke at
`+0x10` -> `0x140961A40` / `0x140961FD0`) do one thing:

```
obj = *(u64*)(win + 0x1DA0); if (obj) obj->vt[0x10](obj, &flag);   // A: flag=1, B: flag=0
A additionally: jmp win->vt[0xA8](win)
```

That "page aside / page back" is a method of the SETTINGS dialog. Our parent is a second copy of
the key-binding screen, and `CS::KeyConfigDialog`'s vtable **ends at `+0x98`** (`+0xA0`/`+0xA8` read
as string data), so the vanilla path is unavailable unless the parent is 02_040.

What works instead, and looks the same:

- Hide the page when the child's FIRST ROW BUILD arrives (its movie exists by then, so no frame has
  neither of them on screen).
- Bring it back when `job_finished(child)` - the dispatcher's own first gate,
  `FUN_1407A9200(&win[0x1E8])` - reports the child has started closing. That is earlier than its
  slot emptying and earlier than its dialog dying, so the child's FadeOut plays over the restored
  page. Waiting for the slot to empty leaves a 1-2 frame gap; waiting on a timer reads as lag.

## 8. Screen lifecycle signals (proven)

- **Heartbeat**: a dialog's own update runs every frame while it exists, including under a child
  (the input gate stops a parent's commands, not its update). A screen whose heartbeat stopped is
  gone. This is the only signal the bottom of the stack needs, and it reads no foreign memory.
- **Parent's sequence slot**: read only on a parent whose heartbeat is fresh this frame, so the
  object is known alive. Empty slot = the child above it closed.
- **Do NOT** decide "closed" by inspecting the dialog's vtable: that read reported closed on a live
  screen and let presses stack layers. Do NOT make F8 a toggle either - the close is not
  instantaneous, so a third press opened a screen on top of a live one.
- Garbage readings from a dialog for ~600 ms after any close are normal (we are reading a freed
  object until the heartbeat times out), not a sign of corruption.

## 9. Row identity and pools

A native item keeps a POINTER to its fake param row, so each nesting level owns its own pool
(`g_form_pools[level]`) and its own generation counter. A single shared counter invalidated the
parent's live items when a child was built. The level an item belongs to is resolved by the ADDRESS
of its param row (the pools are reserved once and never move); an item from a level that is not the
live one is left untouched rather than handed to the native renderer, which would fill it with
key-binding data read out of our fake row.

The refresh path also builds into TEMPORARY stack lists. They must carry our rows, but must NOT
rebuild the model or bump the generation: doing that made every real row unrecognisable a moment
after the screen was drawn (in game: a parent that came back with dead rows).

Slot to data mapping is the engine's own (`FUN_140739F70`):

```
index = (slotRow + firstVisible[grid+0x348]) * columns[grid+0xD8] + slotCol + flatBase[grid+0xE0]
```

Grid fields (object lies AT `dlg+0xA38`, it is not a pointer): `+0xD0` count mirror, `+0xD4`
cursor, `+0xD8` columns, `+0xDC` visible rows, `+0x348` first visible row, `+0x34C` total rows (the
one the clamp actually reads).

## 10. Hook arity is part of the contract (cost a crash)

`FUN_14093F540` is `(dlg, float dt, uint8_t *consumed)` - the same shape as the input dispatcher it
tail-calls. Hooking it as `(dlg, void *arg)` let the compiler use `r8`/`xmm1` as scratch, so the
game received whatever was left there. It survived on luck until the detour grew, then `r8` came
through as 0 and the game read a byte through it: an access violation at `0x14093F5D1`
(`movzx eax, byte [rdi]`, RDI=0) that looked exactly like "the host we pushed onto is bad".

Check the prologue before writing a detour signature: `mov rdi, r8` means there is a third
argument, `movaps xmm6, xmm1` means the second one is a float. This applies even when the detour
"only logs".

## 11. Still open

- The child draws OVER the parent unless we hide it ourselves (section 7).
- Icon variant 1 (`native_menu_icons = 1`) is still matrix-based and therefore still resettable;
  only variant 2 was moved off the matrix.
- Hosting over gameplay is a PUSH, not a slot, so nothing takes input away from the HUD there. A
  real window host for that state has not been found; `log_window_candidates()` scans CSMenuMan and
  logs anything window-like when an open finds no window.
