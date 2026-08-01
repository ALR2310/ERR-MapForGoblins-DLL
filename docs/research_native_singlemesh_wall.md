# Lever A (native single-mesh markers): findings, the wall, and revival options

Status (2026-07-18): **PARKED.** The single-mesh idea is architecturally sound and its
core was proven at runtime, but drawing our injected icons through the Scaleform
DrawingContext hit a hard GPU-texture wall (below). The shipped performance endgame is
**B+C** (viewport-windowed attach + bulk self-detach), which reaches ~34-48ms close-freeze
on the Steam Deck (near the engine's own floor). The probe code has been removed from the
source; this doc preserves everything useful.

Detailed RE reports live in `scratch/endgame_research/leverA_*.md` (gitignored):
`lever_A_single_mesh.md`, `lever_A_p0_spec.md`, `lever_A_holder_resolution.md`,
`lever_A_texture_creation.md`, `leverA_child_to_texture.md`, `leverA_build_own_texture.md`,
`leverA_datadef_lookup.md`, `leverA_image_singleton.md`, `leverA_code_review.md`.

## The idea
Instead of one Scaleform display object PER marker (~9459 children, each an eager
`TreeCacheNode` -> O(n) close-teardown = the ~75-150ms close freeze), draw ALL icons into
ONE display object's GFx DrawingContext (MovieClip.graphics / beginBitmapFill+drawRect) =
ONE shape = ONE TreeCacheNode = ONE mesh. Close cost collapses to the engine floor at any
zoom, with no pop-in (unlike B), and the same tech doubles as a base for native UI (V4).

## What is PROVEN (runtime-confirmed)
- One host Sprite child = exactly +1 TreeCacheNode. Place a DefineSprite charId (charId 171)
  via a PlaceObject3 tag with flags1=0x00 (NOT 0x10/HasImage), materialize it through the
  V3 factory slot (the PlaceObject capture hook grabs the real MovieClip by depth+charId),
  then attach it. parentCount rose by exactly 1.
- The MovieClip's DrawingContext is obtained via display-object vtable slot **+0x2a0**
  (getter FUN_141136bb0; mints a 0xd8-byte ctx, links into renderer ctx list @owner+0x5320).
- **Attach FIRST, then get the ctx**: a fresh sprite's ctx has a null allocator (ctx+0x28)
  until it is attached into a live movie's render tree. Getting/using the ctx before attach
  faults inside beginBitmapFill's allocator call.
- Init the shape with **shape-reset FUN_14119d0c0**, NOT begin (FUN_14119cb50) - begin
  returns 0 on a fresh ctx (accumulator missing) and the fill then faults. shape-reset
  allocates a fresh ShapeData at ctx+0x38.
- DrawingContext primitives (all RE'd + callable): begin FUN_14119cb50, beginBitmapFill
  FUN_14119cd80 (rcx=ctx, edx=mode 0x40 clamp/0x41 repeat/0x42 no-wrap, r8=holder,
  r9=Matrix2x4 float[8]), moveTo FUN_14119d690, lineTo FUN_14119d7a0, endFill FUN_14119d650,
  shapeReset FUN_14119d0c0. Per quad: beginBitmapFill + moveTo + 4x lineTo + endFill (twips
  = px*20, DAT_142a02f80). Vertex cap 65535/shape (FUN_1411f11b0) -> shard by layer x
  category (~75 shapes) for the full 9459.
- Fill matrix (maps texture->local): {Wq/Wimg,0,mx,0, 0,Hq/Himg,my,0}; identity = native
  size at origin.

## THE WALL: beginBitmapFill needs a CREATED GPU texture we cannot obtain
beginBitmapFill's `holder+0x18` must be a **created GPU texture image**: `CS::CSTextureImage`
(vtable RVA 0x2bb8910) or `Scaleform::Render::TextureImage` (0x2bb8838), IsImageCreated != 0.
For our runtime-injected DefineBitsLossless2 icons we could NOT get one. Every path failed
(do NOT re-try these blind):

1. **child -> ImageShapeCharacterDef (0x2ccd388) -> ShapeData -> fillRec+0x10**: that def is
   loadMovie-ONLY (ctor FUN_141250f80, callers are all import/SWF-bind handlers). Our
   in-movie PlaceObject3(HasImage) children never have it (XREF-exhaustive). The scan finds
   nothing.
2. **Capture at injection from the MovieDataDef**: charId lives in the hash table
   `*(dataDef+0x180)` (dataDef=*(loaderCtx+0x38); entries @+0x10 stride 0x20: key u32 @+0x08,
   type u32 @+0x10, value u64 @+0x18; hash=((cid>>8)^cid)&mask). type==1 -> value is a bind
   INDEX into the +0x130 list (node: binding @+0x08, bindIndex u32 @+0x10, next @+0x18);
   binding (vt 0x2cc8200) +0x10 = the image. **BUT that image is a `Render::RawImage`
   (vt 0x2cbf680, CPU, IsImageCreated==2), NOT a GPU texture.** beginBitmapFill crashes with
   it (a partial fill corrupts render state -> delayed crash).
3. **Scaleform ImageCreator CreateImage** (creator=drawCtx+0x30 vt 0x2bbb3a0, +0x20 =
   FUN_141161970) on the RawImage -> returns 0 (RawImage is not a valid source; the proxy
   ScaleformImageResource 0x2bb79a0 has a return-0 IsImageCreated + a field-getter at +0x60,
   which is what faulted at exe+0x119CFC2 in the earliest attempts).
4. **FromSoft image singleton DAT_143d82510 / FUN_140d63e50** (the path the game's own icon
   drawer FUN_140d81640 uses): NAME-keyed (L"MENU_ItemIcon_%05d" etc.) against a separate FD4
   texture repository DAT_143d73e58. Orthogonal to our charIds - no charId->name chain, our
   bitmaps are not in that repo.
5. **CS TextureManager minter FUN_140d650b0** (via provider=*(singleton+0x78)) on the
   RawImage -> FAULTS (SEH-caught) even with a valid empty-DLString name arg. singleton +
   provider read fine; the minter itself rejects/faults on our RawImage.

**Root cause:** our injected icons only become GPU-resident inside the engine's OWN
sprite-171 frame render; that GPU texture is not reachable or mintable from outside that
render flow. The DrawingContext/beginBitmapFill API is therefore a dead end for our
runtime-injected bitmaps.

## Crash lessons (paid for; keep)
- Any per-charId capture/lookup MUST be flag-gated - running it unconditionally in
  `inject_all_icons` once hung the load thread (a full hashmap scan, mask up to ~1M) and
  crashed the game on map open.
- NEVER full-scan a game hashmap by mask; use the hash+chain path, bounded.
- Drawing with a non-GPU image (RawImage) corrupts render state -> delayed crash on the
  next frame (outside our SEH). Validate the image vtable is a real TextureImage first.
- All raw reads go through SEH-safe rq/rd32 (safe_copy); bad offsets then return 0, not crash.

## Revival options (if ever resumed) - ranked
1. **The one untested angle:** let the engine RENDER an icon once (via a normal sprite-171
   instance / a marker), so it mints the GPU CSTextureImage itself, then find WHERE the
   sprite-frame render caches that texture and BY WHAT KEY (charId? the RawImage ptr? a
   render-resource handle?). If a key->CSTextureImage cache exists post-render, capture it and
   feed holder+0x18. This is the only path not yet closed; the cache + key are unknown and
   need fresh RE (likely in the Renderer2D/MeshCache/HAL texture-upload path, or a
   TextureManager cache keyed by the source image pointer).
2. **Fallback shape-char (Q2 of lever_A_single_mesh.md):** build our OWN
   ImageShapeCharacterDef via FUN_141250f80 (the ctor that DOES mint the texture correctly) +
   register a synthetic charId + place ONE instance per icon. Caveat: this is PER-ICON (one
   shape char each), so it does NOT achieve single-node - it would only reduce, not collapse,
   the node count. Probably not worth it over B+C.
3. **Ship our icons pre-created as GPU textures** by a different injection that goes through
   the created-texture path the game trusts (unknown; likely needs a DefineExternalImage /
   image-import route rather than DefineBitsLossless2). Speculative.

## Bottom line
B+C is the perf endgame (near engine floor, no crashes, no pop-in for C). Single-mesh's
extra ~14ms + no-pop-in did not justify the texture wall. Effort redirected to V4 (native
UI), where the DrawingContext primitives + attach/materialize machinery RE'd here may still
be reused for custom native rendering.
