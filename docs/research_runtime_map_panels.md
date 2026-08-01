# The map panels on a build whose map movie was replaced

## The problem, measured

The mod's two map panels - `MfgTip` (the height readout) and `MfgBanner` (the focus banner) - are a
SECOND placement of the movie's own tooltip sprite, added by rewriting `02_120_worldmap.gfx` while it
loads (`goblin_own_movie.cpp`). That rewrite requires seeing the movie's bytes.

Measured on Convergence 3.0.1.2 (2026-07-29), with every `.gfx` the opener is handed logged: our hook
(`CS::CSScaleformFileOpener::OpenFile`) saw **90 movies, every one of them named `data0:/...`**, and of
the **22 movies Convergence overrides in `mod\menu\`, exactly 0** - not just `02_120_worldmap.gfx` but
also `02_122`, `02_020_inventory`, `05_000_title` and the rest. The rule is clean: a movie the mod does
not override reaches that opener; one it overrides never does.

**It is the loader, not the override.** The tempting conclusion - "a mod that ships its own 02_120
cannot be transformed" - is wrong: ERR ships its own `menu/02_120_worldmap.gfx` too (69696 bytes) and
the panels work there. ERR runs under ModEngine2, which substitutes the bytes BELOW the engine's own
opener, so the engine still calls it with a `data0:` name and merely receives modded bytes - which we
see and can rewrite. Convergence 3.x runs under ModEngine3, whose VFS serves an overridden file without
that call happening at all. On the same build `02_160_keyconfiguration.gfx` (the menu screen's host) is
NOT overridden, comes from `data0:`, and our transform of it works as usual.

Result on that build: icons work (they are injected during the PARSE, a different hook), the highlight
works, and both map panels were absent.

Three ways to get the panels anyway:

1. **Chase the loader's route** - find where ME3 serves an overridden movie and rewrite the bytes
   there. Rejected: it makes a visible feature depend on the internals of a third-party loader, its
   next version puts us back here, and it would point our byte-level rewriter at a movie authored by
   someone else, which only holds while their tag layout matches what the rewriter expects.
2. **Inject into the PARSED structures**, the way the icons already do on this very build (a frame
   appended to sprite 171 during the parse) - loader-independent by construction. For a panel that
   means getting a placement into `Body`'s own frame, which is not something we have done: the probe
   for it is described at the end of this file and was never established.
3. **Use the instance the movie already has** - what was built. Costs nothing in loader knowledge and
   needs only `Body/PlaceName` to exist, which is true of anything that is still the map screen.

## What the replaced movie actually contains

`scratch/dump_movie_structure.py` on Convergence's own file next to the vanilla one (both read-only,
`scratch/conv3_02_120_structure.md` vs `scratch/vanilla_02_120_structure.md`): **32 diff lines**, all
of them extra icons (sprite 171: 906 frames vs 348) and seven extra external images. Every named clip
is identical, including `Body/PlaceName` - the game's own name popup - and the sprite behind it, whose
`State_0` holds eight line clips at x 17, y -39 + 35.9 * i.

So the panels do not need to be created: the movie already has an instance of exactly the sprite they
are made of. It belongs to the game, and the game only ever fills its top lines.

## How the game drives that popup (FUN_14087A8E0, the function we already hook)

```
no pin (or pin invisible):   FUN_140735A60(panel+8, 0)          hide the popup
                             FUN_1407353B0(line0+0x70, "")      clear the first line's text
with a pin:                  FUN_140735A60(panel+8, 1)          show it
                             FUN_1409CC470(mapArea, &p, pinPos) project the pin
                             clamp x against mapArea+0x340/+0x348 with the panel's own shoulders
                             FUN_1407356E0(panel+8, &p)         place it
                             per line i: FUN_140735A60(line_i, i < pin.lineCount)
                                         FUN_1407353B0(line_i+0x70, pin.line(i))
```

Two facts out of this decide the whole design:

1. **The game never positions the lines.** Their placement is authored and its per-frame routine only
   ever sets each line's text and visibility. A position we give a line therefore stands.
2. **The popup remembers where it is.** `FUN_1407356E0` stores the position it is about to apply at
   `wrapper+0x78` and skips the write when it matches, and the popup's wrapper is `panel + 8`. So
   `panel+0x80` IS the popup's live position, in the same space our own panel is placed in (Body's),
   with the game's clamping already folded in - no projection of ours to drift.

## What was built

Both panels borrow lines of the game's popup when our own clip is absent (`goblin_maphover.cpp`):

- the height line takes the LAST line, the banner the three before it - the ones a place name never
  reaches (the map's own pins carry one or two);
- each borrowed line is placed AGAINST the popup: `line = target - popupPos`, so it stands still at
  our corner while the name above keeps following the marker. Without that subtraction the height line
  rode the popup down to the reticle, which is what the first version did;
- the banner also shows the popup back (the game hides it on every frame with nothing to name) and,
  when nothing has been hovered since the map opened, gives it a position of its own;
- our writes moved to AFTER the original call, since the game rewrites those lines every frame.

### The wrapper trap

A path lookup hands back a PLAIN proxy; the popup and its lines are a RICH wrapper that embeds one at
+8 and remembers what it last applied (visibility at +0x69, position at +0x78). Its setters skip a
write that matches that memory. So hiding one of the game's own lines through the inner proxy would
leave the memory lying, and the game's own next "show this line" would be dropped as redundant - the
map would stop naming places. Anything of the game's is therefore touched with `FUN_140735A60` /
`FUN_1407356E0` on the wrapper itself (`kPanelVisible` / `kPanelPosF`), never with the proxy
primitives.

## The panels in the PARSED movie (the loader-independent route)

Built second, in `goblin_gfx_probe.cpp`, and it is the one that makes the borrowed lines a fallback
rather than the answer. Same ground the icon frames already stand on: the parser has just built the
movie's structures, and a placement added to them plays exactly like an authored one.

Nothing is assumed about the movie, which is the point. The host identifies ITSELF:

| what we need | where it comes from |
|---|---|
| the host sprite | the one whose frame places a child named `PlaceName` (the clip the GAME names places with, so it exists wherever the map works) |
| the character to place | that same placement's character id - our panels are a second placement of it |
| a free depth | the highest depth in that frame, + 2 and + 4 |

Reading those out of a live tag means decoding the placement body at `tag+8`: flags, depth, character
id, then a bit-packed MATRIX and colour transform to step over, then the instance name.
`PlaceObject2` and `PlaceObject3` differ by one flags byte, so both readings are tried and the one
whose name comes out printable wins - which keeps this independent of the tag vtables (they move on a
game update; the body layout does not).

`scratch/check_panel_decode.py` is that decoder ported back to Python and run over every 02_120 on
this machine (vanilla, ERR, Convergence, Reborn): all four report host sprite 241, character 225, max
depth 94. It earned its keep immediately - the first C++ version read depth and character id through
the bit reader, and they are ordinary little-endian u16 BYTE fields, so it reported character 57600
for 225 and depth 24064 for 94. That would have placed a nonexistent character at a nonsense depth in
game with nothing to explain why.

Two safety rules, both learned the hard way earlier in this codebase:

- **the tag list is ours to replace, the frame array is not.** The per-frame tag-pointer array and the
  tag objects are arena-owned in stock Scaleform and the engine frees neither (`Frame::DestroyTags`
  only runs each tag's non-deleting destructor, nothing releases `pTagPtrList`), so a longer array from
  the game's own malloc is safe and the old one is left where it lies. The FRAME array is the one
  `~SpriteDef` frees through `MemoryHeap::Free`, and this route never touches it.
- **only with a CAPTURED tag vtable.** The RVA fallback is an address from another game version, and
  a wrong vtable in a frame the engine PLAYS is a call through whatever happens to sit there - not a
  guarded read that degrades into a missing feature. Without the capture the panels are left to the
  load-time transform.

If the load-time transform already placed `MfgTip` (a build whose file we did see), the scan finds that
name and adds nothing, so the two routes cannot both place a panel.

## The MENU screen took the same lesson, by a different route

The map's panels are one placement of a character the movie already has, so they could go into the
PARSED structures. The menu screen (`02_160_keyconfiguration`) cannot: its transform adds new
CHARACTERS - the icon bitmaps and the strip sprite - rebuilds the row clip, re-spaces the rows and
renames captions. That is a byte transform, and it used to run on the engine's own movie opener, which
is exactly the call ME3 makes unnecessary for a file it overrides.

So the byte transform stayed and the PLACE it runs moved: to the movie tag loop
(`FUN_141169590`, AOB-registered as `gfx_tag_loop`), which every movie goes through whoever served the
file. On entry the header is parsed and no tag is read yet. The transform reads the movie through the
Scaleform File interface - `GetLength` at vtable+0x38, `Read` at +0x50, `Seek` at +0x70 - so a blob
from an archive and a stream from disk read the same, rebuilds the bytes, and points the source at
them. `MFG_MOVIE_ROUTE=file` goes back to the opener, as a way to compare the two on a live build.

Two defects came out of that move, both of the same shape - a number the engine had already computed
from the ORIGINAL movie and that our replacement had to stay consistent with:

1. **The stream bound** (`ctx+0x2c4`, the loop's `position < bound`). It is not the file's size: a
   55600-byte movie gives 55584. Setting it to our buffer's size handed the parser 16 bytes it should
   never see and it read the movie's own trailer as a tag. Fixed by moving the bound by the same DELTA
   the movie grew, which keeps whatever convention the engine used.
2. **The source position.** Rewinding the swapped source to 0 was the real crash. The reader had
   already taken the header out of that source and reads ahead into a window, so the moment the window
   ran out the parser went on reading from byte 0 and parsed the header as a tag. The movie came out
   wrong, the screen built from it never got a display object, and the engine's own walk over that
   screen's children read through null - `ChildVisitor` at `FUN_140d84850`, an AV reading address 0,
   reached from our menu update. Fixed by preserving the position.

What the reader has already buffered stays the ORIGINAL movie's bytes. That is sound rather than lucky:
every edit that early is same-length (a caption, a matrix, a font size) and moves no tag boundary,
while the insertions all sit inside sprite defines - measured, the first one is at offset 37589 in
`02_160` against a 16KB window, and the live log prints the `read ahead` (520 bytes in both builds
tested) so the day that margin disappears is visible rather than silent.

## Not needed any more

The earlier plan - synthesize a named `PlaceObject3` for a second tooltip placement at runtime, the
way the 9459 native markers are created - is not being pursued for these two panels. The probe it
needed (does a context whose sprite is `Body` reach the RM2 executor? `note_context` in
`goblin_gfx_probe.cpp`, `debug_logging` only) stays in the build as an instrument: the contexts it
logged on Convergence carried no readable sprite id at any of the offsets tried, so that route was
never established. It is the fallback if a future build turns out to ship a movie without a
`Body/PlaceName` at all.
