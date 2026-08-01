#include "goblin_gfx_probe.hpp"

#include "goblin_guarded.hpp" // engine calls that fault by design stay out of the crash log
#include "generated_shared/goblin_menu_icon_tags.hpp"

#include "goblin_config.hpp"
#include "goblin_messages.hpp"

#include "goblin_build_variants.hpp" // MFG_STALL_PROFILER gates the hardware write watch
#if MFG_STALL_PROFILER
namespace goblin::watch { void pump(); }
#endif
#include "goblin_build_variants.hpp"
#include "generated_shared/goblin_map_icons.hpp" // one DefineBitsLossless2 tag per custom map icon
#include "generated_shared/goblin_logo.hpp"       // runtime-injectable MapForGoblins logo (bitmap + matrix)
#include "generated/goblin_item_icons.hpp"       // goblin::generated::ANON_ICON_ID
#include "goblin_inject.hpp"   // goblin::remap_injected_icons (point markers at our injected frames)
#include "goblin_maphover.hpp" // map_dialog() - the V3 spike only fires while the map is open
#include "goblin_stall_probe.hpp"
// goblin_settings_textures_dev.hpp (5,260,706 bytes) and goblin_settings_insert_dev.hpp (113,808)
// were included here unconditionally for the retired re-host experiment. No symbol from either
// namespace is referenced anywhere in src/ or tools/, and neither reaches the DLL (both blobs are
// `inline const unsigned char` arrays, so their COMDATs are discarded) - but the file still parsed
// 5.4 MB of them on every profile build. The headers stay in the tree as the experiment's data.
#include "modutils.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Path A (runtime icon-frame injection) instrumentation + PoC injector.
//
// We hook the GAME's OWN GFx SpriteDef CONSTRUCTOR (0x1411be1f0): it hands us the real
// SpriteDef `this` for every DefineSprite as the worldmap movie loads. The ctor hook only
// catches sprite 171 if our DLL loads BEFORE the movie - the offline/ME loader (shipping
// path) loads at process start and catches it; the dev injector loads too late. We ring-buffer
// the pointers and, once the worldmap movie finished loading (frameCount populated), find the
// one with charId@+0x18==171.
//
// LIVE-CONFIRMED SpriteDef layout (this build): charId@+0x18, frameCount@+0x30, vtable
// 0x142CC2E58; sealed frame GArray ON the SpriteDef: data@+0x38, count@+0x40, cap@+0x48; frame
// element stride 0x10 = {ExecuteTag** tags@+0, u32 tagCount@+8}. The tag objects are immutable
// display instructions, so a frame can SHARE another frame's tag list (shallow clone).
//
// Icon injection is UNCONDITIONAL: we register our DefineBitsLossless2 bitmaps in the live worldmap
// movie, append a frame per icon to SpriteDef-171, and remap markers to the injected frames - so icons
// render without a modified/shipped gfx. This is the mod's normal icon path (no ini toggle).
//   debug_logging (ini, default off) - logging only. It does NOT arm the RM2::Execute hook: that one
//                              is armed by the kNativeMarkers build variant and carries the marker
//                              factory pulse, so it is live in every shipping build. The SpriteDef/dict
//                              dumps additionally require the compile key MFG_DUMP_FRAMES, which is not
//                              defined anywhere in the tree. Never changes injection behavior.
// See docs/research_no_gfx_icons.md.
namespace
{
    using CtorFn = void *(void *, void *); // rcx = new SpriteDef `this`, rdx = CDEF (load ctx)
    CtorFn *o_ctor = nullptr;

    // AvmSprite::AddDisplayObject (0x140f01b10) used to be hooked here to capture movieDef. Removed
    // 2026-07-28: it never fired (AS2 path; this worldmap movie is AS3). Its real signature, worth
    // keeping because anyone hooking it again will get it wrong, is `this` + EIGHT arguments
    // (SDK Src/GFx/AS2/AS2_AvmSprite.cpp:410): (CharPosInfo*, ASString*, ArrayLH<SwfEvent*>*,
    // const void* initSource, unsigned createFrame, unsigned long addFlags,
    // CharacterCreateInfo* createOverride, InteractiveObject* origChar). The last five arrive on the
    // stack and the callee both READS and WRITES those slots, so a short detour hands it our own frame.
    std::atomic<uint64_t> g_moviedef{0};

    // Resource-dict lookup 0x14113fd90: rcx = container (= movieDef+0xd8), edx = charId. The most
    // reliable movieDef source - every charId resolution funnels here (both AddDisplayObject and the
    // 2nd caller). movieDef = rcx - 0xd8.
    using LookupFn = void *(void *, uint32_t, void *);
    LookupFn *o_lookup = nullptr;

    // R2 diagnostics: lossless image loader 0x1411e2670 (rcx = movieContext, rdx = tagInfo).
    using LosslessFn = void *(void *, void *);
    LosslessFn *o_lossless = nullptr;

    // DefineSprite loader 0x1411e2bc0 (rcx = movieContext). We hook it to inject our embedded "?"
    // bitmap by calling the NATIVE lossless loader during the worldmap load (when the load context +
    // image manager are valid) - feeding it our tag bytes via the live reader. Then charId 13507 is
    // registered natively into both dict containers, and we just place it in frames post-load.
    using SpriteLoaderFn = void *(void *, void *);
    SpriteLoaderFn *o_spriteloader = nullptr;
    std::atomic<bool> g_qmark_injected{false}; // frames appended (per worldmap load)
    // A g_img_registered flag stood here ("our bitmaps registered (at native-13507 load)"). It had a
    // single occurrence - this declaration - and its own trailing note had gone stale besides:
    // registration runs at sprite-171 load now, not at 13507 (see lossless_detour).

    // srcIconId (the gfx iconId a category's markers are baked with) -> our injected iconId. Filled at
    // worldmap load; read by remap_injected_icons. srcIconIds are small (369..441) so a flat array is fine.
    constexpr int ICON_MAP_SIZE = 1024;
    uint32_t g_icon_iid[ICON_MAP_SIZE] = {0};

    void inject_all_icons(uint64_t ctx);      // defined below; called from lossless_detour (13507 moment)
    bool inject_logo_into_plaque(uint64_t sd); // defined below; called from spriteloader_detour (sprite 246)

    // DIAGNOSTIC: hook RemoveObject2::Execute (0x1411bcce0) to see what happens when OUR injected RM2
    // tags run - does it find the display entry at the depth, and is that entry sticky ([entry+0x71]&2)?
    // rcx=this(tag), rdx=ctx(display container; list base@+0x28, count@+0x30, node depth@+0x14), r8d=frame.
    using ExecFn = void *(void *, void *, uint32_t);
    ExecFn *o_rm2exec = nullptr;
    std::atomic<uint64_t> g_my_rm2_d1{0}, g_my_rm2_d2{0};
    std::atomic<unsigned> g_rmhook_logs{0};
    // A g_in_inject re-entrancy flag lived here. It guarded a PO2-loader detour against recursing
    // while we drove that loader ourselves - and that detour was never installed (it went with the
    // re-host cluster). Nothing ever LOADED the flag; it was only stored to.

    // ── Runtime injected-charId base (no build-time / per-profile guess) ───────────────────────────
    // The common resource-dict registrar 0x1411cf250 runs for EVERY native charId as a movie streams in
    // (rcx = movieDef/load ctx, r8 = &charId u32; confirmed: registrar derives dictOwner = [rcx+0x38],
    // the same dictOwner the DefineSprite loader uses from [loadctx+0x38] -> rcx == loadctx == movieDef).
    // We hook it to track each movie's real max NATIVE charId at runtime, SCOPED BY ctx (not global), so
    // our injected bitmaps sit above every native id no matter which/whose gfx actually loaded. This is
    // why no per-profile bake is needed and external gfx edits can't break us.
    // (The registrar is reached by AOB, not by this RVA - a constexpr RVA_FN_DICT_REGISTRAR = 0x11CF250
    //  sat here and no expression read it. The address is already in the prose above and at the scan.)
    using RegistrarFn = void *(void *, void *, void *, void *);
    RegistrarFn *o_registrar = nullptr;
    constexpr int REGN = 16;                    // a few movies can be loading/resident at once
    std::atomic<uint64_t> g_reg_ctx[REGN] = {};
    std::atomic<uint32_t> g_reg_max[REGN] = {};
    constexpr uint32_t CHARID_SANE_CEIL = 0xF0000; // ignore absurd ids (garbage, not a real charId)
    constexpr uint32_t INJECT_BASE_MARGIN = 64;    // headroom over the observed native max
    // Max charId seen across EVERY movie: the last-resort input when the per-movie key misses (the
    // registrar and the DefineSprite loader turned out not to always hand us the same outer object).
    // Over-estimating only pushes our base higher, which is always safe; under-estimating is not.
    std::atomic<uint32_t> g_reg_gmax{0};

    uint64_t rq(uint64_t a); // defined below (with the other guarded readers)
    // Both the registrar and the sprite loader derive the real dict owner as [outer+0x38] (the registrar's
    // own prologue is `mov rcx,[rcx+0x38]`), so THAT is the stable per-movie identity. Keying on the outer
    // pointer made every lookup miss (logged reg_max=0 on every load).
    uint64_t dict_owner(uint64_t outer)
    {
        uint64_t d = rq(outer + 0x38);
        return d ? d : outer;
    }

    void reg_note(uint64_t ctx, uint32_t cid) // cheap; called very frequently during movie load
    {
        if (!ctx || cid == 0 || cid >= CHARID_SANE_CEIL) return;
        uint32_t g = g_reg_gmax.load(std::memory_order_relaxed);
        while (cid > g && !g_reg_gmax.compare_exchange_weak(g, cid, std::memory_order_relaxed)) {}
        int slot = -1;
        for (int i = 0; i < REGN; ++i)
        {
            uint64_t c = g_reg_ctx[i].load(std::memory_order_relaxed);
            if (c == ctx)
            {
                uint32_t m = g_reg_max[i].load(std::memory_order_relaxed);
                while (cid > m && !g_reg_max[i].compare_exchange_weak(m, cid, std::memory_order_relaxed)) {}
                return;
            }
            if (c == 0 && slot < 0) slot = i;
        }
        if (slot >= 0)
        {
            uint64_t expect = 0;
            if (g_reg_ctx[slot].compare_exchange_strong(expect, ctx, std::memory_order_relaxed))
                g_reg_max[slot].store(cid, std::memory_order_relaxed);
            else
                reg_note(ctx, cid); // slot was taken meanwhile (rare); retry
        }
    }
    uint32_t reg_max_for(uint64_t ctx) // accepts either the outer load ctx or the dict owner itself
    {
        uint64_t own = dict_owner(ctx);
        for (int i = 0; i < REGN; ++i)
        {
            uint64_t c = g_reg_ctx[i].load(std::memory_order_relaxed);
            if (c && (c == ctx || c == own))
                return g_reg_max[i].load(std::memory_order_relaxed);
        }
        return 0;
    }

    // Injected-bitmap base charId, computed live at the 13507 inject moment (0 until then). When unset
    // (hook never fired / non-worldmap path) we fall back to the generated compile-time constant.
    std::atomic<uint32_t> g_inject_base{0};
    uint32_t inject_base()
    {
        uint32_t b = g_inject_base.load(std::memory_order_relaxed);
        return b ? b : (uint32_t)goblin::generated::MAP_ICON_CHARID_BASE;
    }
    // Lowest / highest 1-based frame id appended this worldmap load (for the remap overlap diagnostic).
    std::atomic<uint32_t> g_iid_lo{0};
    std::atomic<uint32_t> g_iid_hi{0};

    // ---- Heap-safe allocation for Scaleform-OWNED buffers ----
    // Two DIFFERENT ownership rules apply to what we append into a live DefineSprite, and getting them
    // the wrong way round is how the v2.0.4 heap corruption happened. Audited from the binary 2026-07-28:
    //
    //  * The FRAME ARRAY (SpriteDef+0x38) is owned by SCALEFORM'S OWN HEAP, not the CRT. ~SpriteDef
    //    (0x1411be250) frees it as `rcx = *(0x144593250); rdx = [this+0x38]; call [rcx_vt+0x60]` -
    //    MemoryHeap::Free - and the array's capacity helper (0x1411bfb00) allocs/reallocs through the
    //    same object (+0x70 AllocAutoHeap, +0x58 Realloc). The SDK agrees: SpriteDef::Playlist is
    //    `ArrayLH<Frame, StatMD_Other_Mem>` (Src/GFx/GFx_SpriteDef.h:71) and AllocatorBaseLH routes
    //    through Memory::AllocAutoHeap / Memory::Free (Src/Kernel/SF_Allocator.h:133-166). So a
    //    replacement array must come from gfx_heap_alloc below. A CRT pointer handed to MemoryHeap::Free
    //    is a foreign pointer - the heap resolves its bookkeeping from the pointer itself.
    //  * The synthesized TAG OBJECTS and the per-frame tag-pointer arrays are never freed by the engine
    //    at all: Frame::DestroyTags (0x1412449b0) only runs each tag's non-deleting destructor
    //    (SDK Src/GFx/GFx_CharacterDef.cpp:46), and nothing releases pTagPtrList - in stock Scaleform
    //    they are arena-owned. Those stay on the game's CRT malloc (gfx_alloc): the allocator is
    //    irrelevant because no free ever happens, and they cost a few KB per worldmap load.
    //
    // gfx_alloc uses the GAME's own _malloc_base (resolved by AOB) rather than ours, so that anything
    // which does reach a CRT free reaches the right _crtheap. Returns null if unresolved, and callers
    // abort injection rather than guess.
    using GameMallocFn = void *(*)(size_t);
    std::atomic<uint64_t> g_game_malloc{0};

    void resolve_game_malloc()
    {
        if (g_game_malloc.load(std::memory_order_relaxed)) return;
        try
        {
            void *m = modutils::scan<void>(
                {.aob = "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 77 ?? 48 85 C9 B8 01 00 00 00 48 0F 44 D8 EB ?? E8 ?? ?? ?? ?? 85 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0"});
            g_game_malloc.store((uint64_t)m, std::memory_order_relaxed);
            spdlog::info("[icons] frame buffer source ready @ 0x{:X}", (uint64_t)m);
        }
        catch (const std::exception &e)
        {
            spdlog::error("[icons] frame buffer source unavailable ({}); icon setup skipped", e.what());
        }
    }

    // The Scaleform global MemoryHeap: *(exe + 0x4593250), Alloc(size, align/stat) at vtable +0x50 (the
    // same call goblin_sfimage.cpp uses for its image resource). Use this for anything the engine will
    // FREE - today that is the frame array and nothing else.
    void *gfx_heap_alloc(size_t n)
    {
        __try
        {
            auto sane = [](uint64_t v) { return v > 0x10000ull && v < 0x7FFFFFFFFFFFull; };
            const uint64_t hptr = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)) + 0x4593250;
            const uint64_t heap = *reinterpret_cast<uint64_t *>(hptr);
            if (!sane(heap))
                return nullptr;
            const uint64_t hvt = *reinterpret_cast<uint64_t *>(heap);
            if (!sane(hvt))
                return nullptr;
            using AllocFn = void *(void *heap, size_t size, void *stat);
            void *p = (*reinterpret_cast<AllocFn **>(hvt + 0x50))(reinterpret_cast<void *>(heap), n,
                                                                  nullptr);
            if (p)
                memset(p, 0, n);
            return p;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // Allocate a buffer on the GAME's CRT heap (zeroed, matching the old VirtualAlloc behaviour).
    // Only for buffers the engine never frees - see the ownership note above.
    // Returns null if the game allocator is unresolved -> caller must abort injection.
    void *gfx_alloc(size_t n)
    {
        uint64_t m = g_game_malloc.load(std::memory_order_relaxed);
        if (!m) return nullptr;
        void *p = reinterpret_cast<GameMallocFn>(m)(n);
        if (p) memset(p, 0, n);
        return p;
    }

    // On-map MapForGoblins logo. The gfx baked it into the decorative-plaque sprite 246 (re-pointing its
    // char-10 PlaceObject to a logo bitmap-fill shape, scale 0.38 / translate -243). At runtime we instead
    // register the logo bitmap at charId = inject_base()+MAP_ICON_TAG_COUNT (right after the icons, at the
    // 13507 moment) and re-point sprite 246's char-10 PO3 to it with the baked LOGO_MATRIX. Sprite 246
    // loads AFTER 13507 (gfx tag-stream order: 13507 #175 -> sprite246 #291), so the base/charId are ready.
    constexpr uint32_t LOGO_PLAQUE_SPRITE = 246;
    constexpr uint16_t LOGO_PLAQUE_CHAR = 10;
    std::atomic<uint32_t> g_logo_charid{0};
    std::atomic<bool> g_logo_placed{false};
    std::atomic<uint64_t> g_worldmap_ctx{0}; // the worldmap movie's load ctx (captured at its 13507)
    uint64_t g_native_place_tags[ICON_MAP_SIZE]{};
    constexpr uint32_t SPRITE171_RM2_CAP = 4096;
    uint64_t g_sprite171_rm2_tags[SPRITE171_RM2_CAP]{};
    std::atomic<uint32_t> g_sprite171_rm2_count{0};
    uint32_t logo_charid() { return inject_base() + (uint32_t)goblin::generated::MAP_ICON_TAG_COUNT; }

    // The Path-A milestone watches (g_ms_charid / g_ms_reg_seen / g_ms_reg_rcx / g_ms_dictchecked /
    // g_ms_watch_cid / g_ms_watch_hits) lived here. They existed to confirm that our injected charIds
    // reached the char dict during the settings re-host experiment. None of them was ever assigned a
    // watch target, so every branch reading them was unreachable.

    // HISTORY. The Path-A settings re-host experiment kept its state here: a "MASTER OFF-SWITCH"
    // (ENABLE_SETTINGS_REHOST), a route selector (NATIVE_SPLICE), and ~15 globals holding the map root,
    // the placement contexts, the registrar owner and the Task #4 placement guards. None of it was
    // wired to anything by the end - the off-switch was read by NO expression, so it did not switch
    // anything either. The experiment is documented (and its hazard recorded) in
    // docs/research_retired_native_ui_experiments.md: with the splice on, every worldmap load had
    // 40149 bytes of settings defs pushed into the buffer the engine parses, and map TILES went
    // transparent during scroll/zoom. The resolver/PO2-loader detours that caused it are gone, and
    // so is the rest of the cluster (see the Path A/Path B tombstone further down - that removal is
    // what finally emptied this). Three thread_locals stood here with it: tls_po2_ctx and
    // tls_po2_frame (the ctx/frame of the PlaceObject2::Execute running on this thread) and
    // tls_in_probe (a re-entrancy guard for the resolver probe). Each had exactly one occurrence in
    // the file - its own declaration.

    void *registrar_detour(void *rcx, void *rdx, void *r8, void *r9)
    {
        if (r8)
        {
            uint32_t cid = *reinterpret_cast<const uint32_t *>(r8); // registrar itself derefs [r8]
            reg_note(dict_owner((uint64_t)rcx), cid); // key on the dict owner, not the outer pointer
            // A milestone-watch branch stood here: when the registrar fired for a charId held in
            // g_ms_charid it recorded the owner. Nothing ever wrote that charId, so the branch was
            // unreachable, and the two atomics it set had no readers either.
        }
        return o_registrar(rcx, rdx, r8, r9);
    }

    // A Path A diagnostic on the CHAR-DEF registrar 0x11169D90 stood here (which owner object native
    // worldmap char defs and our injected shape land in). Its typedef and trampoline were declared and
    // never assigned - no hook was installed - and the question it asked was answered: we do share the
    // movie's real char registry, which is why the icon injection works at all.

    // XInput gamepad injection: hook XInputGetState (game reads XINPUT1_4) and OR in the buttons we
    // want, driven by sentinel file scratch\pad.cmd ("<hexmask> <frames>"). Bypasses ER's injected-
    // KEYBOARD filter because it's the game's OWN XInput poll returning our state.

    constexpr int RING = 8192;
    void *g_sprites[RING] = {nullptr};
    std::atomic<unsigned> g_idx{0};
    std::atomic<uint64_t> g_found_sd{0}; // located SpriteDef-171 (0 until found)
    std::atomic<bool> g_dict_dumped{false};

    // Resource dict READ container (movieDef+0xd8) - live-confirmed via AddDisplayObject disasm:
    //   [+0x00]=slot array base, [+0x08]=count; slot stride 16 (slot[0]=node ptr); node charId@+0x2c.
    constexpr unsigned OFF_MOVIEDEF_DICT = 0xd8;
    constexpr unsigned OFF_NODE_CHARID = 0x2c;

    // SpriteDef offsets (live-confirmed, this build).
    constexpr unsigned OFF_CHARID = 0x18;
    constexpr unsigned OFF_FRAMECOUNT = 0x30;
    constexpr unsigned OFF_FRAMEARR_DATA = 0x38;
    constexpr unsigned OFF_FRAMEARR_COUNT = 0x40;
    constexpr unsigned OFF_FRAMEARR_CAP = 0x48;
    constexpr unsigned FRAME_STRIDE = 0x10;
    // FALLBACK base charId for our injected bitmaps. The PRIMARY base is computed at RUNTIME from the
    // live native max charId of the actual worldmap movie (registrar hook -> reg_max_for -> inject_base),
    // so it survives any external gfx change with no per-profile bake. This generated constant is used
    // only if the registrar hook never observed the movie's ctx (inject_base() falls back to it).
    constexpr uint32_t INJECT_CHARID_BASE = goblin::generated::MAP_ICON_CHARID_BASE;
    // eldenring.exe RVAs of the Scaleform ExecuteTag vtables (proven via the SWF tag-loader dispatch +
    // each Execute's disassembly). The previous build had RM2 and PO2 SWAPPED, which is why our "removes"
    // were really PlaceObject2 re-places that never cleared anything. Correct mapping:
    //   RemoveObject2 vtable 0x142C65D80, Execute 0x1411bde10 - depth = bare u16 @body+0 (tag+8), NO flags
    //                                     byte, no short/long form; on a depth match it UNLINKS the node.
    //   PlaceObject2  vtable 0x142C65CC0, Execute 0x1411bcce0 - flags@body+0, depth u16 @body+1 (place).
    //   PlaceObject3  vtable 0x142C65D20, Execute 0x1411bdb40 - flags@body+0, depth u16 @body+2 (place).
    //   RemoveObject(tag5) vtable 0x142C65DD0 shares Execute 0x1411bde10 (also depth u16 @+8).
    // RM2 object is only 0x10 bytes {vtable@+0; depth u16 @+8}; the frame executor (0x1411bf131) dispatches
    // purely on tag[0]=vtable with no validity/heap check, so a VirtualAlloc'd synthesized tag runs like a
    // native one. See reference_scaleform_displaylist.
    constexpr uint64_t RVA_VT_REMOVEOBJECT2 = 0x2C65D80;     // FALLBACK only (vtables are captured live)
    constexpr uint64_t RVA_VT_PLACEOBJECT3 = 0x2C65D20;      // FALLBACK only
    // NAMING, corrected 2026-07-28: the slot we use at vtable +0x30 is NOT ExecuteTag::Execute. It is
    // AddToTimelineSnapshot(TimelineSnapshot *, unsigned frame) - the real Execute is slot 1 (+0x08) and
    // takes only (this, DisplayObjContainer *). Everything in this file that says "Execute" for +0x30,
    // and every parameter called "ctx" on that path, is really the SNAPSHOT builder and a
    // TimelineSnapshot; the "nodes" it searches are SnapshotElements (linked via +0x00 = pPrev, which is
    // why treating one as an object with a vtable at +0 is an arbitrary-pointer call). The mechanism we
    // rely on is unaffected - our synthesized tags are fed to the same snapshot builder the engine uses -
    // but the names cost hours when they lie, so they are being corrected as each site is touched.
    constexpr uint64_t RVA_FN_REMOVEOBJECT2_ADDSNAPSHOT = 0x11BDE10; // RemoveObject2::AddToTimelineSnapshot
    constexpr uint64_t RVA_FN_PLACEOBJECT3_ADDSNAPSHOT = 0x11BDB40;  // PlaceObject3, same slot

    // Native tag vtables captured LIVE from the stock sprite-171 frames at load (a composite icon frame
    // is RM2+RM2+PO3). This avoids hardcoding the vtable addresses - the RVAs above are used only if the
    // capture fails. Set by capture_tag_vtables(); read by build_remove_tag / build_clean_place_tag.
    std::atomic<uint64_t> g_po3_vt{0};
    std::atomic<uint64_t> g_rm2_vt{0};
    uint64_t po3_vtable()
    {
        uint64_t v = g_po3_vt.load(std::memory_order_relaxed);
        return v ? v : (uint64_t)GetModuleHandleW(nullptr) + RVA_VT_PLACEOBJECT3;
    }
    uint64_t rm2_vtable()
    {
        uint64_t v = g_rm2_vt.load(std::memory_order_relaxed);
        return v ? v : (uint64_t)GetModuleHandleW(nullptr) + RVA_VT_REMOVEOBJECT2;
    }

    bool safe_copy(void *dst, const void *src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Upper bound is the x64 user-mode max (~128TB). The old 0x7FF000000000 bound REJECTED the game's
    // heap under ME2/ME3, which sits at 0x7FF2_xxxxxxxx (higher than ERR's heap) - that silently broke
    // every pointer check (sprite-171 locate, the whole icon injection), so icons rendered default under
    // ME2/ME3 while ERR (lower heap) worked. Confirmed live: a valid SpriteDef self=0x7FF2D5F8D060 was
    // rejected; widening this single bound made the existing load-time injection work under ME2. 2026-06-25.
    bool looks_heap(uint64_t v) { return v > 0x10000ull && v < 0x7FFFFFFFFFFFull; }
    uint64_t rq(uint64_t a) { uint64_t v = 0; return (a && safe_copy(&v, (void *)a, 8)) ? v : 0; }
    uint32_t rd32(uint64_t a) { uint32_t v = 0; return (a && safe_copy(&v, (void *)a, 4)) ? v : 0; }
    bool wr32(uint64_t a, uint32_t v) { return a && safe_copy((void *)a, &v, 4); }
    bool wr64(uint64_t a, uint64_t v) { return a && safe_copy((void *)a, &v, 8); }

#ifdef MFG_DUMP_FRAMES
    void dump_obj(const char *label, uint64_t addr)
    {
        unsigned char buf[0xC0];
        if (!addr || !safe_copy(buf, (void *)addr, sizeof(buf)))
        {
            spdlog::debug("[gfxprobe] {} @ 0x{:X} <unreadable>", label, addr);
            return;
        }
        spdlog::debug("[gfxprobe] ---- {} @ 0x{:X} ----", label, addr);
        for (size_t r = 0; r < sizeof(buf); r += 8)
        {
            uint64_t v;
            memcpy(&v, buf + r, 8);
            spdlog::debug("[gfxprobe]   +0x{:02X}: {:016X}", (unsigned)r, v);
        }
    }

    // Find a charId's binding node in the read dict (movieDef+0xd8) and dump it + its resource, so we
    // can compare a gfx-native image (13507) vs our runtime-injected one (20000) side by side.
    void dump_charid_node(uint64_t movieDef, uint32_t charId, const char *label)
    {
        uint64_t cont = movieDef + OFF_MOVIEDEF_DICT;
        uint64_t base = rq(cont);
        uint32_t count = rd32(cont + 8);
        if (!looks_heap(base) || count == 0 || count > 100000)
            return;
        uint64_t node = 0;
        for (uint32_t k = 0; k < count; ++k)
        {
            uint64_t n = rq(base + (uint64_t)k * 16);
            if (n && rd32(n + OFF_NODE_CHARID) == charId) { node = n; break; }
        }
        if (!node)
        {
            spdlog::debug("[cmp] {} charId {} NOT in dict.", label, charId);
            return;
        }
        spdlog::debug("[cmp] {} charId {} node@0x{:X}:", label, charId, node);
        for (unsigned off = 0; off < 0x60; off += 8)
        {
            uint64_t v = rq(node + off);
            spdlog::debug("[cmp]     +0x{:02X}: {:016X}{}", off, v, looks_heap(v) ? " <mem>" : "");
        }
    }
#endif // MFG_DUMP_FRAMES

    void *ctor_detour(void *self, void *cdef)
    {
        unsigned i = g_idx.fetch_add(1, std::memory_order_relaxed);
        g_sprites[i % RING] = self;
        return o_ctor(self, cdef);
    }

    // V3 stage 1: one-shot gate for the top-down layer walk in tick() (below).
    std::atomic<int> g_layer_dumped{0}; // reset on map close, walk once per open


    void *lossless_detour(void *rcx, void *rdx)
    {
        // Diagnostic only. The registration of OUR bitmaps NO LONGER happens here: it depended on the
        // gfx containing an embedded image (our "?" charId 13507) to land in a "warm image-manager"
        // moment, but a stock/overhaul gfx may carry ZERO embedded bitmaps (confirmed: this loader fired
        // 0 times on a pure-DLL run). Registration now runs at sprite-171 load instead (the image manager
        // is already warm there from the worldmap's external-image loads), which is gfx-content-independent.
        // (a call counter and a captured context were recorded here for dump_r2, which never
        // printed them; the detour itself stays because o_lossless is the pass-through)
        return o_lossless(rcx, rdx);
    }

    // dump_r2() stood here. Its comment promised a dump of the global image service and the
    // lossless-image state, but the body only read six values into locals and dropped them -
    // there was no spdlog call, no store and no return. It ran once per session for nothing.

    uint64_t build_clean_place_tag(uint16_t cid, uint16_t dp, const unsigned char *mtx, unsigned mlen); // below
    uint64_t build_remove_tag(uint16_t depth);                                // defined below
    void capture_tag_vtables(uint64_t sd);                                    // defined below
    void capture_sprite171_rm2_tags(uint64_t sd);                              // defined below
    uint32_t append_icon_frame(uint64_t sd, uint16_t newCharId, const unsigned char *mat, unsigned matLen); // defined below
    uint32_t compute_safe_base(uint64_t movieDef, uint32_t count);            // defined below (self-healing)
#ifdef MFG_DUMP_FRAMES
    void dump_frames(uint64_t sd);                                 // defined below
#endif

    // SEH-isolated call into the native lossless loader (kept object-free for __try/__except).
    void *seh_call_lossless(void *ctx, void *taginfo)
    {
        void *res = nullptr;
        ++goblin::guarded::depth;
        __try { res = o_lossless(ctx, taginfo); }
        __except (EXCEPTION_EXECUTE_HANDLER) { res = nullptr; }
        --goblin::guarded::depth;
        return res;
    }


    // Peek the charId u16 at the head of the current tag, without consuming the reader.
    uint32_t peek_charid(uint64_t ctx)
    {
        uint64_t reader = rq(ctx + 0x418);
        if (!reader)
            reader = ctx + 0x50;
        uint32_t pos = rd32(reader + 0x4c);
        uint64_t buf = rq(reader + 0x60);
        if (!looks_heap(buf))
            return 0xFFFFFFFF;
        uint32_t lo = 0, hi = 0;
        if (!safe_copy(&lo, (void *)(buf + pos), 1) || !safe_copy(&hi, (void *)(buf + pos + 1), 1))
            return 0xFFFFFFFF;
        return lo | (hi << 8);
    }

    // Inject ONE DefineBitsLossless2 tag (an embedded bitmap) under `charId`, by driving the NATIVE
    // lossless loader with a borrowed reader pointed at our tag bytes. MUST run right after a native
    // lossless load (image-manager state fresh - any other moment builds a NULL pixel descriptor and
    // crashes). The loader STREAMS pixels from the MemoryFile source (reader+0x20), so we clone the
    // native MemoryFile and point it at our bytes. Returns true on a non-null loader result.
    bool inject_lossless_tag(uint64_t ctx, const unsigned char *bytes, unsigned len, uint16_t charId)
    {
        static unsigned char *buf = nullptr; // reused writable copy (grown to the largest tag)
        static unsigned bufcap = 0;
        if (bufcap < len)
        {
            if (buf) VirtualFree(buf, 0, MEM_RELEASE);
            buf = (unsigned char *)VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            bufcap = buf ? len : 0;
        }
        if (!buf) return false;
        memcpy(buf, bytes, len);
        memcpy(buf, &charId, 2); // charId is the first u16 of the tag body

        uint64_t reader = rq(ctx + 0x418);
        if (!reader) reader = ctx + 0x50;
        uint64_t srcMF = rq(reader + 0x20);
        if (!looks_heap(srcMF)) return false;
        static uint64_t myMF = 0;
        if (!myMF)
        {
            void *mf = VirtualAlloc(nullptr, 0x48, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!mf || !safe_copy(mf, (void *)srcMF, 0x48)) return false;
            myMF = (uint64_t)mf;
        }
        wr64(myMF + 0x18, (uint64_t)buf); // data buffer = our tag
        wr32(myMF + 0x20, len);           // size
        wr32(myMF + 0x24, 0);             // pos = 0

        // Save native reader state + window CONTENT (our refill overwrites the shared read window),
        // restore after. The window buffer is at reader+0x60 with capacity reader+0x68 (default 0x200;
        // when there is no file source the buffer is INLINE at reader+0x6c, still 0x200). The game streams
        // refills into that buffer IN PLACE and never reallocates it (confirmed in the refill FUN_1411bba10),
        // so the buffer pointer/capacity stay put - we only have to restore its contents. We MUST copy
        // exactly `capacity` bytes: a fixed larger size reads/writes PAST the buffer into adjacent memory
        // (the reader's own tail when the buffer is inline, or neighbor heap), and because our lossless
        // load allocates in between, the write-back stamps a stale snapshot over live neighbor heap ->
        // heap corruption (the ME2 startup-parse crashes: ntdll heap fast-fails / null derefs in the SWF
        // parser after we return). Also save the +0x54 stream counter and +0x58 refill flag the loader moves.
        uint64_t winbuf = rq(reader + 0x60);
        uint32_t wincap = rd32(reader + 0x68);
        static unsigned char winsave[0x4000];
        if (wincap > sizeof(winsave))
        {
            spdlog::warn("[icons] reader window cap 0x{:X} exceeds snapshot buffer; clamping.", wincap);
            wincap = sizeof(winsave);
        }
        bool havewin = winbuf && wincap && safe_copy(winsave, (void *)winbuf, wincap);
        uint64_t s20 = rq(reader + 0x20);
        uint32_t s4c = rd32(reader + 0x4c), s50 = rd32(reader + 0x50), s54 = rd32(reader + 0x54);
        uint8_t s58 = (uint8_t)rd32(reader + 0x58);

        wr64(reader + 0x20, myMF);
        wr32(reader + 0x4c, 0); wr32(reader + 0x50, 0); wr32(reader + 0x54, 0);
        uint32_t taginfo[8] = {36u, 0, len, 0, 0, 0, 0, 0}; // [+0]=type36 (DefineBitsLossless2), [+8]=length

        void *lret = seh_call_lossless((void *)ctx, taginfo);

        wr64(reader + 0x20, s20);
        wr32(reader + 0x4c, s4c); wr32(reader + 0x50, s50); wr32(reader + 0x54, s54);
        safe_copy((void *)(reader + 0x58), &s58, 1);
        if (havewin) safe_copy((void *)winbuf, winsave, wincap);
        return lret != nullptr;
    }

    // ── PATH A / PATH B (re-host the settings menu inside the map movie): REMOVED 2026-07-31 ──
    // The whole tag-injection half of that experiment lived here and had no callers left:
    //   inject_full_tag()     drove the game's own per-tag loader over one hand-built SWF tag
    //   resolve_tag_loader()  indexed the two native loader tables (0x3b7fd10 / 0x3b7fff0)
    //   ensure_stream_fns()   AOB-scanned ReadTagInfo + tag-align at first use
    //   capture_seh()         SEH filter storing the faulting instruction/data address
    //   dict_node_for()       scanned a movieDef's read-dict for a charId
    // plus g_readtaginfo / g_tagalign / g_stream_fns_tried, g_seh_at / g_seh_data / g_seh_code,
    // g_root_tags_injected and the Po2LoaderFn typedef with o_po2loader (never assigned - that
    // hook was never installed).
    //
    // Proof it was unreachable rather than merely unused: the only literal the cluster could ever
    // print, "[rehost] stream fns", is absent from the built DLL, and no `#if` or CMake exclusion
    // covers this region (the file's only preprocessor key is MFG_DUMP_FRAMES). It cost two AOB
    // scans at first use and carried a VirtualAlloc + PAGE_READWRITE tag buffer.
    //
    // What it established is not lost - it is the ancestry of the two routes that DO ship:
    // inject_lossless_tag above (map-icon bitmaps into the live worldmap movie) and the parse-side
    // panel injection. The rest is written up in docs/research_retired_native_ui_experiments.md.
    // The dropped constant ROOT_INJECT_COUNT left two dangling comment lines here as well; they
    // went with it.

    // Register ALL embedded map-icon bitmaps (charId BASE+i) at the proven 13507 moment.
    void inject_all_icons(uint64_t ctx)
    {
        int ok = 0;
        uint32_t base = inject_base();
        for (int i = 0; i < goblin::generated::MAP_ICON_TAG_COUNT; ++i)
        {
            const auto &e = goblin::generated::MAP_ICON_TAGS[i];
            if (inject_lossless_tag(ctx, e.tag, e.tagLen, (uint16_t)(base + i)))
            {
                ++ok;
            }
        }
        spdlog::info("[icons] registered {}/{} bitmaps (charId {}..{}).", ok,
                     goblin::generated::MAP_ICON_TAG_COUNT, base,
                     base + goblin::generated::MAP_ICON_TAG_COUNT - 1);

        // Register the MapForGoblins logo bitmap right after the icons (same fresh-manager moment).
        uint32_t lcid = logo_charid();
        if (inject_lossless_tag(ctx, goblin::generated::LOGO_TAG, goblin::generated::LOGO_TAG_LEN, (uint16_t)lcid))
        {
            g_logo_charid.store(lcid, std::memory_order_relaxed);
            spdlog::info("[logo] registered logo bitmap at charId {}.", lcid);
        }
        else
            spdlog::warn("[logo] logo bitmap registration FAILED (charId {}).", lcid);

    }

    // SEH-isolated worldmap sprite-171 injection (POD-only locals so __try is legal): register our
    // bitmaps, append a frame per icon, remap markers. This runs for EVERY user on every worldmap load
    // (injection is unconditional), so a fault here - a wrong struct offset on some game/overhaul version,
    // a hostile movie, etc. - must NEVER crash the game. On AV we log and bail; the native loader already
    // ran (called before this), so the map still works, just without our icons that load.
    void seh_inject_sprite171(uint64_t sd, uint64_t ctx, uint32_t fcnt)
    {
        __try
        {
            capture_tag_vtables(sd); // grab native PO3/RM2 vtables from stock frames (no hardcode)
            // Compute the charId base self-healingly (high floor, raised above any live charId, window
            // verified clear). Register our bitmaps (image manager is warm from the worldmap's external
            // images), then append a frame per icon and remap markers to the injected iconIds.
            uint32_t base = compute_safe_base(ctx, (uint32_t)goblin::generated::MAP_ICON_TAG_COUNT + 1);
            g_inject_base.store(base, std::memory_order_relaxed);
            // PlaceObject tags embed the bitmap charId, whose collision-safe
            // base is recomputed for every loaded movie.
            memset(g_native_place_tags, 0, sizeof(g_native_place_tags));
            // A new worldmap movie is loading, so whatever movieDef we latched belongs to the
            // PREVIOUS one and must not be dereferenced again (tick() walks its resource dict).
            // Cleared here as well as at map close, because a movie can be re-loaded without a
            // close ever running.
            g_moviedef.store(0, std::memory_order_relaxed);
            g_dict_dumped.store(false, std::memory_order_relaxed);
            g_worldmap_ctx.store(ctx, std::memory_order_relaxed); // scope sprite-246 logo to THIS movie
            uint64_t sub = rq(ctx + 0x18);
            uint64_t mgr = sub ? rq(sub + 0x40) : 0;
            spdlog::info("[icons] sprite-171 load: safe base={} (floor {}); reg_max={}; "
                         "img-manager [[ctx+0x18]+0x40]=0x{:X} (must be non-null to register).",
                         base, INJECT_CHARID_BASE, reg_max_for(ctx), mgr);
            inject_all_icons(ctx); // registers all icon bitmaps + the logo bitmap
            spdlog::info("[icons] worldmap sprite-171 (frameCount={}) loading; appending {} icon frames.",
                         fcnt, goblin::generated::MAP_ICON_TAG_COUNT);
            for (int k = 0; k < ICON_MAP_SIZE; ++k) g_icon_iid[k] = 0;
            int placed = 0;
            uint32_t iid_lo = 0, iid_hi = 0;
            for (int i = 0; i < goblin::generated::MAP_ICON_TAG_COUNT; ++i)
            {
                const goblin::generated::MapIconTag e = goblin::generated::MAP_ICON_TAGS[i]; // POD copy
                uint32_t iid = append_icon_frame(sd, (uint16_t)(inject_base() + i), e.matrix, e.matrixLen);
                if (iid && e.srcIconId >= 0 && e.srcIconId < ICON_MAP_SIZE)
                {
                    g_icon_iid[e.srcIconId] = iid;
                    if (!iid_lo || iid < iid_lo) iid_lo = iid;
                    if (iid > iid_hi) iid_hi = iid;
                    ++placed;
                }
            }
            capture_sprite171_rm2_tags(sd);
            g_iid_lo.store(iid_lo, std::memory_order_relaxed);
            g_iid_hi.store(iid_hi, std::memory_order_relaxed);
            // (The appended-frame-id vs srcIconId overlap check that used to run here computed its
            // answer and threw it away - its log line was removed during antivirus string sanitizing.
            // The invariant it watched still holds by construction: iids are appended above the
            // worldmap's frame count and srcIconIds are 369..433.)
#ifdef MFG_DUMP_FRAMES
            dump_frames(sd); // DEV-only (compile-time gated): live tag layout of key frames
#endif

            // The sprite pointer rides along because it reveals which heap region the game handed
            // us - that is what identified the looks_heap bound bug in 2026-06, and it used to be
            // carried only by the status registry.
            spdlog::info("[icons] appended {} frames (base charId {}, sprite 0x{:X}); "
                         "remapping markers.", placed, base, sd);
            goblin::remap_injected_icons(); // point markers at our added iconIds (before pins built)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::error("[icons] EXCEPTION during sprite-171 icon load - icons may be incomplete this "
                          "load; game kept alive.");
        }
    }

    // SEH-isolated logo re-point (POD-only). Same all-users rationale.
    bool seh_inject_logo(uint64_t sd)
    {
        __try { return inject_logo_into_plaque(sd); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uint64_t locate_sprite171();  // fwd decl (defined below): worldmap sprite-171 by charId+frameCount

    // A g_movie_swapped counter for the retired Path B buffer swap stood here with a four-line note
    // about validating that mechanism with an identity copy. Single occurrence, and the mechanism it
    // belonged to left with the Path A/Path B cluster.
    // ── Category icons for the IN-GAME MENU rows ─────────────────────────────────────
    // The menu row clip (DefineSprite cid 189 of 02_160_KeyConfiguration) has no image
    // child, and an image registered into another movie is not reachable from this one -
    // so our icons must become part of THIS movie. Same linchpin as the world map: while
    // the movie is still parsing its defines, hand the parser an EXTENDED copy of its own
    // bytes. We splice immediately BEFORE the row clip's tag:
    //     orig[0 .. tag189) + ICON_BLOB (bitmaps + a 64-frame icon sprite) + ROW_TAG + rest
    // ROW_TAG is the row clip rebuilt with our icon sprite placed as a named child
    // ("MfgIcon"), so the row keeps its charId and everything else about it.
    // Generated offline by scratch/gen_menu_icons_insert.py.
    // The splice state (g_menu_icons_spliced / g_menu_icons_ctx) and buffer_is_menu_movie() lived
    // here. The movie test survives as content_is_menu() in goblin_own_movie.cpp - the same search
    // for the "02_160_KeyConfiguration" marker - and that copy is the one with a caller.

    // find_top_level_sprite_tag() lived here: it walked a movie buffer to the byte range of a
    // top-level DefineSprite with a given charId, for the retired icon splice below.

    // The first version of this crashed the game (2026-07-25 17:56 dump): it re-pointed the
    // row's existing CursorLock placement at our icon sprite, but CursorLock has a child of
    // its own named "Lock" and something resolves the path "CursorLock/Lock" - after the swap
    // that path yielded null and the engine's child-visitor dereferenced it. The generator now
    // adds OUR OWN named child ("MfgIcon", depth 6) and leaves every original placement alone;
    // the extended buffer was re-validated offline (all 266 defs parse, CursorLock intact).
    // OFF, and staying off. Two in-game runs crashed identically (dumps eldenring.exe.28348
    // and .34424): AV inside the engine's child visitor (VisitMembers) on a null interface,
    // with the dialog CTOR on the stack, before any row was ever drawn. The first run
    // re-pointed an existing placement (CursorLock) and the second only ADDED a child of our
    // own - the crash is the same either way, so the trigger is modifying THIS movie at all,
    // not the particular edit. The extended buffer parses cleanly offline, so the damage is
    // semantic: our natively-parsed DefineBitsLossless2 characters are not fully usable in
    // this movie, and the row clip's instantiation trips over them.
    // What actually ships instead: goblin_own_movie.cpp rebuilds the menu movie ON THE PARSE and
    // splices the icon characters in there. Unconditionally - there is no per-open bracket and
    // there cannot be one: 02_160 is parsed ONCE, in the startup preload, and every screen after
    // that instances that single parse (goblin_own_movie.hpp says why at length).
    // (An earlier note here said goblin_sfimage.cpp draws our pixels into a runtime-created clip -
    // that route was disabled too, and its code has since been removed.) The generator and its
    // offline self-check remain in scratch/gen_menu_icons_insert.py as fallback material.
    // The kEnableMenuIconSplice flag that used to sit here gated nothing: no code read it.



    // ── which movie is this? ─────────────────────────────────────────────────────────
    // Cached offset of the movie definition's file-name pointer, discovered on first use.
    std::atomic<int> g_url_off{-1};

    // A movie name is a short asset path; the buffer is ours so nothing downstream reads game memory.
    constexpr size_t kUrlMax = 192;

    // Copy a candidate name out of the definition into our own buffer, then judge the COPY.
    // Every read is guarded on purpose: looks_heap only says "this number could be a pointer", and
    // the discovery pass below tries about a hundred of them, so one raw dereference here would be a
    // crash waiting for the first object layout we did not anticipate.
    // Scaleform's String does not point AT the characters: it points at a reference-counted header
    // (size + refcount) with the characters behind it, and the pointer can carry flag bits in its low
    // bits. So a candidate is tried both as a plain char* and at the handful of offsets a header of
    // that shape puts the text at - which is why the first version, testing only offset 0, found
    // nothing and quietly fell back to the sprite-shape heuristic.
    const int kCharsAt[] = {0, 0x08, 0x0C, 0x10};

    bool read_chars_at(uint64_t p, char *out, size_t cap);

    bool read_name_at(uint64_t movieDef, int off, char *out, size_t cap)
    {
        uint64_t p = rq(movieDef + (uint32_t)off);
        if (!looks_heap(p))
            return false;
        p &= ~3ull; // drop any flag bits kept in the low two bits
        for (int k = 0; k < (int)(sizeof(kCharsAt) / sizeof(kCharsAt[0])); ++k)
            if (read_chars_at(p + (uint32_t)kCharsAt[k], out, cap))
                return true;
        return false;
    }

    bool read_chars_at(uint64_t p, char *out, size_t cap)
    {
        if (!looks_heap(p))
            return false;
        // Shrink on failure rather than giving up: a perfectly valid short string can sit near the end
        // of a page, where a full-length read would fault on the next one.
        size_t got = 0;
        for (size_t want = cap - 1; want >= 16; want /= 2)
            if (safe_copy(out, (const void *)p, want))
            {
                got = want;
                break;
            }
        if (!got)
            return false;
        out[got] = 0;
        size_t i = 0;
        for (; i < got; ++i)
        {
            const unsigned char c = (unsigned char)out[i];
            if (c == 0)
                break;
            if (c < 0x20 || c > 0x7E) // a path is printable ASCII; anything else is another field
                return false;
        }
        if (i < 5 || i >= got) // no terminator inside the copy means this is not a short path
            return false;
        out[i] = 0;
        const char *dot = std::strrchr(out, '.');
        if (!dot)
            return false;
        return _stricmp(dot, ".gfx") == 0 || _stricmp(dot, ".swf") == 0;
    }

    // Read a movie's own file name through its definition. false = it could not be established, and
    // callers must then fall back to whatever they did before.
    bool movie_file_url(uint64_t movieDef, char *out, size_t cap)
    {
        if (!looks_heap(movieDef) || cap < 32)
            return false;
        const int known = g_url_off.load(std::memory_order_relaxed);
        if (known >= 0 && read_name_at(movieDef, known, out, cap))
            return true;
        // Discovery pass: the field's offset is not hardcoded, because this SDK build may lay the
        // object out differently than the published headers. One hit is enough to cache.
        for (int off = 0; off <= 0x300; off += 8)
            if (read_name_at(movieDef, off, out, cap))
            {
                if (g_url_off.exchange(off, std::memory_order_relaxed) != off)
                    spdlog::info("[gfxprobe] movie name found at def+0x{:X}: '{}'", off, out);
                return true;
            }
        return false;
    }

    bool name_contains(const char *hay, const char *needle)
    {
        const size_t n = std::strlen(needle);
        for (const char *p = hay; *p; ++p)
            if (_strnicmp(p, needle, n) == 0)
                return true;
        return false;
    }

    // ── is the discovered field really the movie's own name? ─────────────────────────
    // The offset is found by pattern, so it could in principle latch onto some unrelated path that
    // happens to sit in the object. Letting such a field say "this is NOT the worldmap" would delete
    // the icons for everyone, so a name may only rule a movie out after the field has PROVEN it varies
    // between movies - which a constant or mis-latched field never will. Until then the name can only
    // confirm, and everything falls back to the sprite-shape heuristic exactly as before.
    char g_seen_name[kUrlMax] = {};
    std::atomic<int> g_seen_set{0};
    std::atomic<int> g_name_varies{0};

    void note_name(const char *url)
    {
        if (g_name_varies.load(std::memory_order_relaxed))
            return;
        if (g_seen_set.exchange(1, std::memory_order_relaxed) == 0)
        {
            strncpy_s(g_seen_name, sizeof(g_seen_name), url, _TRUNCATE);
            return;
        }
        if (_stricmp(g_seen_name, url) != 0)
        {
            g_name_varies.store(1, std::memory_order_relaxed);
            spdlog::info("[gfxprobe] movie-name field verified: it differs between movies "
                         "('{}' vs '{}'), so a name may now rule a movie out", g_seen_name, url);
        }
    }

    // 1 = this movie IS the worldmap by its own name, 0 = the name did not match, -1 = no name.
    int ctx_is_worldmap(uint64_t ctx)
    {
        uint64_t movieDef = rq(ctx + 0x38);
        if (!looks_heap(movieDef))
            movieDef = ctx; // pre-0x38 layout, same fallback compute_safe_base uses
        char url[kUrlMax] = {};
        if (!movie_file_url(movieDef, url, sizeof(url)))
            return -1;
        note_name(url);
        return name_contains(url, "02_120_worldmap") ? 1 : 0;
    }

    // ══ the map's own panels, added to the PARSED movie ══════════════════════════════
    // The mod's two map panels (the height readout and the focus banner) are a SECOND placement of the
    // clip the game itself uses to name a place. The load-time byte transform that adds them
    // (goblin_own_movie.cpp) needs to see the movie's bytes, and whether it does is the LOADER's
    // decision: ModEngine2 substitutes an overridden file below the engine's own opener, so the call
    // still happens and we see modded bytes; ModEngine3 serves an overridden movie without that call at
    // all (measured on Convergence: of the 22 movies it overrides, our opener saw 0, while all 90 it does
    // not override came through). So the placement is added here instead, to the structures the parser
    // has just built - the same ground the icon frames already stand on, and independent of any loader.
    //
    // Nothing below is assumed about the movie. The host sprite identifies ITSELF: the clip the game
    // names places by is called `PlaceName`, so the sprite whose frame places a child under that name is
    // the one to add ours to, that child's character is the sprite to place, and the depths in that frame
    // say what depth is free. A movie the game can still show a place name on therefore carries
    // everything this needs, whoever authored it.
    constexpr const char *kHostChildName = "PlaceName";  // the game's own tooltip clip
    constexpr const char *kTipChildName = "MfgTip";
    constexpr const char *kBannerChildName = "MfgBanner";
    // Parked far off screen. The panels are positioned by goblin_maphover on every frame they are shown,
    // and this keeps them from appearing at the clip's own origin for the frame before the first hover.
    // The same six bytes the load-time transform parks with: MATRIX {HasScale 0, HasRotate 0,
    // nTranslateBits 20, tx = ty = -400000 twips}.
    const unsigned char kParkedMatrix[6] = {0x29, 0x3C, 0xB0, 0x13, 0xCB, 0x00};

    std::atomic<int> g_panels_state{0};        // 0 = not done, 1 = placed by us, 2 = already in the movie
    std::atomic<unsigned> g_panel_scan_at{0};  // ring cursor, so each SpriteDef is examined once
    // The ring holds every movie's sprites. The cursor starts at the sprite the worldmap movie was
    // loading when we first got here, so nothing another movie defined can be mistaken for the host.
    std::atomic<int> g_panel_scan_init{0};

    uint64_t build_named_place_tag(uint16_t charId, uint16_t depth, const unsigned char *matrix,
                                   unsigned matLen, const char *name);  // defined below

    // ── just enough SWF bit reading to reach a placement's instance name ─────────────
    // Between the character id and the name sit a MATRIX and a colour transform, both bit-packed and
    // variable-length. Reading past them is what makes the name reachable; going by the tag's vtable
    // instead would tie this to an address that moves on a game update, while the body layout does not.
    struct BodyCur
    {
        const unsigned char *b;
        uint32_t len;
        uint32_t bit;
        bool bad;
    };

    uint32_t body_bits(BodyCur &c, uint32_t n)
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            if ((c.bit >> 3) >= c.len)
            {
                c.bad = true;
                return 0;
            }
            v = (v << 1) | ((c.b[c.bit >> 3] >> (7 - (c.bit & 7))) & 1u);
            ++c.bit;
        }
        return v;
    }

    void body_align(BodyCur &c) { c.bit = (c.bit + 7u) & ~7u; }

    // depth, character id and morph ratio are ordinary LITTLE-ENDIAN u16 byte fields, not bit-packed -
    // reading them through the bit reader byte-swaps them (an offline check of this decoder against the
    // real movies reported character 57600 for 225 and depth 24064 for 94 before this existed). They are
    // always byte-aligned where they occur: the flags come first and are whole bytes.
    uint32_t body_u16(BodyCur &c)
    {
        const uint32_t at = c.bit >> 3;
        if ((c.bit & 7u) != 0 || at + 1 >= c.len)
        {
            c.bad = true;
            return 0;
        }
        c.bit += 16;
        return static_cast<uint32_t>(c.b[at]) | (static_cast<uint32_t>(c.b[at + 1]) << 8);
    }

    void body_skip_matrix(BodyCur &c)
    {
        if (body_bits(c, 1))
            body_bits(c, body_bits(c, 5) * 2);  // scale
        if (body_bits(c, 1))
            body_bits(c, body_bits(c, 5) * 2);  // rotate/skew
        body_bits(c, body_bits(c, 5) * 2);      // translate
        body_align(c);
    }

    void body_skip_cxform(BodyCur &c)
    {
        const uint32_t has_add = body_bits(c, 1);
        const uint32_t has_mult = body_bits(c, 1);
        const uint32_t nbits = body_bits(c, 4);
        if (has_mult)
            body_bits(c, nbits * 4);
        if (has_add)
            body_bits(c, nbits * 4);
        body_align(c);
    }

    // Decode one placement tag body: is this a NAMED placement, and if so, what does it place, at what
    // depth, under what name? PlaceObject2 and PlaceObject3 differ by one byte (the second flags byte),
    // so both readings are tried and the one whose name comes out as a plain printable string is taken -
    // a wrong reading shifts every field and cannot produce one.
    bool decode_named_place(const unsigned char *body, uint32_t len, uint16_t *depth, uint16_t *cid,
                            char *name, uint32_t namecap)
    {
        for (int form = 0; form < 2; ++form)
        {
            BodyCur c{body, len, 0, false};
            const uint32_t flags = body_bits(c, 8);
            if (form)
                body_bits(c, 8);  // PlaceObject3's second flags byte
            if (!(flags & 0x20))
                continue;  // no instance name: not one of the placements we read
            const uint32_t dep = body_u16(c);
            uint32_t ch = 0;
            if (flags & 0x02)
                ch = body_u16(c);
            if (flags & 0x04)
                body_skip_matrix(c);
            if (flags & 0x08)
                body_skip_cxform(c);
            if (flags & 0x10)
                body_u16(c);  // morph ratio
            if (c.bad)
                continue;
            const uint32_t at = c.bit >> 3;
            uint32_t n = 0;
            while (at + n < len && body[at + n] && n + 1 < namecap)
            {
                const unsigned char k = body[at + n];
                if (k < 0x20 || k > 0x7E)
                {
                    n = 0;
                    break;
                }
                ++n;
            }
            if (!n || at + n >= len || body[at + n] != 0)
                continue;  // no terminator inside the copy: this reading is not the right one
            for (uint32_t i = 0; i < n; ++i)
                name[i] = static_cast<char>(body[at + i]);
            name[n] = 0;
            *depth = static_cast<uint16_t>(dep);
            *cid = static_cast<uint16_t>(ch);
            return true;
        }
        return false;
    }

    // Walk one frame's tag list, reporting what the frame places by name. `wanted`/`already` are matched
    // against the instance names; maxDepth comes back as the highest depth any tag in the frame uses.
    bool scan_frame_names(uint64_t sd, uint16_t *hostCid, uint16_t *maxDepth, int *already)
    {
        const uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        const uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        if (!looks_heap(fdata) || fcnt == 0 || fcnt > 8192)
            return false;
        uint64_t tags = 0;
        uint32_t tagCount = 0;
        if (!safe_copy(&tags, (void *)fdata, 8) ||
            !safe_copy(&tagCount, (void *)(fdata + 8), 4))
            return false;
        if (!looks_heap(tags) || tagCount == 0 || tagCount > 4096)
            return false;
        bool found = false;
        for (uint32_t i = 0; i < tagCount; ++i)
        {
            uint64_t tag = 0;
            if (!safe_copy(&tag, (void *)(tags + i * 8), 8) || !looks_heap(tag))
                continue;
            unsigned char body[0x60] = {};
            uint32_t got = 0;
            for (uint32_t want = sizeof(body); want >= 0x10; want /= 2)
                if (safe_copy(body, (void *)(tag + 8), want))  // the SWF body starts at tag+8
                {
                    got = want;
                    break;
                }
            if (!got)
                continue;
            uint16_t dep = 0, ch = 0;
            char nm[64] = {};
            if (!decode_named_place(body, got, &dep, &ch, nm, sizeof(nm)))
                continue;
            if (dep > *maxDepth)
                *maxDepth = dep;
            if (std::strcmp(nm, kTipChildName) == 0)
                *already = 1;  // the load-time transform got here first
            if (std::strcmp(nm, kHostChildName) == 0 && ch)
            {
                *hostCid = ch;
                found = true;
            }
        }
        return found;
    }

    // Add tags to a frame's tag list. Both the list and the tag objects are ARENA-owned in stock
    // Scaleform and the engine frees neither (Frame::DestroyTags only runs each tag's non-deleting
    // destructor, and nothing releases pTagPtrList - audited 2026-07-28), so a longer array from the
    // game's own malloc is safe here and the old one is simply left where it lies. That is the same
    // ownership rule the icon frames stand on; the FRAME array is the one that must come from
    // Scaleform's heap, and this does not touch it.
    bool append_frame_tags(uint64_t sd, const uint64_t *add, uint32_t addN)
    {
        const uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        if (!looks_heap(fdata))
            return false;
        uint64_t tags = 0;
        uint32_t tagCount = 0;
        if (!safe_copy(&tags, (void *)fdata, 8) || !safe_copy(&tagCount, (void *)(fdata + 8), 4))
            return false;
        if (!looks_heap(tags) || tagCount == 0 || tagCount > 4096)
            return false;
        uint64_t *arr = (uint64_t *)gfx_alloc(((size_t)tagCount + addN) * 8);
        if (!arr)
            return false;
        if (!safe_copy(arr, (void *)tags, (size_t)tagCount * 8))
            return false;
        for (uint32_t i = 0; i < addN; ++i)
            arr[tagCount + i] = add[i];
        const uint32_t total = tagCount + addN;
        // The pointer first, the count second: the frame is played from the game's own thread, and a
        // count that reached it before the array would send it through the old array's end.
        if (!safe_copy((void *)fdata, &arr, 8))
            return false;
        return safe_copy((void *)(fdata + 8), &total, 4);
    }

    // Examine every SpriteDef the parser has finished, once each, for the panel host. Runs while the
    // worldmap movie loads; stops for good as soon as the panels are in.
    void inject_map_panels()
    {
        // Only with the tag vtable CAPTURED from this movie's own tags. The RVA fallback would be an
        // address from another game version, and unlike a read that faults into a skipped feature, a
        // wrong vtable in a frame the engine PLAYS is a call through whatever sits there.
        if (!g_po3_vt.load(std::memory_order_relaxed))
        {
            static std::atomic<int> s_said{0};
            if (s_said.exchange(1, std::memory_order_relaxed) == 0)
                spdlog::info("[panels] the placement tag vtable has not been captured from this movie "
                             "yet - it comes from the icon sprite, which loads first, so this only says "
                             "the host was reached before it");
            return;
        }
        const unsigned head = g_idx.load(std::memory_order_relaxed);
        if (g_panel_scan_init.exchange(1, std::memory_order_relaxed) == 0)
            g_panel_scan_at.store(head ? head - 1 : 0, std::memory_order_relaxed);
        unsigned at = g_panel_scan_at.load(std::memory_order_relaxed);
        if (head > (unsigned)RING && at < head - (unsigned)RING)
            at = head - (unsigned)RING;  // the ring wrapped past what we had not looked at yet
        for (; at < head; ++at)
        {
            const uint64_t sd = (uint64_t)g_sprites[at % RING];
            if (!looks_heap(sd))
                continue;
            if (!rd32(sd + OFF_FRAMEARR_COUNT))
            {
                // No tags yet. For the newest def that means it is still being parsed, so the cursor
                // stays on it; for an older one it means it never got any, and waiting for it would
                // park the cursor short of the host forever.
                if (at + 1 < head)
                    continue;
                break;
            }
            uint16_t hostCid = 0, maxDepth = 0;
            int already = 0;
            const bool host = scan_frame_names(sd, &hostCid, &maxDepth, &already);
            if (already)
            {
                g_panel_scan_at.store(at + 1, std::memory_order_relaxed);
                g_panels_state.store(2, std::memory_order_relaxed);
                spdlog::info("[panels] '{}' is already in this movie - the load-time transform saw the "
                             "file, so nothing is added here",
                             kTipChildName);
                return;
            }
            if (!host)
                continue;
            // Above everything the frame already uses, so the panels draw over the clips they share the
            // host with, and two apart so each keeps a depth of its own.
            const uint16_t d1 = static_cast<uint16_t>(maxDepth + 2);
            const uint16_t d2 = static_cast<uint16_t>(maxDepth + 4);
            const uint64_t t1 = build_named_place_tag(hostCid, d1, kParkedMatrix, sizeof(kParkedMatrix),
                                                      kTipChildName);
            const uint64_t t2 = build_named_place_tag(hostCid, d2, kParkedMatrix, sizeof(kParkedMatrix),
                                                      kBannerChildName);
            if (!t1 || !t2)
            {
                spdlog::warn("[panels] tag allocation failed - the map panels stay absent on this run");
                g_panel_scan_at.store(at + 1, std::memory_order_relaxed);
                return;
            }
            const uint64_t add[2] = {t1, t2};
            const bool ok = append_frame_tags(sd, add, 2);
            g_panel_scan_at.store(at + 1, std::memory_order_relaxed);
            if (ok)
            {
                g_panels_state.store(1, std::memory_order_relaxed);
                spdlog::info("[panels] '{}' + '{}' placed in the parsed movie: host sprite {} (charId "
                             "{}), placing character {}, depths {} and {}",
                             kTipChildName, kBannerChildName, sd, rd32(sd + OFF_CHARID), hostCid, d1,
                             d2);
            }
            else
            {
                spdlog::warn("[panels] the host frame's tag list could not be extended - the map panels "
                             "stay absent on this run");
            }
            return;
        }
        g_panel_scan_at.store(at, std::memory_order_relaxed);
    }

    void seh_inject_map_panels()
    {
        __try
        {
            inject_map_panels();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void *spriteloader_detour(void *rcx, void *rdx)
    {
        uint32_t cid = (rcx ? peek_charid((uint64_t)rcx) : 0xFFFFFFFF); // charId before the load consumes it
        void *ret = o_spriteloader(rcx, rdx);
        // Ask the movie for its own name. Sampling it on ordinary loads too is what lets the field prove
        // it varies between movies (see note_name) - the cost after the first call is one guarded read,
        // and it stops once proven.
        if (rcx && !g_name_varies.load(std::memory_order_relaxed))
            ctx_is_worldmap((uint64_t)rcx);
        // The map's own panels go into the parsed movie, so they exist even on a build whose file the
        // load-time transform never saw. Tried after each of the WORLDMAP movie's sprites finished
        // parsing, and only until they are in: the host sprite is one of the last it defines, and which
        // one it is has to come from the sprite itself rather than from a charId we guessed.
        if (rcx && !g_panels_state.load(std::memory_order_relaxed))
        {
            const int wm = ctx_is_worldmap((uint64_t)rcx);
            if (wm == 1 || (wm < 0 && (uint64_t)rcx == g_worldmap_ctx.load(std::memory_order_relaxed)))
                seh_inject_map_panels();
        }
        const int is_wm = (cid == 171 && rcx) ? ctx_is_worldmap((uint64_t)rcx) : -1;
        if (cid == 171 && rcx && is_wm == 0 && g_name_varies.load(std::memory_order_relaxed))
        {
            // Ruled out by a name from a field that has proven itself. Before this existed, a sprite 171
            // belonging to some other movie could be taken for the worldmap's.
            static std::atomic<int> s_said{0};
            if (s_said.exchange(1) == 0)
                spdlog::info("[gfxprobe] sprite 171 belongs to another movie - not ours to touch");
        }
        else if (cid == 171 && rcx && !g_qmark_injected.load(std::memory_order_relaxed))
        {
            // Find the WORLDMAP sprite-171 among ALL recorded ctors (charId 171 + worldmap-sized
            // frameCount), NOT just the last-ctor'd sprite. The previous "last ctor" proxy was a
            // RACE: a 1-frame child sprite is often ctor'd between the worldmap 171 ctor and this
            // loader call, so the proxy read frameCount=1 (and the old fcnt>=300 gate also rejected
            // overhauls whose worldmap 171 has fewer frames, e.g. ERR 2.2.9.x = 261). locate_sprite171
            // already scans the ring for charId==171 with a frame array of fc in [100,4096] (skips
            // 1-frame "other movie" 171s), so it returns the real worldmap sprite on any base.
            uint64_t sd = locate_sprite171();
            if (is_wm == 1)
                spdlog::info("[gfxprobe] worldmap confirmed BY NAME (sprite {} located: {})", cid,
                             sd ? "yes" : "no");
            if (sd)
            {
                uint32_t fcnt = rd32(sd + OFF_FRAMECOUNT);
                g_qmark_injected.store(true, std::memory_order_relaxed);
                seh_inject_sprite171(sd, (uint64_t)rcx, fcnt); // SEH-guarded (runs for every user on map load)
            }
        }
        else if (cid == LOGO_PLAQUE_SPRITE && rcx &&
                 (uint64_t)rcx == g_worldmap_ctx.load(std::memory_order_relaxed) &&
                 !g_logo_placed.load(std::memory_order_relaxed))
        {
            // Same worldmap movie as the icon inject (gated by ctx): re-point the plaque's char-10
            // placement to our logo bitmap. Sprite 246 loads after 13507, so the logo charId is set.
            unsigned i = g_idx.load(std::memory_order_relaxed);
            uint64_t sd = i ? (uint64_t)g_sprites[(i - 1) % RING] : 0;
            uint32_t scid = sd ? rd32(sd + OFF_CHARID) : 0;
            if (sd && scid == LOGO_PLAQUE_SPRITE)
            {
                const bool logo_ok = seh_inject_logo(sd);
                if (logo_ok)
                    g_logo_placed.store(true, std::memory_order_relaxed);
                // Logged because it was logged NOWHERE: the only record of this was the status
                // status registry, which had no reader and has since been taken out of the build.
                spdlog::info("[icons] logo plaque: {}", logo_ok ? "re-pointed" : "re-point FAILED");
            }
        }
        return ret;
    }

    // ── V3 native markers: how the RM2 hook below earned its keep (HISTORY) ──
    // A one-shot spike used to ride here: on the first native RemoveObject2 executed on a live
    // display context with the world map open, it executed OUR synthesized PlaceObject3 on the same
    // ctx through the tag's own vtable Execute. It proved the v3 mechanism end to end -
    // place-into-live-list, native render, parent-transform inheritance, teardown tolerance - and
    // that is why native markers ship. The spike itself is long gone; what the detour below does
    // TODAY is log the display list for our injected RM2 tags and drive the marker-factory pulse.
    // Written up in scratch/v3_native_markers_plan.md.


    // DIAGNOSTIC detour on RemoveObject2::Execute. For OUR injected RM2 tags, inspect the display list
    // (ctx+0x28 base, ctx+0x30 count; node depth@+0x14, sticky flag@+0x71) for an entry at our depth,
    // BEFORE the original runs - tells us whether the anon's layer is present and sticky.
    // ── which display-list contexts do we actually get? ──────────────────────────────
    // A context is only alive inside the callback that hands it to us (retaining one across frames
    // crashed the first V3 build), so knowing WHICH sprites' contexts pass through decides whether a
    // panel can be created at runtime at all - and under which callback.
    constexpr int kCtxSeenMax = 16;
    uint64_t g_ctx_seen[kCtxSeenMax] = {};
    uint32_t g_ctx_seen_n = 0;

    // Print the candidate fields rather than trusting one offset: the marker factory reads the sprite
    // at ctx+0x58, and that produced a nonsense character id here - so the layout in THIS callback is
    // something the log has to tell us. Deduplicated by CONTEXT, so one bad read cannot silence the
    // rest (which is what happened the first time).
    void note_context(uint64_t ctx, const char *via)
    {
        if (!goblin::config::debugLogging || !ctx)
            return;
        for (uint32_t i = 0; i < g_ctx_seen_n; ++i)
            if (g_ctx_seen[i] == ctx)
                return;
        if (g_ctx_seen_n >= kCtxSeenMax)
            return;
        g_ctx_seen[g_ctx_seen_n++] = ctx;
        for (uint32_t off = 0x18; off <= 0x78; off += 8)
        {
            const uint64_t p = rq(ctx + off);
            if (!looks_heap(p))
                continue;
            const uint32_t cid = rd32(p + OFF_CHARID);
            const uint32_t fc = rd32(p + OFF_FRAMECOUNT);
            const bool sprite_like = cid > 0 && cid < 8192 && fc > 0 && fc < 100000;
            spdlog::info("[ctxprobe] {} ctx 0x{:X} +0x{:X} -> 0x{:X} charId {} frames {}{}", via, ctx,
                         off, p, cid, fc, sprite_like ? "  <-- looks like a sprite" : "");
        }
    }

    void *rm2exec_detour(void *thisTag, void *ctx, uint32_t frame)
    {
        uint64_t t = (uint64_t)thisTag;
        if ((t == g_my_rm2_d1.load(std::memory_order_relaxed) ||
             t == g_my_rm2_d2.load(std::memory_order_relaxed)) &&
            g_rmhook_logs.fetch_add(1, std::memory_order_relaxed) < 40)
        {
            uint16_t dep = 0; safe_copy(&dep, (void *)(t + 8), 2); // RM2 depth = u16 @body+0 (tag+8)
            uint64_t cx = (uint64_t)ctx;
            uint64_t lbase = rq(cx + 0x28);
            uint64_t lcnt = rq(cx + 0x30);
            uint64_t foundNode = 0; uint8_t f71 = 0; uint32_t fcid = 0;
            if (looks_heap(lbase) && lcnt > 0 && lcnt < 65536)
                for (uint64_t i = 0; i < lcnt; ++i)
                {
                    uint64_t node = rq(lbase + i * 8);
                    if (looks_heap(node) && rd32(node + 0x14) == dep)
                    {
                        foundNode = node;
                        safe_copy(&f71, (void *)(node + 0x71), 1);
                        break;
                    }
                }
            spdlog::debug("[rmtag] my RM2 depth={} ctx=0x{:X} listBase=0x{:X} cnt={} -> entry@0x{:X} flag71=0x{:02X}",
                         dep, cx, lbase, lcnt, foundNode, f71);
        }
        const uint32_t exact_count = g_sprite171_rm2_count.load(std::memory_order_acquire);
        const bool exact_sprite171 = exact_count != 0 &&
            std::binary_search(g_sprite171_rm2_tags,
                               g_sprite171_rm2_tags + exact_count, t);
        note_context(reinterpret_cast<uint64_t>(ctx), "rm2");
        void *ret = o_rm2exec(thisTag, ctx, frame);
        // Execute queued native placements only while this callback's timeline
        // context is live. Retaining ctx for a later map frame caused the first
        // integrated V3 build to crash before its first attach.
        // Materializing our children from inside the executor was suspected and cleared: with this pulse
        // off the black tiles remained, and they went away only when the resolver/loader hooks did.
        if (goblin::variants::kNativeMarkers && exact_sprite171 && ctx &&
            g_qmark_injected.load(std::memory_order_relaxed) &&
            goblin::maphover::map_dialog() != nullptr)
            goblin::stall_probe::v3_native_factory_pulse(ctx, frame);
        return ret;
    }

#ifdef MFG_DUMP_FRAMES
    // One-shot diagnostic: dump the live tag layout of selected frames so we can see, for THIS build's
    // gfx in memory, what RemoveObject2 vs PlaceObject tag objects actually look like (vtable + body).
    void dump_frames(uint64_t sd)
    {
        uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        if (!looks_heap(fdata) || fcnt == 0) return;
        uint32_t idxs[] = {0, 1, 2, 3, fcnt >= 3 ? fcnt - 3 : 0, fcnt >= 2 ? fcnt - 2 : 0, fcnt - 1};
        for (uint32_t ii = 0; ii < sizeof(idxs) / sizeof(idxs[0]); ++ii)
        {
            uint32_t fi = idxs[ii];
            uint64_t elem = fdata + (uint64_t)fi * FRAME_STRIDE;
            uint64_t tagsArr = rq(elem);
            uint32_t tc = rd32(elem + 8);
            spdlog::debug("[framedump] frame[{}] elem@0x{:X} tagsArr=0x{:X} tagCount={}", fi, elem, tagsArr, tc);
            if (!looks_heap(tagsArr) || tc == 0 || tc > 32) continue;
            const uint64_t RM2_VT = (uint64_t)GetModuleHandleW(nullptr) + RVA_VT_REMOVEOBJECT2;
            for (uint32_t k = 0; k < tc; ++k)
            {
                uint64_t t = rq(tagsArr + (uint64_t)k * 8);
                unsigned char b[0x20] = {0};
                safe_copy(b, (void *)t, sizeof(b));
                uint64_t vt = 0; memcpy(&vt, b, 8);
                const char *kind = (vt == RM2_VT) ? "RM2" : "PO?";
                spdlog::debug("[framedump]   tag[{}]@0x{:X} {} +0x08={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} +0x10={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} +0x18={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                             k, t, kind,
                             b[0x08], b[0x09], b[0x0A], b[0x0B], b[0x0C], b[0x0D], b[0x0E], b[0x0F],
                             b[0x10], b[0x11], b[0x12], b[0x13], b[0x14], b[0x15], b[0x16], b[0x17],
                             b[0x18], b[0x19], b[0x1A], b[0x1B], b[0x1C], b[0x1D], b[0x1E], b[0x1F]);
            }
        }
    }
#endif // MFG_DUMP_FRAMES

    // Append ONE frame at the END of the live timeline (any base gfx), placing newCharId, and return its
    // 1-based iconId (= the new frameCount). Frame = RemoveObject2(d1)+RemoveObject2(d2)+PlaceObject(@d1)
    // - exactly what a gfx-authored composite frame does, so it clears the predecessor and shows only our
    // icon. Returns 0 on failure. Called once per embedded icon at worldmap load.
    uint32_t append_icon_frame(uint64_t sd, uint16_t newCharId, const unsigned char *mat, unsigned matLen)
    {
        uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        uint32_t fcap = rd32(sd + OFF_FRAMEARR_CAP);
        if (fcnt == 0 || fcnt > 8192 || !looks_heap(fdata))
        {
            spdlog::warn("[icons] frame array looks wrong (cnt={}); skip.", fcnt);
            return 0;
        }
        // Replicate EXACTLY what a gfx-authored composite frame does (add_gfx_icon.py, proven in-game to
        // cleanly clear the predecessor): RemoveObject2(depth 1) + RemoveObject2(depth 2) + PlaceObject
        // (our image @depth 1). We SYNTHESIZE the two RemoveObject2 tags directly (build_remove_tag) -
        // a RemoveObject2 is just {vtable=RemoveObject2 @+0; depth u16 @+8}. (Earlier this borrowed/cloned
        // tags by a vtable that was actually PlaceObject2, so the "removes" re-placed and never cleared
        // depth 1 - that was the root-cause bug.) The real RemoveObject2::Execute unlinks the display node
        // at the depth, so depths 1 and 2 of the predecessor composite are cleared before our place.
        uint64_t rm2_d1 = build_remove_tag(1);
        uint64_t rm2_d2 = build_remove_tag(2);
        g_my_rm2_d1.store(rm2_d1, std::memory_order_relaxed); // for the rm2exec_detour diagnostic
        g_my_rm2_d2.store(rm2_d2, std::memory_order_relaxed);
        // The placement matrix is PER-ICON (passed in): each icon's bitmap is cropped tight, so its
        // centering depends on its own W x H. Computed at build time by generate_map_icons.icon_matrix().
        uint64_t placeTag = build_clean_place_tag(newCharId, 1, mat, matLen); // bitmap (HasImage)
        if (!placeTag)
        {
            spdlog::warn("[icons] build_clean_place_tag failed; abort.");
            return 0;
        }
        uint64_t tags[3];
        uint32_t tagCount = 0;
        if (rm2_d1) tags[tagCount++] = rm2_d1;
        if (rm2_d2) tags[tagCount++] = rm2_d2;
        tags[tagCount++] = placeTag;
        uint64_t *tagsArr = (uint64_t *)gfx_alloc((size_t)tagCount * 8);
        if (!tagsArr)
        {
            spdlog::warn("[icons] tags array setup failed; skip.");
            return 0;
        }
        for (uint32_t j = 0; j < tagCount; ++j)
            tagsArr[j] = tags[j];
        unsigned char elem[FRAME_STRIDE] = {0};
        memcpy(elem, &tagsArr, 8);
        memcpy(elem + 8, &tagCount, 4);

        uint32_t target = fcnt + 1; // append exactly one frame at the end
        uint64_t arr = fdata;
        if (target > fcap)
        {
            uint32_t newcap = target + 64;
            // Scaleform heap, NOT the CRT: ~SpriteDef frees this pointer through MemoryHeap::Free.
            void *buf = gfx_heap_alloc((size_t)newcap * FRAME_STRIDE);
            spdlog::info("[verify] frame array grow {} -> {} from the SCALEFORM heap: 0x{:X}",
                         fcap, newcap, (uint64_t)buf);
            if (!buf || !safe_copy(buf, (void *)fdata, (size_t)fcnt * FRAME_STRIDE))
            {
                spdlog::warn("[icons] frame grow failed; abort.");
                return 0;
            }
            arr = (uint64_t)buf;
            wr64(sd + OFF_FRAMEARR_DATA, arr);
            wr32(sd + OFF_FRAMEARR_CAP, newcap);
        }
        safe_copy((void *)(arr + (uint64_t)fcnt * FRAME_STRIDE), elem, FRAME_STRIDE);
        wr32(sd + OFF_FRAMEARR_COUNT, target);
        wr32(sd + OFF_FRAMECOUNT, target);
        uint32_t iconid = fcnt + 1; // 1-based iconId of the appended frame
        spdlog::debug("[icons] frame iconId {} charId {} ({} RM2 + place)", iconid, newCharId, tagCount - 1);
        return iconid;
    }

    void *lookup_detour(void *rcx, uint32_t charId, void *r8)
    {
        // Capture ONLY when resolving charId 171 (the worldmap icon sprite) so movieDef is the
        // worldmap movie, not some other tiny menu movie that happens to resolve first.
        if (charId == 171 && !g_moviedef.load(std::memory_order_relaxed) && rcx)
        {
            uint64_t md = (uint64_t)rcx - OFF_MOVIEDEF_DICT; // container = movieDef+0xd8
            if (looks_heap(md))
                g_moviedef.store(md, std::memory_order_relaxed);
        }
        void *res = o_lookup(rcx, charId, r8);
        // A Path-A milestone log stood here: for each distinct charId in our injected range, print
        // whether the char-dict lookup resolved it. It was gated on g_ms_watch_cid, which nothing
        // ever set, so it never printed a line.
        return res;
    }

    // READ-ONLY: dump the resource-dict read array so we can design resource insertion (R1) from
    // exact live structure. Logs count, head/tail nodes (charId@+0x2c, vtable), to learn sort order,
    // max charId, and a clonable image binding node. Never writes.
#ifdef MFG_DUMP_FRAMES
    void dump_dict(uint64_t movieDef)
    {
        uint64_t cont = movieDef + OFF_MOVIEDEF_DICT;
        uint64_t base = rq(cont);
        uint32_t count = rd32(cont + 8);
        spdlog::debug("[dictdump] movieDef=0x{:X} container@0x{:X}: base=0x{:X} count={} (+0x10=0x{:X} +0x18=0x{:X})",
                     movieDef, cont, base, count, rq(cont + 0x10), rq(cont + 0x18));
        if (!looks_heap(base) || count == 0 || count > 100000)
        {
            spdlog::warn("[dictdump] container looks wrong; abort.");
            return;
        }
        auto node_at = [&](uint32_t k) { return rq(base + (uint64_t)k * 16); };
        // head + tail nodes: charId@+0x2c, to confirm ascending sort + find max charId
        spdlog::debug("[dictdump] first 8 nodes (charId@+0x2c, vtable@+0):");
        for (uint32_t k = 0; k < 8 && k < count; ++k)
        {
            uint64_t n = node_at(k);
            spdlog::debug("[dictdump]   [{}] node=0x{:X} charId={} vt=0x{:X}", k, n, rd32(n + OFF_NODE_CHARID), rq(n));
        }
        spdlog::debug("[dictdump] last 6 nodes:");
        for (uint32_t k = (count > 6 ? count - 6 : 0); k < count; ++k)
        {
            uint64_t n = node_at(k);
            spdlog::debug("[dictdump]   [{}] node=0x{:X} charId={} vt=0x{:X}", k, n, rd32(n + OFF_NODE_CHARID), rq(n));
        }
        // dump full structure of node[0] (low charId = image, the R1 clone template) and a mid node
        for (uint32_t idx : {0u, count / 2})
        {
            uint64_t nn = node_at(idx);
            spdlog::debug("[dictdump] node [{}] @0x{:X} charId={} vt=0x{:X} - first 0x60 bytes:",
                         idx, nn, rd32(nn + OFF_NODE_CHARID), rq(nn));
            for (unsigned off = 0; off < 0x60; off += 8)
            {
                uint64_t v = rq(nn + off);
                const char *t = looks_heap(v) ? " <mem>" : "";
                spdlog::debug("[dictdump]     +0x{:02X}: {:016X}{}", off, v, t);
            }
        }
    }
#endif // MFG_DUMP_FRAMES

    // Return the worldmap ICON sprite: the MOST-RECENTLY ctor'd SpriteDef with charId@+0x18==171 and
    // more than one frame. Rationale:
    //  - charId 171 exists in several movies; the worldmap's is the multi-frame icon sprite (it holds
    //    the game's map-pin frames - hundreds), decorative 171s elsewhere are 1 frame. So "fc>1" is the
    //    discriminator - frame-count-VALUE agnostic (ERR 2.2.9.x 261 / stock 348 / our 440 / Convergence
    //    756 / anything), NO hardcoded threshold.
    //  - MOST-RECENT (not biggest): the sprite-loader hook injects using the CURRENT load context
    //    (rcx) to register bitmaps in that movie's image manager, so the sprite MUST be the one this
    //    load call is processing = the 171 just ctor'd for it. (The old code took the absolute-last
    //    ctor'd sprite of ANY charId -> usually a 1-frame child ctor'd in between -> read frameCount=1;
    //    scanning for the most-recent charId==171 specifically fixes that race.)
    // Movie-based identity isn't usable here: injection must run at LOAD, before the render-time
    // charId-171 lookup (lookup_detour) reveals the worldmap movie. Returns 0 if none seen yet.
    uint64_t locate_sprite171()
    {
        unsigned head = g_idx.load(std::memory_order_relaxed); // total ctors; head-1 = most recent slot
        unsigned n = head < (unsigned)RING ? head : (unsigned)RING; // scan only written slots (no underflow)
        for (unsigned back = 1; back <= n; ++back)
        {
            uint64_t sd = (uint64_t)g_sprites[(head - back) % RING];
            if (!looks_heap(sd))
                continue;
            if (rd32(sd + OFF_CHARID) != 171)
                continue;
            uint32_t fc = rd32(sd + OFF_FRAMECOUNT);
            if (fc <= 1 || fc > 1000000) // 1-frame decorative 171 (not the icon sprite) / garbage
                continue;
            // sanity: frame array present and self-consistent
            uint64_t data = rq(sd + OFF_FRAMEARR_DATA);
            uint32_t cnt = rd32(sd + OFF_FRAMEARR_COUNT);
            if (!looks_heap(data) || cnt != fc)
                continue;
            return sd;
        }
        return 0;
    }

    // SYNTHESIZE a PlaceObject3 tag that places image `charId` at `depth` with OUR icon matrix - fully
    // from scratch, NO gfx template (drops the last gfx-content dependency: cloning grace frame 0). PO3
    // tag-object layout (from PO3::Execute 0x1411bdb40 disasm): vtable@+0, then the inline SWF body at +8:
    //   flags0@+8 = 0x06 (HasCharacter|HasMatrix, Move=0, no ColorTransform - exactly what grace's working
    //               placement uses), flags1@+9 = 0x10 (HasImage), depth u16@+0xa, charId u16@+0xc,
    //   MATRIX@+0xe (the per-icon matrix from generate_map_icons.icon_matrix: scale + centered for that
    //               icon's cropped WxH).
    // The frame executor dispatches purely on tag[0]=vtable with no heap/size check, so a VirtualAlloc'd
    // tag runs identically to a native one (same proven mechanism as build_remove_tag for RemoveObject2).
    uint64_t build_clean_place_tag(uint16_t charId, uint16_t depth, const unsigned char *matrix, unsigned matLen)
    {
        void *t = gfx_alloc(0x40);
        if (!t)
            return 0;
        uint64_t vt = po3_vtable();                            // captured-live (RVA fallback)
        // flags0 0x06 = HasCharacter|HasMatrix (NO ColorTransform) - matches the NATIVE vanilla icon
        // PO3 tags (verified via framedump: native icons place at 0x06 and render full), so no cxform.
        uint8_t flags0 = 0x06, flags1 = 0x10;
        safe_copy(t, &vt, 8);                                   // vtable @+0
        safe_copy((void *)((uint64_t)t + 8), &flags0, 1);      // flags0 @+8  (HasCharacter|HasMatrix)
        safe_copy((void *)((uint64_t)t + 9), &flags1, 1);      // flags1 @+9  (HasImage)
        safe_copy((void *)((uint64_t)t + 0xa), &depth, 2);     // depth  u16 @+0xa
        safe_copy((void *)((uint64_t)t + 0xc), &charId, 2);    // charId u16 @+0xc
        safe_copy((void *)((uint64_t)t + 0xe), (void *)matrix, matLen); // matrix @+0xe
        return (uint64_t)t;
    }

    // build_clean_sprite_place_tag() stood here: the same placement tag with the HasImage flag
    // cleared, so the engine instantiates the charId as a MovieClip (which exposes the
    // DrawingContext getter) instead of a bitmap leaf. It existed for the solid-fill spike, and
    // its "used only by create_native_sprite_child" note named a function that never existed in
    // this tree. Its one call site was the asSprite branch of append_icon_frame, and asSprite
    // defaulted to false at the single call - so the branch was never taken.

    // ============================ TASK #4: place the 5 settings roots ============================
    // Make the 5 settings MainTimeline roots (registered by the def stream at charIds 342..448) LIVE,
    // NAMED children of the map _root, so the dialog ctor's FUN_14074a2f0(mapScene,,"TabList")/etc.
    // resolve. Two routes (SETTINGS_ROUTE): P = synthesize a NAMED PlaceObject3 ExecuteTag and call its
    // Execute(tag, rootCtx, frame) - the engine interns the name (GASString) + builds CharPosInfo for us,
    // the SAME proven path our icon frames use. A' = call the root's virtual AddDisplayObject (vtable+0x108)
    // directly with a cloned CharPosInfo (mechanical; the name arg needs a GASString we cannot yet build,
    // so it reuses a captured name = misnamed, for placement-mechanism comparison only).
    // ALL dev-gated (ENABLE_SETTINGS_REHOST + debugLogging), one-shot, SEH-guarded, map-live only.

    // The settings re-host probes lived here: o_po2exec / o_resolve (never assigned, so safe_resolve
    // would have called through a null pointer had anything reached it), their SEH wrappers
    // safe_resolve / safe_exec_tag / safe_add_disp, and the verify_roots / exec_named_child pair below.
    // Six functions that referenced only each other, with no live entry point - which is why counting
    // occurrences made them look used. Removed 2026-07-30 with the rest of the re-host residue.
    // The SEH-wrapper PATTERN they demonstrated is still in force everywhere else in this file: raise
    // goblin::guarded::depth around a raw game call so the crash logger does not record a fault we asked
    // for, and carry the result out in a variable - a `return` inside the __try would skip the decrement
    // and silence the log for a REAL crash for the life of the thread.

    // A NAMED PlaceObject3 ExecuteTag: same synth as build_clean_place_tag but flags0 adds HasName (0x20)
    // and the asciiz instance name follows the matrix (SWF PlaceObject3 field order). The engine's Execute
    // reads this raw body, interns the name as a GASString, and calls AddDisplayObject - correct naming.
    uint64_t build_named_place_tag(uint16_t charId, uint16_t depth, const unsigned char *matrix,
                                   unsigned matLen, const char *name)
    {
        unsigned nlen = (unsigned)strlen(name);
        void *t = gfx_alloc(0x20 + matLen + nlen + 1);
        if (!t) return 0;
        uint64_t vt = po3_vtable();
        uint8_t flags0 = 0x26, flags1 = 0x00; // HasCharacter|HasMatrix|HasName, no HasImage
        safe_copy(t, &vt, 8);
        safe_copy((void *)((uint64_t)t + 8), &flags0, 1);
        safe_copy((void *)((uint64_t)t + 9), &flags1, 1);
        safe_copy((void *)((uint64_t)t + 0xa), &depth, 2);
        safe_copy((void *)((uint64_t)t + 0xc), &charId, 2);
        safe_copy((void *)((uint64_t)t + 0xe), (void *)matrix, matLen);
        safe_copy((void *)((uint64_t)t + 0xe + matLen), (void *)name, nlen + 1); // asciiz name
        return (uint64_t)t;
    }

    // verify_roots() and exec_named_child() stood here - the other half of the dead re-host web
    // described above. Note that build_named_place_tag(), which exec_named_child called, is LIVE and
    // stays: the composite icon frames build their placements with it.

    // Does "TabList" resolve to a live target under `scene`? (guarded so it doesn't re-enter us)

    // Empirical placement probe (static RE stalled on the GFxValue scene<->ctx bridge). From a LIVE
    // map-dialog resolve callback we hold a real map-movie `scene`. Try candidate placement contexts;
    // for each whose movie has our TabList charId registered (dict guard = crash-safe), place a NAMED
    // TabList child + ask the resolver whether "TabList" now resolves. The candidate that works is the
    // right ctx -> place the other 4 roots into it. One-shot, SEH- + re-entrancy-guarded.

    // Capture the native PlaceObject3 + RemoveObject2 tag vtables from the stock sprite-171 frames, so we
    // synthesize tags without a hardcoded vtable RVA. A composite icon frame is RemoveObject2(d1) +
    // RemoveObject2(d2) + PlaceObject3(image) -> tag list [Xvt, Xvt, Yvt] where Y carries the HasImage flag
    // (flags1@+9 == 0x10). The first such frame gives RM2=Xvt, PO3=Yvt. The stock worldmap has hundreds of
    // these (246 RM2 / 234 PO3 tags). RVA fallback (po3_vtable/rm2_vtable) if none is found. One-shot.
    void capture_tag_vtables(uint64_t sd)
    {
        uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        if (!looks_heap(fdata) || fcnt == 0)
            return;
        for (uint32_t f = 0; f < fcnt; ++f)
        {
            uint64_t elem = fdata + (uint64_t)f * FRAME_STRIDE;
            uint64_t tags = rq(elem);
            uint32_t tc = rd32(elem + 8);
            if (!looks_heap(tags) || tc < 3 || tc > 64)
                continue;
            uint64_t t0 = rq(tags), t1 = rq(tags + 8), t2 = rq(tags + 16);
            if (!looks_heap(t0) || !looks_heap(t1) || !looks_heap(t2))
                continue;
            uint64_t v0 = rq(t0), v1 = rq(t1), v2 = rq(t2);
            uint8_t f2 = 0;
            safe_copy(&f2, (void *)(t2 + 9), 1);                 // PO3 image tag has flags1@+9 == 0x10
            // Shape alone is NOT an identity check. The exe links a THIRD placement class,
            // PlaceObject2 (vtable rva 0x2C65CC0), whose body is one byte shorter - so the byte we read
            // as "flags1 == 0x10" is the LOW BYTE OF ITS DEPTH, and a frame laid out [RM2, RM2, PO2]
            // whose depth happens to be 0x10 mod 256 would hand us the PO2 vtable to synthesize PO3
            // bodies with. Confirm identity instead: both candidates must carry the expected
            // AddToTimelineSnapshot in slot +0x30. Audited 2026-07-28.
            const uint64_t mod0 = (uint64_t)GetModuleHandleW(nullptr);
            const bool id_ok = rq(v0 + 0x30) == mod0 + RVA_FN_REMOVEOBJECT2_ADDSNAPSHOT &&
                               rq(v2 + 0x30) == mod0 + RVA_FN_PLACEOBJECT3_ADDSNAPSHOT;
            if (looks_heap(v0) && v0 == v1 && looks_heap(v2) && v2 != v0 && f2 == 0x10 && id_ok)
            {
                g_rm2_vt.store(v0, std::memory_order_relaxed);
                g_po3_vt.store(v2, std::memory_order_relaxed);
                uint64_t mod = (uint64_t)GetModuleHandleW(nullptr);
                spdlog::info("[icons] read native vtables (frame {}): RM2=0x{:X}(rva 0x{:X}) PO3=0x{:X}(rva 0x{:X})",
                             f, v0, v0 - mod, v2, v2 - mod);
                return;
            }
        }
        spdlog::warn("[icons] no composite RM2+RM2+PO3 frame found; using fallback vtable RVAs.");
    }

    void capture_sprite171_rm2_tags(uint64_t sd)
    {
        g_sprite171_rm2_count.store(0, std::memory_order_release);
        const uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        const uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        const uint64_t rmvt = g_rm2_vt.load(std::memory_order_relaxed);
        if (!looks_heap(fdata) || fcnt == 0 || fcnt > 8192 || !rmvt) return;

        uint32_t count = 0;
        for (uint32_t f = 0; f < fcnt && count < SPRITE171_RM2_CAP; ++f)
        {
            const uint64_t elem = fdata + static_cast<uint64_t>(f) * FRAME_STRIDE;
            const uint64_t tags = rq(elem);
            const uint32_t tag_count = rd32(elem + 8);
            if (!looks_heap(tags) || tag_count == 0 || tag_count > 64) continue;
            for (uint32_t i = 0; i < tag_count && count < SPRITE171_RM2_CAP; ++i)
            {
                const uint64_t tag = rq(tags + static_cast<uint64_t>(i) * 8);
                if (looks_heap(tag) && rq(tag) == rmvt)
                    g_sprite171_rm2_tags[count++] = tag;
            }
        }
        std::sort(g_sprite171_rm2_tags, g_sprite171_rm2_tags + count);
        count = static_cast<uint32_t>(std::unique(g_sprite171_rm2_tags,
                                                  g_sprite171_rm2_tags + count) -
                                      g_sprite171_rm2_tags);
        g_sprite171_rm2_count.store(count, std::memory_order_release);
        spdlog::info("[v3native] captured {} exact sprite-171 RM2 tags", count);
    }

    // Place our registered logo bitmap onto the decorative-plaque sprite (246) by re-pointing its
    // char-10 PlaceObject3 to our logo charId. PO3 tag-object body is inline at this+8: flags0@+8,
    // flags1@+9, depth@+0xa, charId@+0xc, matrix@+0xe (confirmed via PO3::Execute 0x1411bdb40). We clone
    // the char-10 tag (valid vtable/flags0/depth envelope), set HasImage (flags1|=0x10), re-key charId to
    // our logo, and overwrite the matrix with the baked LOGO_MATRIX (scale 0.38 / translate -243). char-10
    // is a plain place (HasCharacter|HasMatrix, no colorTransform), so the body is self-terminating after
    // the matrix and a longer matrix just uses spare bytes of the 0x80 clone. Returns true on success.
    bool inject_logo_into_plaque(uint64_t sd)
    {
        uint32_t logo = g_logo_charid.load(std::memory_order_relaxed);
        if (!logo)
        {
            spdlog::warn("[logo] plaque sprite loaded but logo charId not registered yet; skip.");
            return false;
        }
        uint64_t farr = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        if (!looks_heap(farr) || fcnt == 0)
            return false;
        uint64_t tags = rq(farr);             // frame 0 tags array
        uint32_t tagCount = rd32(farr + 8);
        if (!looks_heap(tags) || tagCount == 0 || tagCount > 64)
            return false;
        // find the tag whose body charId (@+0xc) == char 10 (the plaque placement)
        for (uint32_t k = 0; k < tagCount; ++k)
        {
            uint64_t srcTag = rq(tags + (uint64_t)k * 8);
            if (!looks_heap(srcTag))
                continue;
            uint16_t cid = (uint16_t)(rd32(srcTag + 0xc) & 0xFFFF);
            if (cid != LOGO_PLAQUE_CHAR)
                continue;
            // Clone 0x14 bytes, not 0x80. The destination is deliberately roomy (a longer matrix uses
            // the spare bytes), but the SOURCE tag is only 8 bytes of vtable + a 12-byte body, so a
            // 0x80-byte read ran 108 bytes past the object. safe_copy's handler turned that into a
            // silent "no logo" whenever the overread hit an unmapped page. Audited 2026-07-28.
            constexpr size_t kPlaceTagEnvelope = 0x14; // vtable + flags0/flags1 + depth + charId
            void *t = gfx_alloc(0x80);
            if (!t || !safe_copy(t, (void *)srcTag, kPlaceTagEnvelope))
                return false;
            uint8_t flags1 = 0;
            safe_copy(&flags1, (void *)((uint64_t)t + 9), 1);
            flags1 |= 0x10; // HasImage: treat charId as an image character
            safe_copy((void *)((uint64_t)t + 9), &flags1, 1);
            uint16_t lc = (uint16_t)logo;
            safe_copy((void *)((uint64_t)t + 0xc), &lc, 2);                    // charId 10 -> our logo
            safe_copy((void *)((uint64_t)t + 0xe), (void *)goblin::generated::LOGO_MATRIX,
                      goblin::generated::LOGO_MATRIX_LEN);                     // matrix 0.5 -> 0.38/-243
            wr64(tags + (uint64_t)k * 8, (uint64_t)t);                         // swap the tag pointer
            spdlog::info("[logo] plaque sprite {}: re-pointed char-{} placement (tag[{}]) to logo charId {}.",
                         LOGO_PLAQUE_SPRITE, LOGO_PLAQUE_CHAR, k, logo);
            return true;
        }
        spdlog::warn("[logo] plaque sprite {}: no char-{} placement found in frame 0 ({} tags).",
                     LOGO_PLAQUE_SPRITE, LOGO_PLAQUE_CHAR, tagCount);
        return false;
    }

    // Scan the live READ resource dict (movieDef+0xd8) once: return the highest charId < `ceil`, and (if
    // `in_window`) whether any charId falls in [lo, hi). Returns 0 if the dict isn't built/readable yet
    // (then callers fall back to the high floor). movieDef == the load ctx at sprite-171 time.
    uint32_t dict_scan(uint64_t movieDef, uint32_t lo, uint32_t hi, uint32_t ceil, bool *in_window)
    {
        if (in_window) *in_window = false;
        uint64_t cont = movieDef + OFF_MOVIEDEF_DICT;
        uint64_t base = rq(cont);
        uint32_t count = rd32(cont + 8);
        if (!looks_heap(base) || count == 0 || count > 100000)
            return 0;
        uint32_t mx = 0;
        for (uint32_t k = 0; k < count; ++k)
        {
            uint64_t node = rq(base + (uint64_t)k * 16);
            uint32_t c = node ? rd32(node + OFF_NODE_CHARID) : 0;
            if (c == 0 || c >= ceil)
                continue;
            if (c > mx) mx = c;
            if (in_window && c >= lo && c < hi) *in_window = true;
        }
        return mx;
    }

    // Self-healing charId base: a HIGH floor (generated MAP_ICON_CHARID_BASE) so natives loading after
    // sprite-171 cannot reach it, RAISED above any even-higher live charId (e.g. an overhaul gfx with
    // thousands of charIds), with the [base, base+count) window verified clear. Never a tight
    // native_max+margin (that risks a post-sprite-171 native landing inside the window).
    // `ctx` is the sprite-171 load ctx. The dict lives on the movieDef at [ctx+0x38], NOT on ctx itself -
    // reading it off ctx made every scan return 0, so the window check silently did nothing.
    uint32_t compute_safe_base(uint64_t ctx, uint32_t count)
    {
        uint64_t movieDef = rq(ctx + 0x38);
        if (!looks_heap(movieDef))
            movieDef = ctx;                                          // pre-0x38 layout: fall back to the ctx
        // Per-movie ONLY. A process-wide max is not a sound input here: measured live, another movie's
        // stream reaches 35200 and the char-def registry hands out internal ids above 0xFFFF, so a global
        // maximum would drag our base to an unrelated movie's numbering or clean out of u16 range.
        // Offline scan of both the stock and the ERR 02_120_worldmap.gfx (scratch/scan_gfx_charids.py):
        // characters 7..248, external images 1..177 + 13500..13506 - the floor's window is clear in both.
        uint32_t reg = reg_max_for(ctx);                              // max native charId the registrar saw
        uint32_t live = dict_scan(movieDef, 0, 0, 0x100000, nullptr); // overall max in the live dict (0 if not built)
        uint32_t hi = reg > live ? reg : live;
        uint32_t base = INJECT_CHARID_BASE;                          // floor
        if (hi != 0 && hi + INJECT_BASE_MARGIN > base)               // raise above live natives if they exceed the floor
            base = hi + INJECT_BASE_MARGIN;
        bool clash = false;                                          // paranoia: scattered charIds at/above base
        dict_scan(movieDef, base, base + count, 0x100000, &clash);
        if (clash && live)
            base = live + INJECT_BASE_MARGIN;                        // above the overall max -> window clear
        // charId is a u16 in every place tag we emit, so the whole window must fit under 0x10000.
        if (base + count >= 0xFFFFu)
            base = INJECT_CHARID_BASE;
        spdlog::info("[icons] charId base {}: floor {}, reg_max {} (any-movie {}), live dict max {}, "
                     "window clash {}, movieDef 0x{:X}.",
                     base, INJECT_CHARID_BASE, reg, g_reg_gmax.load(std::memory_order_relaxed), live,
                     clash ? "YES" : "no", movieDef);
        return base;
    }

    // SYNTHESIZE a genuine RemoveObject2 ExecuteTag for `depth`. A RemoveObject2 tag is just
    // {vtable @+0 ; depth u16 @+8 (= body+0, NO flags byte)} - the real RemoveObject2::Execute
    // (0x1411bde10) reads `movzx r10d, word ptr [this+8]` as the depth and unlinks the matching display
    // node. The frame executor (0x1411bf131) dispatches purely on tag[0]=vtable with no validity/heap
    // check, so a VirtualAlloc'd tag with the correct vtable runs identically to a native one - no need
    // to find/clone a real RM2. Removing a depth that holds nothing is a harmless no-op. Returns 0 on fail.
    uint64_t build_remove_tag(uint16_t depth)
    {
        void *t = gfx_alloc(0x10);
        if (!t)
            return 0;
        uint64_t vt = rm2_vtable();                          // captured-live (RVA fallback)
        safe_copy(t, &vt, 8);                                 // vtable @+0
        safe_copy((void *)((uint64_t)t + 8), &depth, 2);      // depth u16 @+8 (body+0)
        return (uint64_t)t;
    }

}

void *goblin::gfx_probe::game_alloc(size_t bytes)
{
    resolve_game_malloc();
    return gfx_alloc(bytes);
}

bool goblin::gfx_probe::movie_name(void *ctx, char *out, size_t cap)
{
    if (!ctx || !out || cap < 32)
        return false;
    // Same two steps the icon path identifies the world map by: the definition hangs off the load
    // context at +0x38 (older layouts keep the name on the context itself), and the name field's offset
    // inside it is discovered once and then cached.
    uint64_t def = rq(reinterpret_cast<uint64_t>(ctx) + 0x38);
    if (!looks_heap(def))
        def = reinterpret_cast<uint64_t>(ctx);
    return movie_file_url(def, out, cap);
}

uint32_t goblin::gfx_probe::injected_iconid(int srcIconId)
{
    if (srcIconId < 0 || srcIconId >= ICON_MAP_SIZE)
        return 0;
    return g_icon_iid[srcIconId];
}

int goblin::gfx_probe::source_iconid(uint32_t runtimeIconId)
{
    for (int i = 0; i < ICON_MAP_SIZE; ++i)
        if (g_icon_iid[i] != 0 && g_icon_iid[i] == runtimeIconId)
            return i;
    // Before remapping (or during a guarded partial load), accept a generated
    // source id directly, but reject unrelated vanilla frames.
    for (int i = 0; i < goblin::generated::MAP_ICON_TAG_COUNT; ++i)
        if (goblin::generated::MAP_ICON_TAGS[i].srcIconId == static_cast<int>(runtimeIconId))
            return static_cast<int>(runtimeIconId);
    return -1;
}

uint32_t goblin::gfx_probe::native_character_id(int sourceIconId)
{
    for (int i = 0; i < goblin::generated::MAP_ICON_TAG_COUNT; ++i)
        if (goblin::generated::MAP_ICON_TAGS[i].srcIconId == sourceIconId)
            return inject_base() + static_cast<uint32_t>(i);
    return 0;
}


uintptr_t goblin::gfx_probe::create_native_icon_instance(int sourceIconId, uint16_t depth,
                                                          void *live_ctx, uint32_t frame)
{
    const uint64_t ctx = reinterpret_cast<uint64_t>(live_ctx);
    if (!ctx || !goblin::maphover::map_dialog()) return 0;

    int tag_index = -1;
    for (int i = 0; i < goblin::generated::MAP_ICON_TAG_COUNT; ++i)
        if (goblin::generated::MAP_ICON_TAGS[i].srcIconId == sourceIconId)
        {
            tag_index = i;
            break;
        }
    if (tag_index < 0) return 0;

    uintptr_t child = 0;
    __try
    {
        // Per-icon cached tag (movie heap; the cache is cleared per movie load
        // at the 13507 hook, so no stale cross-generation pointers). Records
        // decode their placement FROM the tag, so a shared tag is only legal
        // while no in-flight record can observe a later depth re-patch - the
        // factory guarantees that by finishing each record (materialize +
        // neutralize) before the next request touches this icon's tag. The
        // one-shot-tag era (~9.4k extra movie-heap allocations per session)
        // existed to survive deferred decode and is no longer needed.
        uint64_t tag = g_native_place_tags[tag_index];
        if (!tag)
        {
            const auto &e = goblin::generated::MAP_ICON_TAGS[tag_index];
            tag = build_clean_place_tag(static_cast<uint16_t>(inject_base() + tag_index),
                                        depth, e.matrix, e.matrixLen);
            g_native_place_tags[tag_index] = tag;
        }
        if (tag)
            safe_copy(reinterpret_cast<void *>(tag + 0xa), &depth, 2);
        const uint64_t vt = tag ? rq(tag) : 0;
        const uint64_t execute = vt ? rq(vt + 0x30) : 0;
        if (!execute) return 0;
        using ExecFn = void *(void *, void *, uint32_t);
        reinterpret_cast<ExecFn *>(execute)(reinterpret_cast<void *>(tag),
                                             reinterpret_cast<void *>(ctx),
                                             frame);
        const uint64_t list = rq(ctx + 0x28);
        const uint64_t count = rq(ctx + 0x30);
        if (looks_heap(list) && count > 0 && count < 4096)
            for (uint64_t i = 0; i < count; ++i)
            {
                const uint64_t node = rq(list + i * 8);
                if (looks_heap(node) && rd32(node + 0x14) == depth)
                {
                    child = static_cast<uintptr_t>(node);
                    break;
                }
            }
    }

    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        child = 0;
    }
    return child;
}

// Place an empty MovieClip child (a DefineSprite instance, flags1=0x00) at `depth`
// on the live sprite-root ctx and return the materialized display-list node, or 0.
// Unlike create_native_icon_instance (a bitmap-image leaf, which needs the deferred
// record-materialization driver), a DefineSprite placement instantiates a MovieClip
// directly - it exposes the DrawingContext getter (vtbl+0x2a0) the solid-fill spike
// draws into. `spriteCharId` should be a DefineSprite in the worldmap movie (171 =
// the icon sprite). Dev-only (used by the debug_logging-gated solid-fill spike).

bool goblin::gfx_probe::remove_native_icon_record(uint16_t depth, void *ctx, uint32_t frame)
{
    const uint64_t c = reinterpret_cast<uint64_t>(ctx);
    if (!c) return false;
    bool ok = false;
    __try
    {
        const uint64_t tag = build_remove_tag(depth);
        const uint64_t vt = tag ? rq(tag) : 0;
        const uint64_t execute = vt ? rq(vt + 0x30) : 0;
        if (execute)
        {
            using ExecFn = void *(void *, void *, uint32_t);
            reinterpret_cast<ExecFn *>(execute)(reinterpret_cast<void *>(tag),
                                                reinterpret_cast<void *>(c), frame);
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    return ok;
}

// The anon "?" is one of the injected icons (its source iconId is goblin::generated::ANON_ICON_ID).
// Kept for the apply_loot_settings pre-load placeholder path.
uint32_t goblin::gfx_probe::anon_dynamic_iconid()
{
    return injected_iconid(static_cast<int>(goblin::generated::ANON_ICON_ID));
}

void goblin::gfx_probe::injected_iid_range(uint32_t &lo, uint32_t &hi)
{
    lo = g_iid_lo.load(std::memory_order_relaxed);
    hi = g_iid_hi.load(std::memory_order_relaxed);
}

bool goblin::gfx_probe::icons_injected()
{
    return g_qmark_injected.load(std::memory_order_relaxed);
}

void goblin::gfx_probe::v3_on_map_close()
{
    // Icon definitions and resources stay loaded across map opens, so nothing of the injection is
    // undone here. What DOES have to go is our cached movieDef pointer: it is latched once (see
    // lookup_detour) and then dereferenced by tick() from the background watcher every 100ms-2s,
    // forever - while the worldmap movie itself is re-loaded (seh_inject_sprite171 recomputes the
    // charId base "for every loaded movie"). A movieDef that has been freed since we latched it
    // makes the collision self-heal read a dead resource dict and bump the injected charId base off
    // garbage. Dropping it here costs one re-latch on the next charId-171 lookup.
    g_moviedef.store(0, std::memory_order_relaxed);
    g_dict_dumped.store(false, std::memory_order_relaxed);
}


void goblin::gfx_probe::tick()
{
    // Called from the DLL's background watcher thread (setup_mod), NOT the overlay's Present hook - so the
    // collision self-heal and (dev) diagnostics run independently of menu_enabled. Icon/resource injection
    // itself is UNCONDITIONAL and happens at LOAD (the sprite-loader hook), independent of this tick; this
    // only runs the functional collision self-heal (always) + the read-only dumps (`probe` = debug_logging).
    // The background loop already paces us (100ms-2s), so no frame throttle here; the work below is one-shot.
    const bool probe = goblin::config::debugLogging;
    goblin::check_patched_slots(); // audit watch; no-op without debug logging
#if MFG_STALL_PROFILER
    goblin::watch::pump();        // arm any queued hardware write watch (never on its own thread)
#endif

    // V3 stage 1: TOP-DOWN layer discovery from the live WorldMapDialog. maphover
    // publishes the MapArea (r8 of the per-frame hook); dialogBase = MapArea -
    // 0x27D8 (map_layer RE). The projection broadcast fn 0x1409C38D0 scales ~10
    // layer widgets at fixed dialog offsets; one is the icon layer. For each we
    // scan the widget for a sub-object holding a display list (+0x28 base / +0x30
    // count) and log the node count - the layer with hundreds/thousands of nodes
    // is the marker layer (our stage-2 insertion target). One-shot per map session.
    if (probe)
    {
        uint64_t area = (uint64_t)goblin::maphover::map_dialog();
        bool map_open = area != 0;
        if (map_open && g_layer_dumped.load(std::memory_order_relaxed) == 0 &&
            g_layer_dumped.exchange(1, std::memory_order_relaxed) == 0)
        {
            __try
            {
                // Wide scan of the dialog object: every qword in [0, 0x3A00) whose
                // target is a heap object with a sane display list (+0x28 base /
                // +0x30 count). The layer widgets + the movie root live among these;
                // the entry with the most nodes is the marker layer (stage-2 target).
                uint64_t dialog = area - 0x27D8;
                spdlog::info("[v3layer] MapArea=0x{:X} dialog=0x{:X} - wide pointer scan:", area, dialog);
                uint64_t best_p = 0, best_cnt = 0, best_off = 0;
                int hits = 0;
                for (uint64_t off = 0; off < 0x3A00; off += 8)
                {
                    uint64_t p = rq(dialog + off);
                    if (!looks_heap(p)) continue;
                    uint64_t lb = rq(p + 0x28), lc = rq(p + 0x30);
                    if (!looks_heap(lb) || lc == 0 || lc >= 20000) continue;
                    // sanity: first node must be a heap object too (real display list)
                    uint64_t n0 = rq(lb);
                    if (!looks_heap(n0)) continue;
                    if (hits++ < 24)
                        spdlog::info("[v3layer]   dialog+0x{:X} -> 0x{:X} list=0x{:X} count={}",
                                     off, p, lb, lc);
                    if (lc > best_cnt) { best_cnt = lc; best_p = p; best_off = off; }
                }
                spdlog::info("[v3layer] BUSIEST: dialog+0x{:X} -> 0x{:X} count={} ({} list-holders total)",
                             best_off, best_p, best_cnt, hits);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                spdlog::warn("[v3layer] AV during dialog scan (skipped, game unharmed)");
            }
        }
        else if (!map_open)
        {
            g_layer_dumped.store(0, std::memory_order_relaxed); // re-arm for the next open
        }
    }

    uint64_t sd = g_found_sd.load(std::memory_order_relaxed);
    if (!sd)
    {
        sd = locate_sprite171();
        if (!sd)
            return;
        g_found_sd.store(sd, std::memory_order_relaxed);
        spdlog::debug("[gfxprobe] *** SpriteDef-171 @ 0x{:X}: frameCount={} ***", sd, rd32(sd + OFF_FRAMECOUNT));
#ifdef MFG_DUMP_FRAMES
        if (probe)
            dump_obj("SpriteDef-171", sd);
#endif
    }

    // Icon/logo register + frame append + remap all happen at LOAD (sprite-loader hook), before pins
    // are built - nothing to do here. tick() only runs the collision self-heal + read-only diagnostics below.

    // Once a lookup has handed us movieDef: the collision SELF-HEAL below is functional (always runs);
    // the resource-dict dumps are dev-only (gated by `probe`). One-shot, never writes.
    {
        // A diagnostic block here loaded two call counters into statics and, when they changed,
        // updated the statics - no log, no state, nothing else. One of the two (g_adddisp_calls)
        // had also stopped being incremented when the AddDisplayObject hook was removed.
        uint64_t md = g_moviedef.load(std::memory_order_relaxed);
        if (md && !g_dict_dumped.exchange(true, std::memory_order_relaxed))
        {
#ifdef MFG_DUMP_FRAMES
            if (probe)
            {
                dump_dict(md);
                // side-by-side: gfx-native "?" (13507) vs our first runtime-injected bitmap (BASE)
                dump_charid_node(md, 13507, "NATIVE-gfx");
                dump_charid_node(md, inject_base(), "ADDED");
            }
#endif

            // Collision guard + SELF-HEAL. Our injected charIds live in the image manager, NOT this READ
            // dict, so a collision = a NATIVE charId sitting in our window [base, base+COUNT+1) (icons +
            // logo). If found, bump the base above it and reset the load one-shots so the NEXT worldmap
            // (re)open re-injects at a clear base (best-effort: takes effect when the movie reloads).
            uint64_t cont = md + OFF_MOVIEDEF_DICT;
            uint64_t dbase = rq(cont);
            uint32_t dcount = rd32(cont + 8);
            if (looks_heap(dbase) && dcount && dcount < 100000)
            {
                uint32_t base = inject_base();
                uint32_t cnt = (uint32_t)goblin::generated::MAP_ICON_TAG_COUNT + 1; // icons + logo
                uint32_t native_max = 0, in_window = 0, window_max = 0;
                for (uint32_t k = 0; k < dcount; ++k)
                {
                    uint64_t node = rq(dbase + (uint64_t)k * 16);
                    uint32_t c = node ? rd32(node + OFF_NODE_CHARID) : 0;
                    if (c == 0) continue;
                    if (c >= base && c < base + cnt) { ++in_window; if (c > window_max) window_max = c; }
                    else if (c < base && c > native_max) native_max = c;
                }
                if (in_window > 0)
                {
                    uint32_t newbase = window_max + INJECT_BASE_MARGIN;
                    g_inject_base.store(newbase, std::memory_order_relaxed);
                    g_qmark_injected.store(false, std::memory_order_relaxed); // allow re-inject at sprite-171
                    g_logo_placed.store(false, std::memory_order_relaxed);
                    g_dict_dumped.store(false, std::memory_order_relaxed);
                    spdlog::warn("[icons] charId COLLISION: {} native ids in [{}, {}) -> bumped base to {}; "
                                 "will re-add on next worldmap open (reopen the map).",
                                 in_window, base, base + cnt, newbase);
                }
            }
        }
    }

}



// A release_resolver_out() stood here: it dropped the AddRef'd ref the name resolver leaves in an out
// SceneObjProxy's inner CSScaleformValue (objIface @out+0x40, flags @out+0x48, handle @out+0x50;
// Release = objIface->vt[0x10] when flags bit6 is set - the game's own FUN_140d7f9d0). Its comment
// read as a live requirement ("without this the leaked ref corrupts refcounts and crashes at map
// teardown") and that requirement IS real - but nothing in this file called this function, and this
// file resolves no proxies. The obligation is discharged where the resolving actually happens: every
// call site there passes the out buffer to the engine's own proxy destructor at 0xD7F850 straight
// after reading it. If a resolver call is ever added HERE, do the same - do not re-add a private copy.







void goblin::gfx_probe::setup()
{
    // Icon/resource injection is UNCONDITIONAL (it's how icons render without a gfx), so the load-time
    // hooks below are ALWAYS armed. The RM2::Execute hook is NOT a dev-only trace: it is armed with
    // kNativeMarkers (1 in every shipping build) and carries v3_native_factory_pulse - our marker
    // instances are materialized from inside that callback. What debug_logging gates is only the
    // logging inside it; the SpriteDef/dict dumps need the compile key MFG_DUMP_FRAMES on top, and
    // that macro is not defined anywhere in the tree.
    try
    {
        // Resolve the game's own CRT allocator FIRST: every buffer we hand to Scaleform (frame array,
        // tag arrays, tags) must be freeable by the game's _free_base, or its teardown corrupts the heap.
        resolve_game_malloc();


        modutils::hook<CtorFn>(
            {.aob = "45 33 C0 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? "
                    "C7 41 08 01 00 00 00 4C 89 41 10"},
            ctor_detour, o_ctor);

        // (The AddDisplayObject hook was REMOVED 2026-07-28. It existed only to capture movieDef, and
        //  across every instrumented run its detour never fired once - the target at 0x140f01b10 is
        //  AvmSprite::AddDisplayObject, the AS2 path, while this game's worldmap movie is AS3. movieDef
        //  comes from the char-lookup detour instead, which does fire. One less patch in the game.)
        modutils::hook<LookupFn>(
            {.aob = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 8B DA "
                    "48 8B F1 E8 F4 F8 FF FF"},
            lookup_detour, o_lookup);

        // Per-define dict registrar: track each movie's live native max charId (scoped by ctx) so the
        // injected-bitmap base is computed at runtime, above every native id, no build-time guess.
        // Must be armed before the worldmap movie loads (setup runs at DLL load via the offline loader).
        modutils::hook<RegistrarFn>(
            {.aob = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 41 8B 00 48 8B F9 48 8B 49 38"},
            registrar_detour, o_registrar); // dict registrar (was file VA 0x1411cf250)
        modutils::hook<LosslessFn>(
            {.aob = "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 48 8B 99 "
                    "18 04 00 00"},
            lossless_detour, o_lossless);
        modutils::hook<SpriteLoaderFn>(
            {.aob = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 99 18 04 "
                    "00 00 48 8B F9 48 85 DB"},
            spriteloader_detour, o_spriteloader);
        spdlog::info("[gfxprobe] icon handlers ready (ctor + movieDef read + registrar + lossless + "
                     "DefineSprite-loader)");

        // The V3 native-marker factory needs the RemoveObject2::Execute hook below, so it is installed
        // with that feature rather than with logging. Two more detours used to live here (the Scaleform
        // name resolver and the PlaceObject2 loader); they were proven in-game to be the cause of the
        // black-tile defect and are gone - see docs/research_retired_native_ui_experiments.md.
        if (goblin::variants::kNativeMarkers)
        {
            // RemoveObject2::Execute - the factory pulse rides on it: our marker instances are
            // materialized from inside this callback, while its timeline context is live.
            // The name-resolver and PlaceObject2-loader detours that used to sit here are GONE: they
            // belonged to the retired settings re-host and were proven in-game to be what rendered map
            // tiles as black squares. See docs/research_retired_native_ui_experiments.md before
            // reintroducing anything that hooks 0x14074a2f0 or 0x11E23B0.
            modutils::hook<ExecFn>(
                {.address = reinterpret_cast<void *>((uint64_t)GetModuleHandleW(nullptr) + RVA_FN_REMOVEOBJECT2_ADDSNAPSHOT)},
                rm2exec_detour, o_rm2exec);
        }
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[gfxprobe] setup failed: {}", e.what());
    }
}

// menu_icons_ready() lived here (see the note in the header): a readiness signal whose flag was
// never raised, with no caller to be misled by it.
