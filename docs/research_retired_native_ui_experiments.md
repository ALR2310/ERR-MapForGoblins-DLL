# Retired native-UI experiments: settings re-host over the map, and the F11 standalone settings menu

Everything here was removed from the code on 2026-07-27. It is kept because the reverse-engineering is
expensive to redo and several facts are reusable for any future native-UI work. Nothing in this file is
live code; treat RVAs as "this build" and re-verify with the AOB checker before trusting them.

The shipped, working native menu is the F8 path (rebuild of `02_160_KeyConfiguration` driving a real
`CS::KeyConfigDialog`), documented separately in `research_native_menu_screens.md`. That path replaced
both experiments below.

## Why these existed

North star: render all mod UI through the game's own Scaleform UI and retire the ImGui overlay. Two
reasons, both concrete. The overlay is the main antivirus false-positive surface (D3D hooks, its own
window, key polling), and on Steam Deck / Wine a separate top-level window fights the game for focus.

## Experiment 1: settings re-host into the worldmap movie

Idea: the map is the one screen the mod already lives on, so show our settings there. That needs the
settings screen's clips to exist INSIDE the worldmap movie, so the map dialog can bind them by name.

**Path A - runtime per-tag definition inject** (`inject_tag_stream`). Registered our DefineShape /
DefineSprite tags into the live movie one tag at a time. Outcome: the defs land in a different
dictionary from the one instantiation resolves against, so placing them crashed. Abandoned.

**Path B - splice the blob into the parse buffer** (`swap_movie_buffer`, gated by `NATIVE_SPLICE`).
While the worldmap movie was still parsing, we replaced the reader's source buffer with our own copy
that had the settings closure spliced in, so the GAME's parser registered everything into every dict.

What was PROVEN to work:
- The buffer swap itself: `[movieswap] SPLICED blob 40149 bytes at ShowFrame@68865 -> new movie len
  109888 (was 69739)`, and the movie still loaded and rendered.
- Registration: `[rehost] CHARDEF charId=300 owner=0x...` through 448 - the whole 149-definition
  closure (cids 300..448) landed in the same char registry as the movie's own characters (charId 171
  logs the same owner).
- Registration is therefore NOT the blocker for any future attempt. Instantiation/binding was.

**Task #4 - root placement** (`po2loader_detour` -> `inject_root_place_tags`, PO2 loader at RVA
`0x11E23B0`): appended five root PlaceObject tags to the map root's frame 1 so the engine would
instantiate the settings roots itself.

**Task #5 - binding verification** (`resolve_detour` on the name resolver `0x14074a2f0`): once per map
open, resolved `TabList`, `BackTabList`, `MenuTitle`, `WindowList` against the live map scene to see
whether our instantiated clips were bindable from it.

### Why it was retired, and the defect it caused

The F8 path reached the goal without touching the map movie at all. And the two hooks above were
proven, by in-game bisection, to be the cause of the **black map tiles** defect: tile-grid-aligned
black squares that came and went while scrolling or zooming. With the dev-hook cluster installed the
map broke; with only the tag-Execute hooks (`rm2exec`, `po2exec`) installed it was clean.

Leading mechanism, not fully proven: `resolve_detour`'s Task-5 probe does not observe, it CALLS the
engine's resolver four extra times per map open and hands it a **0x80-byte stack buffer whose size was
a guess** - sibling code uses `0x60` for that struct family. An out-write past it corrupts the detour's
own frame precisely while the map's chrome is being bound. **Settle that struct's real size before any
future code calls that resolver.**

Nine other hypotheses were disproven along the way, each by measurement rather than by trying a fix.
Worth knowing so they are not re-litigated:
- charId collision: the worldmap movie (stock AND ERR) defines characters 7..248 and external images
  1..177 + 13500..13506, max 13506. Our icon window never overlapped it. Tool:
  `scratch/scan_gfx_charids.py`. Note tag code **1009** is the external-image define tag in this GFx
  build (body: `u32 characterId, u16 format, dims, length-prefixed name`), not FileAttributes.
- The `own_movie` rebuild of the worldmap: proven byte-clean with `scratch/diff_gfx_tags.py` - 364 tags
  both sides, all 158 image tags identical, stream fully consumed ending on End, only
  `DefineSprite cid=241` (Body) differs by our two inserted placements, and the only differing header
  byte is the FileLength field.
- Depth stomp: Body's own children occupy depths <= 104; our panels sit at 106 and 108.
- Our object count: capping V3 markers at 500 (from ~9530) changed nothing.
- Materializing our children from inside `RemoveObject2::Execute`: with the factory pulse off the
  squares remained.
- The sc2 overlay backend: squares also appear with `overlay_render_mode=surface`.

### Reusable RE facts

- **`RemoveObject2::Execute` (RVA `0x11BDE10`) takes `(tag, ctx)`** - two args. `ctx` is the display
  list holder: base at `ctx+0x28`, count at `ctx+0x30`. The tag's depth is a `u16` at `tag+8`. A third
  argument is not read (the function immediately reuses r8 as scratch).
- **`0x140ee3d10` takes `(this, flag)`**, where the second arg is a BOOL (`mov ebx,edx` then
  `test bl,1`), and it returns `this`. We had it modelled as `(tag, ctx, frame)`, so every diagnostic
  built on `tls_po2_ctx` was recording 0 or 1 as a "context". If you hook it, fix the model first.
- MinHook's 5-byte patch lands on one whole leading instruction in both of the above.
- Dictionary registrar `0x11CF250`: the stable per-movie identity is the **dict owner at
  `[outer+0x38]`**, which is what the function's own prologue computes (`mov rcx,[rcx+0x38]`). Keying
  on the outer pointer makes every lookup miss. Char-def registrar: `0x11169D90` (rcx = registry
  owner, rdx = &charId, r8 = CharacterDef).
- The char-def registry hands out internal ids **above 0xFFFF** (65537+ observed), so never derive an
  injected charId base from a process-wide maximum: charId is a u16 in every place tag we emit.
- `0x74A000` is `SetTextHTML`, which is why HTML colour markup works in native menu captions.

## Experiment 2: the F11 standalone settings menu

Idea: open the game's own settings screen (`02_040_OptionSetting`) on demand, suppress its native tabs,
and put our own rows in it - a native settings UI reachable from anywhere, not just the map.

How it worked:
- Build the screen's job with the movie descriptor `{ u32 8, u8 2, L"02_040_OptionSetting" }` via
  `FUN_1408087e0`, then step that `MenuWindowJob` ourselves with `FUN_1407ad1c0`; push it using the
  same ref-move / push-job primitives the confirm dialog uses.
- Suppress the game's own tabs by hooking the tab-append `FUN_140967b50` while a flag marks the open
  as ours, so only our rows remain; track our dialog so its dtor and an anti-restack guard can find it.
- Over the map the screen hangs off `WorldMapDialog+0xA28` (the child holder found by host scan).

Why it was retired: the F8 path does the same job on a screen we fully understand, with the engine's
own Q/ESC behaviour, and without suppressing native UI. F11 also never got its input ownership right.

**Migration note:** while F11 existed, the F8 form code used the F11 menu as a fallback HOST for when
the map is not open. Removing F11 means that fallback has to be replaced - see
`research_native_menu_screens.md` for the host-selection order the F8 path uses (nest parent, then map
window, then settings dialog, then push onto the active menu with descriptor byte 2).
