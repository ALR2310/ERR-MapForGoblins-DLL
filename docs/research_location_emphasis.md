# Telling "here" from "not here": emphasising the icons of the player's own location

## The problem, in the player's words

A dungeon sits under the overworld, and on the map their icons mix. Working out which icon belongs to
the cave you are standing in currently means hovering each one and reading the label. The wish: every
icon stays visible, always - but it must be obvious at a glance that some of the icons next to the
player actually belong to somewhere else, and equally obvious the other way round once the player is
inside that somewhere else.

Status 2026-07-30: **shipped - size, draw order and colour.** What follows is what was established and
what was built on it, including the two dead ends kept so they are not re-walked.

## Where the player is: SOLVED

`goblin::collected::read_player_map_id(uint32_t &)` (`src/goblin_collected.cpp`) returns the packed
map id, `m{AA}_{BB}_{CC}_{DD}` -> `0xAABBCCDD`.

The ChrIns field group read by `read_player_pos` is not three loose floats: it is a five-field block
`[X, Y, Z, radius, mapId]`, and the map id is the piece that makes the block-local X/Z mean anything -
they are local to THAT block. So it sits one field past the radius:

| field | offset | note |
|---|---|---|
| X / Y / Z | `+0x6C0 / +0x6C4 / +0x6C8` | already used by the hover height |
| radius | `+0x6CC` | |
| **map id** | **`+0x6D0`** | the one this feature uses |
| a second identical block | `+0x6D4 .. +0x6E4` | the chunk/tile one; its id at `+0x6E4` |

Chain: WorldChrMan (AOB slot, `world_chr_man_slot()`) -> `+0x1E508` LocalPlayer -> the block above.
Cross-checked against a maintained CE table whose build has the whole group 0x10 lower (`+0x6B0` there
for the coordinates we read at `+0x6C0`). Under `debug_logging` both candidates are logged side by side
as `[playermap] +0x6D0 = m..._... | +0x6E4 = m..._...` whenever either changes, so a walk from the
surface into a cave settles which is which from real play rather than from the table.

The marker side needed nothing: a WorldMapPointParam row already carries `areaNo/gridXNo/gridZNo`, and
for interiors that IS the map - a cave row is `areaNo=31, gridXNo=<cave index>, gridZNo=0`, i.e.
`m31_XX_00`. Same three numbers on both sides.

Do not confuse `goblin::maphover::map_layer()` (the TAB being looked at) with the player's map id (where
the character stands). This feature is about the second.

## What "my location" means

`v3_same_location` (`src/goblin_stall_probe.cpp`):

- **the whole overworld is ONE location** (area 60, and 61 for the DLC one). A marker two tiles away is
  still "out here with me", and a per-tile rule would flip the emphasis every time the player crossed a
  tile seam;
- **every interior is its own location** (area + gridX + gridZ). A cave, the cave next door and the
  surface above them all read differently from each other.

Consequence worth knowing: standing in Siofra (m12, an underground-layer map) and looking at the
OVERWORLD tab, nothing on screen is "own", so the whole tab draws muted. That is the truth being told
correctly - none of it is where you are - but it is the one case where the emphasis is visible as a
global change rather than a local contrast.

## The three levers

| lever | verdict | where |
|---|---|---|
| **size** | SHIPPED | one factor multiplied into `fx/fy` of `v3_position_child`. The centring pivot is multiplied by the same factor inside that call, so an icon grows and shrinks around its own point instead of walking off it - which is also why nothing else had to change |
| **draw order** | SHIPPED, build-time | the seed sort in `v3_native_merge_snapshot` now orders own-location markers LAST (append = on top), ahead of the row-id z-order contract |
| **alpha / tint** | SHIPPED | a Cxform written into the same render-node data the matrix goes into, through the engine's own `GetWritableData`; found off the matrix, budgeted per frame. Does most of the work in practice |
| swapping the image for a muted one | no | children cannot be recreated after the build burst, and a second child per marker hits the teardown wall (~9200 markers) |

### Size, as built

`V3NativeObject` carries `area/gx/gz` and an `emph` factor; `v3_obj_fx/fy` = the layer's widget scale
times that factor, and every place that positions a child goes through them. The factor is re-decided:

- when a child is created (so a marker is born with it - no one-frame pop);
- on every merge, from the point's own map (which is what makes the focus RINGS follow: a ring is
  re-pointed at a different marker between merges, so its map travels with it);
- whenever the player's map, the two scales, or the on/off change - folded into one `emph_sig`
  compare per map frame, so a toggle flipped in the menu lands on the OPEN map.

Defaults: own `1.05`, other `0.95`, both tunable in the ini (`location_emphasis_own_scale` /
`_other_scale`, clamped to 0.35..2.5) - deliberately small, see the tuning note under the ini table.
Master switch `location_emphasis`, on by default, with a row at the TOP level of the native menu next
to `require_map_fragments`.

### Draw order, as built

Depth is fixed when a child is created and the icon factory only pulses during a build burst, so this
is the one part of the emphasis a BUILD decides rather than a transform. A reopen that reuses the movie
therefore keeps the previous open's order (size still updates live); it re-sorts itself the next time
the map builds from scratch. Live re-ordering would mean detach + indexed re-attach per child, which is
only legal for children we hold a reference on - not the general case.

### Colour: the SDK answered it, the vtable never could

**Read the SDK before RE-ing Scaleform.** `C:\Program Files (x86)\Scaleform\GFx SDK 4.0` is installed
on this machine, with full `Src/`. It says in two lines what a day of vtable dumping could not:

- `GFx_DisplayObject.h:239-241` - `GetCxform` / `SetCxform` / `ConcatenateCxform` are **not virtual**.
  That is the whole reason they are absent from the vtable, and no amount of dumping slots would ever
  have found them.
- `GFx_DisplayObject.cpp:246-256` - they forward to the render node:
  `GetRenderNode()->SetCxform(cx)`.
- `Render_TreeNode.h:159` - `TreeNode::SetCxform` is `GetWritableData(Change_CxForm)->Cx = cx`, and
  `NodeData` is `{ EntryData base; Matrix3F M34; StateBag States; Cxform Cx; RectF x2 }`. So the colour
  is a FIELD of the very node data our matrix already goes into, sitting just past the matrix and the
  state bag. (The SDK reaches that node through `pRenNode`; our children do not have one - see below.)
- `Render_CxForm.h:37` - `float M[4][2]`, `[R,G,B,A][mult, add]`. **This is the one place the SDK
  misleads**: ER stores the Cxform the other way round, four multipliers then four addends - the shape
  the SDK's own `GetAsFloat2x4()` produces (newer Scaleform keeps it that way for SIMD). An untouched
  icon reads `1,1,1,1,0,0,0,0`. Searching for the declared interleaving finds nothing at all, which is
  exactly what the first attempt did. Read the layout off a live dump, do not trust the declaration.
- `Render_Constants.h:105` - `Change_CxForm = 0x2`. This also identifies two exe functions for free:
  the projection setter calls `FUN_141157a70(obj, 0x100000)` and `Change_State_ProjectionMatrix3D` is
  exactly `0x00100000`, so `FUN_141157a70` = `GetWritableData(changeFlags)` and `FUN_141157bd0` =
  `GetReadOnlyData()`. Useful the day a raw write is not enough.

**Our markers have NO pRenNode.** The first attempt wrote into `*(child+0x80)` - the SDK's `pRenNode`,
where the vtable's SetMatrix2D visibly writes - and nothing happened, because a live dump showed that
field is **0** for every marker child. Reading the vtable's GetMatrix again with that in mind shows why
the matrix works anyway: it falls through `+0x80 == 0`, `+0x60 == 0` to a third branch that uses
`child+0x48`.

That field is the `Context::Entry`, and the data behind it is reached by pure arithmetic - Scaleform
packs entries 0x48 bytes apart inside 4K pages starting at `page+0x30`, with the data pointer for
entry i at `*(page+0x20) + 0x28 + i*8` (that is `GetReadOnlyData`, `FUN_141157bd0`, inlined in
`v3_node_data_ro`). The live child confirmed it: entry `0x…198`, `(0x198 - 0x30) / 0x48 = 5` exactly.
`FUN_1411cc040` - the accessor GetMatrix calls on that data - is simply `return data + 0x10`.

The `[v3dump]` of that node data reads straight off as the SDK's own member list, which is how every
offset below is known rather than guessed:

| offset | bytes seen | what |
|---|---|---|
| `+0x10` | `0.5, 0, 0, 114811.4 / 0, 0.5, 0, 59421.9 / 0, 0, 1, 0` | `Matrix3F M34` (3x4) |
| `+0x40` | two null pointers | `StateBag States` |
| `+0x50` | `1,1,1,1` then `0,0,0,0` | **`Cxform Cx`** - multipliers, then addends |
| `+0x70` | `0, 0, 1120, 1240` | `RectF AproxLocalBounds` (a 56x62 icon in twips) |
| `+0x80` | `114811.4, 59421.9, 115370.8, 59993.9` | `RectF AproxParentBounds` = local x the matrix |

So the ER node data is the STOCK SDK layout after all (`EntryData` = vptr + Type + Flags = 0x10). The
earlier "preamble is 0x20 longer" note was an artifact of measuring `pRenNode` instead of this.

Still found, not hardcoded - but **the identity colour transform is not a strong enough anchor to find
it by**. `1,1,1,1,0,0,0,0` is a common run of bytes, and a live node offered two candidates (`+0x50`
and `+0xF0`). The anchor is the MATRIX: we wrote it ourselves, its translation is a pair of large
distinctive twip values, so its byte run appears exactly once. `v3_find_cx_offset` reads the child's
matrix back through the vtable getter, finds that run in the node data, steps `0x30 + 0x10` past it
(Matrix3F's own 12 floats, then the StateBag) and accepts the result only if what sits there IS an
identity. Every identity run it saw over `nodeData+0x10..+0x100` goes in the log either way, as ground
truth for the day the anchor stops matching. More than one matrix hit, a non-identity at the computed
spot, or a missing writable getter all log "not usable", after which `v3_fade` returns 1.0 everywhere
and the feature falls back to size and order. It is retried for up to 60 frames because a freshly
created child's entry is not necessarily hooked up the instant it appears.

Writes go through the real `GetWritableData(entry, Change_CxForm)` (AOB `node_get_writable_data`), not
raw memory: that call does the copy-on-write and registers the change with the context, which is how
the renderer learns anything happened. Reimplementing it would mean reimplementing the snapshot
machinery.

**Where the colour lever may be applied from, and where it may NOT.** Everything that touches a child
for the emphasis lives BELOW the retarget branch in `v3_native_tick`, and that placement is
load-bearing. It first sat at the top of the tick, where it ran on the frame the map re-anchored to a
new movie - i.e. over the previous generation's children, whose blocks the engine had already freed.
The matrix write survives that on its vtable guard; the colour write does not, because it hands a
stale render entry to an ENGINE function that walks and writes through it. The crash log reads
exactly that: `RETARGET parent ... mapClosed=true`, then `3867 of 9454 markers resized`, then
`0xC0000005` inside `eldenring.exe+0x1157AEB` - which is `GetWritableData+0x7B`.

**And it must be spread over frames.** A colour write is not a bare store: `GetWritableData`
copy-on-writes the node data and registers a change with the render context, so re-deciding all ~9500
markers in one frame is thousands of allocations and change records - a multi-second freeze. The tick
re-applies a budgeted slice per frame (`emph_dirty` + `emph_cursor`, 192 writes/frame, counting only
objects whose factor actually changed). The merge no longer re-decides every object either; it handles
only the ones whose MAP changed, which in practice is just the focus rings being re-pointed.

`v3_write_cxform` sets all four multipliers to one `fade` factor with zero addends. Our icons are
PREMULTIPLIED alpha (see the icon pipeline), so scaling colour and alpha by the same number is exactly
a fade - scaling alpha alone would leave the colour too strong. It carries the same dead-generation
vtable guard as the matrix write, and refuses any `child+0x48` that is not exactly on an entry slot
rather than compute a plausible-looking pointer out of a field that turned out to be something else.

**Desaturation is not available, and no amount of trying will make it so.** A Cxform is per-channel
`out = in * mult + add`; greying an icon needs its luminance mixed back into every channel, which is
cross-channel and outside what the structure can express. Scaleform's only cross-channel colour tool
is a ColorMatrix filter, and filters render the object through an offscreen pass - per marker, at
~9500 markers. So the nearest honest thing ships instead: hold blue, drop red and green
(`r = fade * (1 - 0.35 * cool)`, `g = fade * (1 - 0.15 * cool)`), which reads as cold and distant
rather than grey. Addends stay ZERO: the art is premultiplied, so a positive addend lights up the
transparent part of every icon quad, not just the drawn pixels.

ini, all live (they are part of the emphasis signature, so editing one lands on the OPEN map):

| key | default | range |
|---|---|---|
| `location_emphasis_own_scale` | 1.05 | 0.35..2.5 |
| `location_emphasis_other_scale` | 0.95 | 0.35..2.5 |
| `location_emphasis_other_fade` | 0.75 | 0.2..1.0 - never faded into invisibility, nothing is ever hidden |
| `location_emphasis_other_cool` | 0.90 | 0..1 |

Tuned in game 2026-07-30: size does very little of the work and colour does nearly all of it. The
scales ended up near 1.0 (a size difference large enough to read is also large enough to look like a
bug) while the cool shift went almost to full - it separates the two sets at a glance without making
any marker harder to find or click.

### What the vtable turned out to hold (the dead end, kept so it is not re-walked)

The child vtable is `exe+0x2CBA380` (printed at map build as `[v3native] marker child vtable = ...`,
not gated on debug logging, so any log has it). It was dumped with
`VtDumpN.java 0x142CBA380 64 64`, and the guess that the cxform accessors sit next to the matrix ones
is **wrong**:

| slots | what they are |
|---|---|
| 2 / 3 | Get/Set Matrix2D - 8 floats, into the object at `child+0x80` at its `+0x30` |
| 4 / 5 | Get/Set Matrix3D - 12 floats, into the SAME `+0x30` (the storage is a 3x4; SetMatrix2D fills the top two rows and writes a constant third), plus a flag byte at `+0x60` |
| 6 / 7 / 8 | Get/Set/Clear ProjectionMatrix3D - 16 floats (4x4), kept as a ref-counted node property under key `DAT_1445936e8`, node flag bit 12 |
| 9 / 10 / 11 | Get/Set/Clear ViewMatrix3D - 12 floats |
| 22..33+ | a long run of scalar get/set pairs (8-byte fields of a geom struct) - the AS display properties |

No cxform pair anywhere in the first 64 slots - because there is none to find (see above). Nor is there
anything to search by name: Scaleform's display classes carry no C++ RTTI in this exe (`rtti_map.py` on
the vtable returns only the nearest preceding class, and `SymHunt Cxform` finds nothing), and the
identity-cxform byte constant is not in the image either, so there are no xrefs to take. `clip_set_gray`
(`rva_anchors.py`) is not a colour call at all - it is `gotoAndStop("Grayout")`, a frame label our icons
do not have.

A debug-only one-shot `[v3dump]` (debug_logging) still prints `child+0x00..0xBF` and
`*(child+0x80)+0x00..0xBF` as bytes at the first marker build. It is no longer needed to FIND the
colour, but it is the ground truth if the scan ever reports "not identified".

Do NOT swap the icon image instead: children cannot be recreated after the build burst, so a muted
variant frame could only be chosen at build time - and a reopen that reuses the movie would keep the
previous location's choice, which is exactly the case the live path handles.

## Interaction to remember

The de-overlap (`goblin_inject.cpp`) spaces visible markers `kMinDist = 8.0f` world units apart, a
spacing chosen for FULL-SIZE icons. Shrunk markers now pack looser than they need to; nothing looks
wrong yet, but if the scales are pushed further the spacing wants to become per-marker rather than one
constant.
