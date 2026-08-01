# The HUD does not come back after our over-gameplay screen closes

**Status 2026-07-28: ROOT CAUSE FOUND AND FIXED (pending an in-game confirmation of the fix).**

`CS::CSFeManImp+0x78` is the HUD's own visibility mode: **3 = HUD on in gameplay, 1 = hidden under a
menu, 0 = what our pushed screen leaves behind**. In the stuck state the engine genuinely believes the
HUD is off - mode 0, all 31 per-widget flags cleared, menu ids 7 and 26 demoted to registered-only.

Proven by poking the live process (`scratch/poke_hud_mode.py`): writing 3 into that byte brought the
HUD up instantly, the mode STAYED 3 for the whole watch window, and the widget flags went 2/31 ->
14/31 with the visible-id set back to the full gameplay `{5,7,8,19,23,26}`. So the mode is
**edge-driven, not recomputed per frame**, and the engine only reaches 3 again through its own
0 -> 1 -> 3 transition - which is exactly why opening and closing any native menu repairs it.

**Timing matters as much as the value.** The first version restored the mode from `menu_torn_down()`,
which prune_screens() only reaches after the heartbeat times out - `level 0 gone (no update for
609 ms)` - and that read in game as a visible lag: the engine starts the HUD coming back TOGETHER
with a native menu's close animation, we started it after the screen was already gone. So the restore
now hangs off `job_finished(dlg)` (`restore_hud_when_close_starts`), the engine's own "this screen's
job is done" predicate on `screen+0x1E8` that is already used to reveal the page under a closing
screen, and which is true while the screen is still fading out. The `menu_torn_down()` call stays as a
fallback for the paths that never go through a normal close; it is a no-op once the early one has run.

The fix (`goblin_stall_probe.cpp`, `hud_mode_capture` / `hud_mode_restore`) is symmetric cleanup
rather than a policy: capture the mode before we push, put that same value back once every screen of
ours is gone. Guards: only write when the mode is still 0 (never override a legitimate "menu on top"
state), and only trust the singleton when the `CSFeAutoHideCtrl` vtable is at `+0x4E70` (so a game
update that moves the layout makes this do nothing instead of corrupting). The singleton comes from
an AOB registered as non-critical `feman_slot` in `tools/aob_signatures.py`, so a miss only means the
old behaviour returns. The map-hosted path is untouched by construction: the capture sits in the
`pushed` branch, which requires `!host`, and over the map the map window IS the host.

The rest of this file is the trail that led there - it is kept because most of it is exclusions, and
because two of the wrong turns were caused by method rather than by the game.

## The defect

Press F8 over gameplay, then close the screen: the game's HUD stays hidden until any native menu
(inventory, map, ESC menu, settings) is opened - then it comes back on its own. Opening and closing those
native menus never causes the problem, so the game's own path restores whatever we disturb. Pre-existing,
not a regression of the 2026-07 work.

## What is proven

- Over gameplay our screen is PUSHED onto the active menu, which in gameplay IS the HUD menu
  (`*(CSMenuMan+0x80)`), with the descriptor byte patched to 2 so the screen receives input.
- The CSMenuMan singleton pointer lives at **`exe+0x3D6B7B0`** (found at runtime by scanning the module's
  data sections for the live pointer - `[hudprobe] CSMenuMan pointer ... is stored at exe+0x...`).
- **The menu-state array is at `singleton+0x90`: 71 one-byte entries (indices 0..0x46), bit 0 = the entry
  is registered, bit 1 = it is visible.** So 3 = shown, 1 = hidden, 0 = unregistered. Established from the
  writer itself (see the tool below): `cmp ax,0x47` / `movsx ecx,[rdx+rdi+0x90]` / `and eax,1` (acts only
  on registered entries) / `or cl,3` to show / `and cl,1` to hide / store. The writing function is
  `exe+0x734342 .. exe+0x73443D` (251 bytes, per .pdata); it resolves the entry index from a byte-swapped
  4-byte key through `exe+0x767DF0`.
- Across our screen, entries in that array go from 0 to 1/3 (our screens register entries) and some go
  3 -> 1. When the game restores the HUD it **zeroes** entries rather than setting them to 3.

## Excluded, each by an in-game run

1. `CSMenuMan+0x97` and `+0xAA` (the two bytes that flip 3 -> 1): written back to 3, both writes logged,
   HUD stayed hidden.
2. The counter at base menu `+0x168` (goes up by one per open, never comes down): restored, HUD stayed
   hidden.
3. The HUD menu's own sequence-slot job at `+0x10` (a different object after our screen than before):
   saved with a held reference and put back through the engine's own slot setter - HUD stayed hidden.
4. Zeroing every entry our screen added (mimicking what the game does): HUD stayed hidden, and pressing
   ESC right afterwards hung and crashed the game - that array is live state the menu machinery
   re-registers, so writing it from our thread races it.
5. Restoring every 3 -> 1 entry across the whole 71-entry array: reported "no entry went from 3 to 1",
   i.e. our screen does not hide the HUD through this array at all.
6. Hosting our screen on the HUD menu's child holder (`+0xA28`) instead of pushing - the structural theory
   that we displace the HUD screen rather than parenting to it. The holder read as empty and took the job,
   but the screen never came up, so that field is not a child holder on this object.

## What the defect actually IS, measured per menu id (2026-07-28, second pass)

The menu-state array is indexed by the **menu id itself**. The engine's accessor `0x140767DF0` is three
instructions (`mov word [rcx], dx; mov rax, rcx; ret`) - it only wraps the id in a temporary - and every
reader then does `state = [id + CSMenuMan + 0x90]` behind a `cmp id, 0x47`. The state is **three** bits
(`and al, 7`; `or byte [..], 7` exists at `0x14074571A`), bit 0 = registered, bit 1 = visible. The
engine's own hide is `and byte [id + this + 0x90], 1` at `0x1408D4447`, i.e. it KEEPS the registration.

Reading the array per id instead of as hex changes the picture completely:

| moment | visible ids |
|---|---|
| gameplay, before our screen | **5, 7, 8, 19, 23, 26** |
| after our screen closed | 5, 8, 19, 23 - **7 and 26 dropped to registered-only** |
| after a native menu open/close | back to the gameplay set |

So the defect is on the HIDE side, not the restore side: our screen hides menus 7 and 26 (the normal
3 -> 1 the engine does under a full-screen screen) and our close never shows them again.

Two earlier entries in this file were wrong and are corrected by that table:
- The "clean baseline" behind excluded hypothesis 5 ("no entry went from 3 to 1") was sampled during
  STARTUP, where only id 5 is visible. In gameplay six ids are visible and two of them do go 3 -> 1.
- Excluded hypothesis 1 wrote 3 back into `+0x97` and `+0xAA`, which are **exactly ids 7 and 26**. That
  still did not bring the HUD back, so the array is a REFLECTION of state held in the menu objects, not
  the switch. Do not spend another run writing to it.

Also excluded this pass: **`CS::CSFeAutoHideCtrl`** (the HUD fade controller, at `CSFeManImp+0x4E70`,
singleton `*(exe+0x3D6B880)`). Probed across the whole cycle it never moves - mode 0, delay 6.0, all 42
slots zero - not even when the HUD returns.

### The hide path, named (write watch, 2026-07-28)

A hardware write watch on id 7's byte, armed at PUSH time, caught the hide in the act at
**`exe+0x7754B6`** - a DIFFERENT writer from the `exe+0x734438` this file previously named - and the
VEH's stack scan gave the chain `0x7750E0 -> 0x772A3E -> 0x771D3C -> 0x76E8CC -> ... -> 0x766B46`,
i.e. the CSFeManImp update. The owning function is **`0x140775320(feman, float)`**, and it opens:

```
eax = byte [feman + 0x82C1]     ; signed override
esi = byte [feman + 0x78]       ; mode
if (al >= 0) esi = eax          ; cmovns - the override wins unless negative
switch (esi) { ... }            ; then show/hide the menu set for that mode
```

per-id, with `if (registered) { show ? state |= 3 : state &= 1 }`. So the state array is
**recomputed from the mode every frame**, which is the mechanical reason writing 3 into it never
worked. Mode 1's branch (`0x775369..0x775436`) clears ~25 per-widget flag bytes inside CSFeManImp
(singles `0xC8 0x2B8 0x4A8 0x5F8 0x748 0xF20 0x1200 0x1208 0x3648 0x4D58 0x4E58 0x6568`, arrays
`0x9E0` stride `0x150` x4, `0x1250` stride `0x128` x8, `0x1F08` stride `0x128` x7).

### MEASURED: the engine state fully recovers on its own, so the defect is on the RENDER side

The widget-flag digest settles it. One run, `19:58:20` to `19:58:49`, sampling on every menu-manager
change:

| time | mode | widget flags | visible ids |
|---|---|---|---|
| 19:58:20.077 before our push | 3 | **14/31** | 5,7,8,19,23,26 |
| 19:58:27.363 our screen closed | 0 | 2/31 | 5,8,19,23 |
| 19:58:36.383 (9 s later) | 1 | 0/31 | 5,7,8,19,23,28 |
| **19:58:41.183 (14 s later)** | **3** | **14/31** | **5,7,8,19,23,26** |
| 19:58:48.455 native menu opens (id 6) | 0 | 1/31 | 4,5,6,8,23,37 |

One conclusion holds, one was wrong:

1. **The mode byte drives the widget flags** - mode 3 -> 14/31, mode 1 -> 0/31, mode 0 -> 1..2/31,
   with no exception in any sample. So `CSFeManImp+0x78` is the HUD's own visibility state.
2. ~~Every engine-side signal recovers on its own, so the defect is on the render side.~~ **WRONG,
   and retracted the same day.** The `19:58:41` sample was a TRANSIENT, not a recovery. Read out of
   the live process while the HUD was visually gone (`scratch/read_hud_state.py`, no rebuild
   required):

   ```
   mode=0 override=-1 effective=0 flags=2/31 visible=[5, 8, 19, 23]
   registered-only ids: [4, 7, 9, 11, 12, 13, 14, 15, 17, 18, 24, 25, 26, 27]
   ```

   That is exactly the post-close state: mode 0, widget flags cleared, ids 7 and 26 demoted to
   registered-only. **The engine does NOT think the HUD is on**, so Scaleform is not implicated and
   the earlier "sampling caught it back at baseline" reading was an artefact of sampling only on
   menu-manager churn.

Method note that made the difference: read the LIVE process instead of rebuilding the DLL. With the
game sitting in the bad state, `scratch/read_hud_state.py` gives mode, override, all 31 widget flags
and the per-id menu state in one shot, and `--watch N` prints only on change - so a single keypress
by the player can be captured as a before/after without a build or a restart.

### Both candidate switches are excluded - and why the method was at fault

Measured across a full cycle: the mode is `3` before our screen, `0` right after our close, then it
moves `1 -> 0 -> 3 -> 1 -> 0` on its own; the override is `-1` (none) throughout. At one sample the
state matched the pre-push baseline **exactly** - mode 3 AND visible ids `{5,7,8,19,23,26}` - well
before any native menu was opened. So neither the mode byte nor the state array stays wrong.

The real problem with the last several rounds: every correlation was against the log label
"while the HUD was hidden", which is a fixed string printed on close, **not a measurement**. Nothing
in the probe ever established when the HUD was actually back. Do not repeat that - the widget-flag
digest above (`[hudflags]`) is the measurement, and any future claim about this defect should be
correlated against it rather than against a label or the user's timing.

## The job containers, measured (2026-07-28)

The base menu carries several `DLFixedVector`s. Their layout is not a guess: `0x140733D70` is the
engine's own remove-by-value for this container type, and its asserts name
`dantelion2/Core/Util/DLFixedVector.inl`. From that code the element storage starts at
`align8(vector_base)`, elements are 8 bytes, capacity is 8 (the push `0x1407AA400` refuses past it),
and the **count is at `base+0x48`**. So on the base menu:

| vector | count | what a run showed across one F8 cycle |
|---|---|---|
| `menu+0x10`  | `menu+0x58`  | element[0] goes from garbage (`0xA38478E380A3A37`) to a live heap pointer; the COUNT does not change, i.e. a pushed element was popped and the stale slot never cleared |
| `menu+0xD0`  | `menu+0x118` | 0 before AND after, although `p_push_job` (`0x1407EDFA0` -> `0x1407F0B50`) pushes exactly here - so the engine drains it synchronously |
| `menu+0x120` | `menu+0x168` | **0 -> 1 and stays 1** |

So something of ours does settle in the third one. That is NOT the same as saying it is the HUD
switch: writing that count back was excluded hypothesis 2 below and did not bring the HUD back.
The leftover therefore matters as an IDENTITY (what object is it?), not as a number, which is what
the RTTI probe below answers.

Useful engine primitives found while measuring, for whoever needs the removal side:
`0x140733D70(vector, value)` scans from the last element down, shifts the tail and decrements the
count; its 7 call sites (e.g. `0x1407ADBF5`, `0x14089E2BB`) all follow it with a release of the
object's own reference, so a hand-rolled removal must release too or it leaks.

## Naming objects instead of logging addresses

`eldenring.exe` ships full MSVC RTTI, so `scratch/rtti_map.py` turns a vtable RVA into a class name
(`py scratch/rtti_map.py 0x2a93a60` -> `CS::MenuWindow`). `log_job_stack` now logs
`vt exe+0x...` for the menu, its `+0x10` slot and every element of both vectors, so one run names
every participant. Do this before theorising about any of these fields.

## The tool that made the difference, and how to reuse it

`goblin::watch` in `src/dllmain.cpp`: a **hardware data breakpoint**. `request(address, tid)` queues, our
background thread arms it (Dr0 + Dr7 write-watch, 1 byte), and the VEH logs the writing instruction as
`exe+RVA` and then disarms itself in the handler.

Two rules learned the hard way, both of which hung the game:
- **Never arm on the target thread.** Setting debug registers needs SuspendThread on that thread, so
  arming from the thread you are watching suspends you with nobody left to resume it. `arm()` now refuses.
- **Make it one-shot.** The byte turned out to be written from a hot path; staying armed drowns the thread
  in exceptions.

Also: verify deploys. Two runs were wasted testing an old DLL because the game held the file and the copy
silently failed - `scratch/deploy_probe.py` now hashes every target and exits non-zero on a stale one.

## Where to look next

The HUD is its own set of menus/widgets; the switch is on that side, not in the menu-state table. Two
concrete starting points:

- Watch the HUD's own objects rather than CSMenuMan: find the HUD menu instance (it is the active menu in
  gameplay, i.e. `*(CSMenuMan+0x80)` before our push) and diff a much larger window of it - the current
  snapshot covers only 0x400 bytes.
- Or attack from the restore side: while the HUD is hidden, arm the write watch on the HUD menu object's
  own fields and open a native menu; the game's restore will name the instruction that flips it, the same
  way it named the array writer.
