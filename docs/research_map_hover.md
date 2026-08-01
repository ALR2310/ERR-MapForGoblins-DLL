# Map hover -> which icon? (RE for manual-hide + plan_3 hover overlay)

Goal: at runtime, know WHICH map icon the cursor is over. The game shows that
icon's label(s) on hover; if the label is resolved by NATIVE code on hover we can
hook it and read the point's WorldMapPointParam (textId/iconId/pos) to identify our
marker exactly (better than compute-nearest in dense clusters).

Exe: `G:\Steam\steamapps\common\ELDEN RING\Game\eldenring.exe` (ImageBase
0x140000000, v2.6.2.0). Ghidra project `scratch/ghidra_proj`. Scripts
`scratch/ghidra_scripts/HoverRE{,2,3}.java` -> `scratch/hover_re{,2,3}.c`.

## Established (HIGH confidence, static RE)

- **Each map point is a native instance that EMBEDS its WorldMapPointParam.**
  `CSWorldMapPointIns` (vtable `0x142b487a8`, ctor `0x140a811e0`). Its vt[0]
  (`0x140a812d0`) sets `instance[0xe] = CS::WorldMapPointParam::vftable`, i.e. a
  WorldMapPointParam sub-object lives at **instance + 0x70**. So a hovered instance
  ptr -> read +0x70 -> textId/iconId/pos -> identify our injected marker.
- **Three point types are built** (build fn `0x140a82a80`, three loops over data-
  source collections idx 0x26/0x27/0x21 via `FUN_140cf6300` count):
  - `CSWorldMapDiscoveryPointIns` (create `0x140a7f990`) - discovery/grace points
  - `CSWorldMapReentryPointIns`   (create `0x140a854a0`)
  - `CSWorldMapPointIns`          (create ctor `0x140a811e0`) - generic (loot/POI)
  Each element: alloc `0x141eb9ed0`, create instance, virtual predicate
  `[rax+8]` (visibility/dispMask filter), then insert `0x140a81830` +
  `0x140a81b50` + add-to-container `0x140a84450` into containers on the map-UI
  owner (e.g. `[owner+0x398]` = an ordered/indexed set keyed by a dword at +0x20).
- **No name/FMG resolution at build.** Full call listing of the build range
  `0x140a82a80-0x140a82ea6` (HoverRE3) contains ONLY count/alloc/create/insert/
  add + a couple of virtual calls (`[r8]`, `[r8+0x68]`). There is NO call into the
  msg repository / FMG and NO string build. => **point labels are resolved on
  demand (per hover/render), not baked into Scaleform at build.** This is the key
  green light for the label-hook approach.
- Prior finding (fork `marker_to_mapspace_re_findings.md:82`): the hover HIT-TEST
  (which sprite is under the cursor + the highlight) is computed Scaleform-side, no
  native "hovered index" field. That is about the highlight, NOT the label resolve.

## Not-yet-found (the next RE step)

The native function that, on hover, takes the focused point's WorldMapPointParam
and resolves its label (textId -> FMG PlaceName) for display. Notes for the hunt:
- xref-to-vtable is useless here: consumers call instance methods INDIRECTLY via
  the vtable ptr at `[instance+0]`, so `getReferencesTo(vtable_VA)` returns only
  ctor/dtor (confirmed: HoverRE2 found only `0x140a811e0`/`0x140a812d0`).
- Better leads to try next:
  1. Resolve `CS::WorldMapPointParam` wrapper vtable (symbol lookup returned 0 -
     get it from the ref in `0x140a812d0`, `instance[0xe] = <vftable VA>`), dump
     its methods; find a `getTextId`/`getRawRow`/`getName` accessor, then xref THAT.
  2. Find the map-point-detail / name panel: search functions that read a
     WorldMapPointParam textId offset AND call the msg repository
     (`msg_repository_slot` @ exe+0x3D7D4F8; the FMG getter). If such a fn is called
     per-hover (from the map dialog update, not a build loop) -> that's the hook.
  3. The map dialog holds a "current/focused point". Find the map WorldMapDialog
     update/tick fn (map-open ctor `0x1409cef10` per plan_3 A1) and look for a
     member that stores a CSWorldMapPointIns* set each frame from the Scaleform
     focus, then read/hook that member.
  4. Scaleform->native callback: the worldmap gfx may fscommand a native handler on
     focus change; find the map gfx external-interface dispatch.

## Pass 4-6 result: the native label system is for SPECIAL points, not ours

Followed the "on-hover FMG resolve" lead (HoverRE4-6). Found a complete native
label subsystem, but it is NOT the generic-marker path:

- One "label-slot manager" object with a **6-slot array @ +0x28** (stride 0x30):
  slot = {id @+0x00 (low32; -1=empty), type @+0x08, **state @+0x0C (7 = hovered)**,
  pos @+0x38.., textId @+0x48..}. Aux array @ +0x148 (5), count @ +0x170.
- Functions on it:
  - `FUN_1409faf10(mgr, point)` - hover handler: id = `FUN_1409f7b00(point)`, find
    slot, set state=7, build label `FUN_1409fc8e0(mgr, slot, 3)`.
  - `FUN_1409f98c0(...)` - register a slot (id, type, textId -> FMG name).
  - `FUN_1409fc8e0(mgr, slot, mode)` - resolve+show label: FMG `FUN_140d10ae0(
    MsgRepository=DAT_143d7d4f8, slot.textId)`.
  - `FUN_1409fbcc0(mgr, dt)` - per-frame state-machine over the 6 slots.
  - `FUN_1409fa560 / FUN_1409f8440` - remove/compact slots.
- **The id is a field-entity handle** (`FUN_1409f7b00`: top nibble bits 28-31 == 1
  => resolve via registry `DAT_143d65f88`), NOT our WorldMapPointParam row id.
- **It is the SPECIAL-point system**: `FUN_1409fa560`/`FUN_1409fbcc0` contain the
  literals `"CS::PartyMemberInfo::LeavePartyMember"` /
  `"...Update_PartyMemberState"`. So these slots are co-op party members / reentry
  points / great runes / spectral-steed - field objects with entity handles, gated
  to a tiny set. Our generic loot/POI markers (plain WorldMapPointParam rows built
  as CSWorldMapPointIns, no field handle) do NOT enter this manager.

### Conclusion on the native hover-label hook

For OUR markers there is no cheap native "hovered point" hook: the generic-point
hover highlight+label is Scaleform-side (matches the fork's prior finding), and the
one native label system that exists is for special field-entity points only. Chasing
the generic path further would mean decoding the Scaleform worldmap gfx callbacks -
high effort, low marginal value, because:

## SOLVED + LIVE-CONFIRMED: hook FUN_14087a8e0, read row textId

The full mechanism was verified in-game with a probe DLL (src/goblin_maphover_probe.cpp,
TEST ONLY - not for release). Confirmed exact identity of the hovered marker:

- **Hook `FUN_14087a8e0` @ 0x14087a8e0** (unique 57-byte AOB in goblin_maphover_probe.cpp).
  Fires once/frame with the pin the game is naming in the popup. __fastcall; **item = RDX
  (arg2)**; item may be null (nothing focused -> skip).
- **Gate**: `*(void**)item == module_base + 0x2AD6688` (CS::WorldMapPointPinData). Other
  pin types (e.g. vtable rva 0x2AD8228 = grace/discovery) are not our loot/POI markers.
- **Identity (LIVE-CONFIRMED)**: `row = *(void**)(item + 0x248)` (the param row);
  **`textId = *(int32*)(row + 0x30)`**. For OUR injected markers this is the offset-encoded
  textId in 500000000..599999999 (e.g. 500020855, 500800010, 500010020) -> look it up in
  our marker table for the EXACT hovered marker. row_tid==0 / out-of-band => a vanilla
  point, not ours (skip). NB item+0x5c is NOT the iconId (read 0 live); textId is the id.

This is the game's OWN choice (nearest-to-reticle-within-radius, then displayed), so it
matches the highlight+popup perfectly - no compute-nearest approximation. Use for BOTH
manual-hide and the plan_3 hover overlay: in the hook, resolve textId->our marker, then
(manual-hide) toggle it hidden, or (overlay) show its extra info.

Remaining for the real feature (not the probe): thread-safe publish of the current
hovered marker from the hook to the overlay/hide logic (the hook runs on the game thread),
and revert/guard the probe (it's a raw logging hook). The AOB must be registered in
tools/aob_signatures.py (critical=false or a live-checked entry) before shipping.

## (earlier) BREAKTHROUGH (pass 9-18, Ghidra): the generic hovered-point IS native

The earlier "special-slot-manager only / generic is Scaleform" conclusion was chasing
the wrong subsystem. The generic-marker focused point + its name ARE resolved
NATIVELY; only the visual hit-highlight glow is Scaleform. (Full writeup:
scratch/hover_findings_pass9-18.md; decompiles hover_re9.c..hover_re18.c.)

The chain (v2.6.2.0, ImageBase 0x140000000):
- WorldMapDialog vtable VA 0x142b2d7d8 (ctor 0x1409cef10); vtable[2] update 0x1409cfb60
  -> WorldMapDialogBase per-frame update 0x1409c32f0.
- Base ctor 0x1409be5e0 wraps Scaleform clips into native list controllers
  (CS::WorldMapItemControl): ItemList, WarpList, MarkerList, MemoList + Body/PointCursor
  + Body/PlaceName. Nearest-point finders: dialog+0x715={ItemList},
  dialog+0x71b={MarkerList,WarpList,MemoList}.
- Each frame 0x1409c32f0: FUN_1409dbf80 picks the provider item NEAREST the cursor
  within radius DAT_142b2c888, stores it into the cursor via FUN_1409bd470, then
  FUN_14087a8e0 displays that item's name into Body/PlaceName (native -> Scaleform text).
- So the game literally does compute-nearest-to-reticle-within-radius, but we can READ
  its exact pick instead of approximating.

**Static-anchor / offsets (the deliverable):** focused point held in
CS::WorldMapCursorControl (ctor 0x1409bc5b0) at **WorldMapDialog + 0x2DB0**:
- +0x118 = pending focused-point ptr, **+0x140 = committed/current focused-point ptr**
- +0xFC/+0x104/+0x10C = cursor pos vec; +0x114/+0x150 = state bytes
The focused "point" is a WorldMapItemControl/Selectable wrapper (NOT a bare
CSWorldMapPointIns): virtuals [+0x20]=pos, [+0x28]=isSelectable, [+0x40]=lineCount,
[+0x38](i)=name line text (wchar_t*), [+0x48]=icon/type id.

**Two gaps to close with a live probe (game must be running + map open + hovering):**
1. No static global to the live WorldMapDialog was found. Reach it by scanning memory
   for an object whose [0]==(base+0x2b2d7d8) [scratch/probe_dialog.py does this], or
   hook FUN_1409bd470(rcx=cursor, rdx=item) / FUN_14087a8e0(rdx=item) / FUN_1409c32f0.
2. Confirm OUR injected loot markers register as selectable items (ItemList/MarkerList)
   and thus set cursor+0x140 on hover. Probe: hover an injected marker, read
   dialog(+0x2DB0)+0x140, inspect the item's fields / [+0x48] icon / [+0x38] name.

If confirmed: the DLL reads cursor+0x140 each map frame (or hooks FUN_1409bd470) to get
the EXACT hovered marker the game chose - matches the highlight+popup perfectly, and
beats compute-nearest in dense clusters. This is the path for BOTH manual-hide and the
plan_3 hover overlay.

## (superseded) earlier fallback: compute-nearest

Compute-nearest (plan_3 A2): read the live map reticle (WorldMapArea +0xFC/0x100,
pan +0x378/+0x37C, zoom +0x380), project OUR injected markers with our transform
(OFFSET_X=7042, OFFSET_Z=16511) and pick the nearest to the reticle. Works for OUR
markers (which is all manual-hide needs); weaker only in dense clusters, where the
native label-hook would win.
