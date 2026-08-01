#include "goblin_stall_probe.hpp"
#include "goblin_config.hpp"

namespace goblin::watch { void request(uintptr_t address, unsigned long thread_id); }
#include "goblin_build_variants.hpp"
#include "goblin_config_schema.hpp"
#include "goblin_native_menu.hpp"
#include "goblin_own_movie.hpp"
#include "goblin_sfimage.hpp"
#include "goblin_map_icons.hpp"
#include "generated_shared/goblin_menu_icon_tags.hpp"
#include "generated_shared/goblin_map_icons.hpp"
#include "goblin_maphover.hpp"
#include "goblin_mapproject.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_inject.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include "miniz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <intrin.h> // _ReturnAddress

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <tlhelp32.h>

// Sampling profiler for one game thread. capture() is called on the thread we
// want to observe (map UI thread); the actual sampling runs on a detached helper
// thread so the observed thread keeps running the stall we want to measure.
// While the target is paused we only read its context and scan its frozen stack
// for return addresses into the exe image - no allocation, no logging, so the
// helper can never block on a lock the paused thread holds.
namespace
{
    std::atomic<bool> g_running{false};

    // ── pin-registration cost counters ──────────────────────────────────
    // Pass-through wraps on the three per-marker map-widget virtual methods
    // (vtable slots at 0x142cbc840/848/850; entries 0x1410dbb70 / 0x1410dbea0 /
    // 0x1410dc260 in v1.16) and the typed-find walk (0x14113feb0) they drive.
    // These fns are virtual-dispatched (no static xrefs), so recording the REAL
    // return addresses here is the only reliable way to identify the per-marker
    // driver loop for the soft-populate design. Counting only happens while a
    // capture window is open; otherwise each wrap is one relaxed load + branch.
    // Slots 4/5 (2026-07-14 round 2): the per-frame while-map-open hot spots the
    // sampler surfaced - the child-step loop (0x1411d3980), reached through a
    // tail-call from the display visibility setter (0x1411c1c20), and the
    // transform getter (0x14117e140, full matrix decompose with sqrt+atan2 when
    // its cache ptr [this+0x50] is null, cheap copy when cached).
    // Slots 6-8 (release-in-fade expedition): the GFx render batch internals.
    // item-proc (0x140d716a0) runs per movie-slot on a WORKER; next-capture
    // (0x1411577b0) applies the accumulated display-tree changelist (teardown of
    // ~9k objects lands here per the working theory); display (0x14115cde0) draws.
    // The timings decide whether the close freeze is changelist-apply or draw.
    // Slot 9: the batch job entry itself (0x140d7d720, runs on a worker). Its max
    // vs the ~140ms scheduler wait separates "long job" from "job stuck in queue"
    // (priority inversion in the task system).
    constexpr int N_REG = 10;
    const char *const REG_NAME[N_REG] = {"widget-a",   "widget-b",     "widget-c",
                                         "typed-find", "child-step",   "xform-get",
                                         "item-proc",  "next-capture", "movie-display",
                                         "batch-job"};
    using Fn4 = void *(void *, void *, void *, void *);
    Fn4 *o_reg[N_REG] = {};
    std::atomic<bool> g_count{false};
    std::atomic<uint64_t> g_reg_calls[N_REG];
    std::atomic<uint64_t> g_reg_ticks[N_REG];
    std::atomic<uint64_t> g_reg_max[N_REG]; // worst single call (attributes one-off stalls)

    constexpr size_t RET_SLOTS = 32; // per-fn return-address histogram (linear probe)
    struct RetSlot
    {
        std::atomic<uintptr_t> addr{0};
        std::atomic<uint32_t> cnt{0};
    };
    RetSlot g_ret[N_REG][RET_SLOTS];

    // V3 native-marker layer discovery. The hooked child propagation loop receives
    // a live Scaleform display object in RCX. Container subclasses own the vector
    // walked by vtbl+0x348 at +0xd8 (count at +0xe0, stride 0x10). Keep only the
    // busiest object seen during each capture window. For a large object, scan
    // the object and three sampled children for the distinct PlaceObject Execute
    // context layout: list@+0x28/+0x30, pointer stride 8, and the ctx/list node
    // sharing qword 0. This distinguishes the real timeline display list from
    // the 16-byte display-object vector and from hash tables.
    constexpr int V3_LINK_CAP = 16;
    struct V3LinkSnapshot
    {
        uintptr_t root = 0;
        uintptr_t source_off = 0;
        uintptr_t candidate = 0;
        uintptr_t list = 0;
        uintptr_t first_node = 0;
        uint64_t count = 0;
        uint64_t shared_key = 0;
        uint32_t first_depth = 0;
        bool indirect = false;
    };
    struct V3ContainerSnapshot
    {
        uintptr_t parent = 0;
        uintptr_t entries = 0;
        uintptr_t ret = 0;
        uint64_t count = 0;
        uint64_t parent_vt = 0;
        uint64_t parent_28 = 0;
        uint64_t parent_30 = 0;
        uint64_t parent_b8 = 0;
        uint64_t parent_name[3] = {};
        uintptr_t child[3] = {};
        uintptr_t child_aux[3] = {};
        uint64_t child_vt[3] = {};
        uint64_t child_28[3] = {};
        uint64_t child_30[3] = {};
        uint64_t child_name[3][3] = {};
        V3LinkSnapshot link[V3_LINK_CAP] = {};
        uint32_t link_count = 0;
    };
    V3ContainerSnapshot g_v3_container;
    std::atomic<uint64_t> g_v3_published_count{0};
    std::atomic_flag g_v3_snapshot_lock = ATOMIC_FLAG_INIT;

    // Exact DisplayObjContainer mutation path. FUN_14113e970 receives the
    // container's 16-byte child vector (RCX), its owner (RDX), sorted insertion
    // index (R8), and the newly-created display object (R9). Higher-level map/UI
    // builders call this insert core directly, bypassing the 0x14113eb60
    // PlaceObject replace wrapper.
    // Recording the before/after count proves that the 6468-object map layer is
    // built through this API and preserves the arguments needed for a later,
    // deliberately-scoped native insertion experiment. This one probe runs for
    // the whole debug-logging session because insertion precedes capture start.
    using V3AddFn = void(void *, void *, void *, void *);
    V3AddFn *o_v3_add = nullptr;

    struct V3AddSnapshot
    {
        uintptr_t vector = 0;
        uintptr_t parent = 0;
        uintptr_t child = 0;
        uintptr_t ret = 0;
        uint64_t before_count = 0;
        uint64_t after_count = 0;
        uint64_t insert_index = 0;
        uint64_t child_vt = 0;
        uint32_t child_depth = 0;
        bool owner_match = false;
    };
    V3AddSnapshot g_v3_add;
    std::atomic<uint64_t> g_v3_add_calls{0};
    std::atomic<uint64_t> g_v3_add_grew{0};
    std::atomic<uint64_t> g_v3_add_same{0};
    std::atomic<uint64_t> g_v3_add_owner_match{0};
    std::atomic<uint64_t> g_v3_add_max_count{0};
    std::atomic_flag g_v3_add_lock = ATOMIC_FLAG_INIT;

    // PlaceObject add path used by the V3 custom-icon spike. Unlike the generic
    // insert-core hook above, this level still has the character id. Filter to
    // our spike's (first injected charId, depth 24) pair and retain the ready
    // DisplayObject it creates.
    using V3PlaceFn = void(void *, void *, void *, void *, uint64_t);
    V3PlaceFn *o_v3_place = nullptr;
    struct V3CustomSnapshot
    {
        uintptr_t vector = 0;
        uintptr_t parent = 0;
        uintptr_t placement = 0;
        uintptr_t child = 0;
        uintptr_t ret = 0;
        uint64_t before_count = 0;
        uint64_t after_count = 0;
        uint64_t child_vt = 0;
        uint64_t child_28 = 0;
        uint64_t child_30 = 0;
        uint64_t child_parent = 0;
        uint32_t depth = 0;
        uint32_t char_id = 0;
        uint32_t placement_flags = 0;
        uint32_t child_flags = 0;
    };
    V3CustomSnapshot g_v3_custom;
    std::atomic<uint64_t> g_v3_custom_hits{0};
    std::atomic_flag g_v3_custom_lock = ATOMIC_FLAG_INIT;

    // One-shot visual experiment. FUN_1410c8440 is the high-level attach API:
    // given its display-list wrapper, a child and an index, it removes the child
    // from any old parent through the engine's own path, inserts it into the new
    // parent's +0xd8 vector, and fixes parent/depth/flags/transform state.
    using V3AttachFn = void(void *, void *, uint32_t);
    V3AttachFn *o_v3_attach = nullptr;
    using V3AttachMovieFn = uint32_t(void *, void *, void *, const char *, void *, int, void *);
    V3AttachMovieFn *o_v3_attach_movie = nullptr;
    constexpr uint32_t V3_MATRIX_SLOTS = 8;
    constexpr uint32_t V3_CANDIDATE_CAP = 128;
    std::atomic<uintptr_t> g_v3_matrix_children[V3_MATRIX_SLOTS]{};
    float g_v3_matrix_base_tx[V3_MATRIX_SLOTS]{};
    float g_v3_matrix_base_ty[V3_MATRIX_SLOTS]{};
    std::atomic<uint64_t> g_v3_matrix_started_ms{0};
    std::atomic<uint32_t> g_v3_matrix_state{0}; // 0 collecting, 1 attached/attempted

    struct V3BuildParentStat
    {
        uintptr_t wrapper = 0;
        uintptr_t parent = 0;
        uint64_t calls = 0;
        uint64_t min_count = UINT64_MAX;
        uint64_t max_count = 0;
    };
    struct V3BuildCorrelation
    {
        uint32_t depth = 0;
        void *owner = nullptr;
        void *ctx = nullptr;
        uint64_t total_calls = 0;
        uint32_t parent_count = 0;
        V3BuildParentStat parents[V3_CANDIDATE_CAP]{};
    };
    thread_local V3BuildCorrelation g_v3_build_corr;

    struct V3AttachMovieContext
    {
        bool active = false;
        char export_name[48]{};
        int requested_depth = 0;
        uint64_t init_count = 0;
        char key[4][24]{};
        uint64_t value[4][2]{};
    };
    thread_local V3AttachMovieContext g_v3_movie_ctx;

    void v3_note_build_attach(uintptr_t wrapper, uintptr_t parent, uint64_t count)
    {
        auto &corr = g_v3_build_corr;
        if (corr.depth == 0)
            return;
        ++corr.total_calls;
        for (uint32_t i = 0; i < corr.parent_count; ++i)
        {
            auto &s = corr.parents[i];
            if (s.parent != parent)
                continue;
            ++s.calls;
            s.wrapper = wrapper;
            s.min_count = (std::min)(s.min_count, count);
            s.max_count = (std::max)(s.max_count, count);
            return;
        }
        if (corr.parent_count < V3_CANDIDATE_CAP)
        {
            auto &s = corr.parents[corr.parent_count++];
            s.wrapper = wrapper;
            s.parent = parent;
            s.calls = 1;
            s.min_count = count;
            s.max_count = count;
        }
    }

    struct V3Candidate
    {
        uintptr_t wrapper = 0;
        uintptr_t parent = 0;
        uintptr_t caller = 0;
        uint64_t count = 0;
    };
    V3Candidate g_v3_candidates[V3_CANDIDATE_CAP]{};
    uint32_t g_v3_candidate_count = 0;
    std::atomic_flag g_v3_candidate_lock = ATOMIC_FLAG_INIT;

    std::atomic<uintptr_t> g_v3_target_wrapper{0};
    std::atomic<uintptr_t> g_v3_target_parent{0};
    std::atomic<uint64_t> g_v3_target_count{0};
    std::atomic<int> g_v3_target_layer{-1};
    // True once the map closed after the current target was published. Gates the
    // parent-changed retarget arm: within ONE open session another 100+ child
    // WorldMapItem list must NOT steal the target (observed: a count=180 list
    // arriving 450ms after the build burst hijacked it and orphaned our batch).
    std::atomic<bool> g_v3_map_closed{false};
    // GetTickCount64() stamped when on_map_close ran. A quick reopen is proven the moment the map
    // dialog's per-frame Update advances maphover::last_activity_ms() PAST this stamp: the dialog
    // resumed, so the map is live NOW, as opposed to the close-then-teardown window where a stale
    // parent still looks heap-valid. This is what gates the safe clear of g_v3_map_closed for
    // reopens that emit no attachMovie burst (the game reused the movie), and it replaces an
    // earlier child-vtable heuristic that the shared marker vtable fooled into relinking a dying
    // parent - heap corruption on fast reopens.
    std::atomic<uint64_t> g_v3_close_ms{0};

    // Lever A recon probe state (declared early so on_map_close can reset it).
    // Solid-fill route-(c) spike: one-shot per build burst, reset on real map close.
    std::atomic<int> g_solidfill_spike_done{0};

    // Factory BATCH: one pulse queues up to V3_FACTORY_BATCH timeline records
    // on the live ctx, then runs the engine's record-materialization driver on
    // them itself (see g_v3_mat_driver below) - the whole batch is created,
    // captured and transferred inside ONE pulse. RM2 pulses are a finite
    // build-burst pool (~2 per remaining native widget, ~3900/session), so at
    // batch=16 the pool covers ~60k markers - far above any profile's needs.
    constexpr size_t V3_FACTORY_BATCH = 16;

    // ERR-look placement for the "cleared" badge twin. The badge frame is
    // authored like every icon (full footprint, centred), so at transfer its
    // basis/pivot are rescaled to ~26px over the ~92px icon and re-centred
    // up-left of the anchor - the effective centre (-335,-336) twips matches
    // the gfx placement in build_vanilla_gfx.py. Baked into the captured
    // basis/pivot once, so zoom counter-scale and show/hide stay uniform.
    constexpr float V3_BADGE_SCALE = 39.0f / 92.0f; // user-tuned: 1.5x the gfx look
    constexpr float V3_BADGE_OFF_X = -335.0f;
    constexpr float V3_BADGE_OFF_Y = -336.0f;
    struct V3FactorySlot
    {
        uint32_t depth = UINT32_MAX; // requested timeline depth (unique match key)
        uint32_t char_id = 0;
        uintptr_t node = 0;  // timeline record from create_native_icon_instance
        uintptr_t child = 0; // captured display child (PlaceObject hook)
        uint32_t age = 0;    // pulses waited for materialization
        uintptr_t root = 0;
        float base_tx = 0.0f, base_ty = 0.0f;
        float basis[4] = {};
        bool held = false;
        // The queue entry this slot consumed (pending_index advances at issue).
        goblin::NativeMarkerPoint point{};
        float map_x = 0.0f, map_z = 0.0f;
    };
    thread_local V3FactorySlot g_v3_factory_slots[V3_FACTORY_BATCH];
    thread_local uint32_t g_v3_factory_active = 0; // slots with node != 0

    // Record-materialization driver (RE: scratch/re_materialize_driver.md,
    // FUN_1411bf1b0 in v2.6.2.0): walks the ctx's record list and creates a
    // real child for EVERY pending place record via the sprite's vtable+0x3b8
    // path. PO Executes only insert RECORDS; the engine itself runs this
    // driver solely on sprite frame advance, and gotoAndStop'd composites
    // never advance again - that was the hard ~1891-creations-per-session
    // ceiling. Calling it ourselves right after queueing a batch materializes
    // all K synchronously. sprite back-ref lives at ctx+0x58.
    using V3MatDriverFn = void(void *, void *); // (execCtx, sprite)
    V3MatDriverFn *g_v3_mat_driver = nullptr;

    // Lever C self-detach primitives (the engine's own remove-from-container path,
    // as used by the high-level reparent FUN_1410c8440):
    //   find-index  FUN_14113f8d0(childVecHeader=owner+0xd8, child) -> index or -1
    //   remove-at   FUN_1410c87c0(wrapper, index) -> erase child at index from
    //               *(wrapper+0x18) (=owner); unlinks its render node, clears
    //               child+0x38 (parent), marks the container dirty and drops the
    //               container's reference (freeing the child if it was the last).
    using V3RemoveAtFn = void(void *, uint32_t);
    V3RemoveAtFn *g_v3_remove_at = nullptr;
    std::atomic<uint32_t> g_v3_last_detached{0};
    std::atomic<uint64_t> g_v3_last_detach_us{0};

    // Scaleform GFx DrawingContext primitives (the C++ backing of AS Graphics),
    // used by the dev-only solid-fill spike. A SOLID color fill needs NO GPU
    // texture (unlike beginBitmapFill, which hit a texture wall), so drawing one
    // filled rectangle proves whether route (c) - a native settings panel built
    // from our own display objects (backgrounds, progress bars, checkbox frames) -
    // is viable. ctx is fetched from a MovieClip child via its vtbl+0x2a0 getter.
    //   begin      FUN_14119cb50(ctx, char clear)   - open/clear the drawing
    //   beginFill  FUN_14119cc20(ctx, u32 argb)     - SOLID color 0xAARRGGBB, no texture
    //   moveTo     FUN_14119d690(ctx, int, int)     - twips
    //   lineTo     FUN_14119d7a0(ctx, int, int)     - twips
    //   endFill    FUN_14119d650(ctx)
    //   shapeReset FUN_14119d0c0(ctx)               - clear accumulator (begin returns 0 on a fresh ctx)
    using DcBeginFn      = char(void *, char);
    using DcBeginFillFn  = void(void *, uint32_t);
    using DcMoveToFn     = void(void *, int, int);
    using DcLineToFn     = void(void *, int, int);
    using DcEndFillFn    = void(void *);
    using DcShapeResetFn = void(void *);
    DcBeginFn      *g_dc_begin      = nullptr;
    DcBeginFillFn  *g_dc_beginfill  = nullptr;
    DcMoveToFn     *g_dc_moveto     = nullptr;
    DcLineToFn     *g_dc_lineto     = nullptr;
    DcEndFillFn    *g_dc_endfill    = nullptr;
    DcShapeResetFn *g_dc_shapereset = nullptr;

    // Leaf 1: scan the parent's logical child vector (owner+0xd8: base@[0],
    // count@+0xe0, entry stride 0x10 with the child ptr at +0) and record the
    // indices whose child ptr is in the pre-sorted `sorted[0..n)` set (our
    // markers). Pure raw memory + POD only, so SEH is legal here (no C++ unwind).
    // Returns the count of ascending indices written to out_idx (capped). `out_faulted` (POD, so SEH stays
    // legal) is set to 1 if the scan took an AV; the caller must then remove NOTHING - see below.
    uint32_t v3_detach_scan(uintptr_t parent, const uintptr_t *sorted, uint32_t n,
                            uint32_t *out_idx, uint32_t out_cap, uint32_t *out_faulted)
    {
        uint32_t found = 0;
        if (out_faulted)
            *out_faulted = 0;
        __try
        {
            uintptr_t base = *reinterpret_cast<uintptr_t *>(parent + 0xd8);
            uint32_t count = *reinterpret_cast<uint32_t *>(parent + 0xe0);
            if (!base || !count || count > 300000)
                return 0;
            for (uint32_t i = 0; i < count && found < out_cap; ++i)
            {
                uintptr_t child =
                    *reinterpret_cast<uintptr_t *>(base + static_cast<uint64_t>(i) * 0x10);
                if (!child)
                    continue;
                uint32_t lo = 0, hi = n; // binary search in sorted[]
                while (lo < hi)
                {
                    uint32_t mid = lo + ((hi - lo) >> 1);
                    if (sorted[mid] < child)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                if (lo < n && sorted[lo] == child)
                    out_idx[found++] = i;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Do NOT hand back the partial list. An AV here means the child vector we were walking
            // changed shape under us (freed/reallocated), so the indices collected before the fault
            // describe a layout that no longer exists - and remove-at drops the container's reference,
            // freeing whatever child now sits at that index. That frees an engine child we never owned:
            // heap corruption, and by then nothing points back here. Removing nothing is always safe -
            // our children simply stay attached and the next pass retries against a settled vector.
            found = 0;
            if (out_faulted)
                *out_faulted = 1;
        }
        return found;
    }

    // Leaf 2: remove the collected indices via the engine's remove-at primitive,
    // DESCENDING (out_idx is ascending) so each surviving index stays valid and the
    // per-remove tail memmove shrinks. Removing drops the container's reference,
    // freeing the child. SEH-guarded. Returns the count removed.
    // Every index is RE-VERIFIED against the live vector immediately before its removal: read the child
    // pointer currently at that index and remove only if it is still one of ours (`sorted[0..n)`).
    //
    // Why: the scan above and this removal are two separate walks of a vector the engine owns and can
    // reshape in between - a marker that is added, removed or reordered between the two turns an index
    // into a pointer at somebody else's child, and remove-at drops the container's reference, freeing an
    // engine object we never owned. That needs no fault to happen, so the AV guard cannot catch it. It
    // runs every map frame while the viewport window is on, which matches map TILES going transparent
    // and coming back during scroll and zoom. `out_skipped` counts indices that no longer matched.
    uint32_t v3_detach_remove(uintptr_t wrapper, uintptr_t parent, const uint32_t *idx, uint32_t n,
                              const uintptr_t *sorted, uint32_t sorted_n, uint32_t *out_skipped)
    {
        uint32_t removed = 0;
        uint32_t skipped = 0;
        __try
        {
            for (uint32_t k = n; k-- > 0;)
            {
                const uintptr_t base = *reinterpret_cast<uintptr_t *>(parent + 0xd8);
                const uint32_t count = *reinterpret_cast<uint32_t *>(parent + 0xe0);
                if (!base || idx[k] >= count)
                {
                    ++skipped;
                    continue;
                }
                const uintptr_t child =
                    *reinterpret_cast<uintptr_t *>(base + static_cast<uint64_t>(idx[k]) * 0x10);
                uint32_t lo = 0, hi = sorted_n; // binary search in sorted[]
                while (lo < hi)
                {
                    const uint32_t mid = lo + ((hi - lo) >> 1);
                    if (sorted[mid] < child)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                if (lo >= sorted_n || sorted[lo] != child)
                {
                    ++skipped; // no longer ours - the vector moved since the scan
                    continue;
                }
                g_v3_remove_at(reinterpret_cast<void *>(wrapper), idx[k]);
                ++removed;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (out_skipped)
            *out_skipped = skipped;
        return removed;
    }

    uint32_t v3_guarded_materialize(void *ctx, uintptr_t sprite)
    {
        __try
        {
            g_v3_mat_driver(ctx, reinterpret_cast<void *>(sprite));
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return static_cast<uint32_t>(GetExceptionCode());
        }
    }

    struct V3NativeObject
    {
        uint64_t row_id = 0;
        int source_icon_id = -1;
        uintptr_t child = 0;
        float map_x = 0.0f;
        float map_z = 0.0f;
        float base_tx = 0.0f;
        float base_ty = 0.0f;
        float base_m[4] = {}; // authored 2x2 basis {m0,m1,m4,m5} captured at staging
        bool visible = false;
        bool attached = true; // lever B: currently linked into the marker parent (has a
                              // render node). Always true unless the viewport-window variant.
        bool ref_held = false; // lever B: we kept an extra reference at build so this
                               // child survives being detached. Only children built with
                               // the viewport window ON have it -> only they may be
                               // detached/re-attached (guards a mid-session flag flip).
    };
    struct V3NativeManager
    {
        int layer = -1;
        uintptr_t wrapper = 0;
        uintptr_t parent = 0;
        uint64_t observed_count = 0;
        uint32_t stable_frames = 0;
        bool seeded = false;
        uint64_t build_started_ms = 0;
        uint64_t next_refresh_ms = 0;
        size_t pending_index = 0;
        size_t frame_budget = 0;
        uint32_t failed = 0;
        uint32_t wrong_contexts = 0;
        uint64_t settle_count = 0;
        uint32_t settle_frames = 0;
        bool completion_reported = false;
        bool in_factory = false;
        uint64_t last_progress_ms = 0; // last creation (or seed); queue watchdog
        uint16_t next_depth = 24;      // timeline depth allocator (never reused)
        // Counter-zoom state: the marker layer's parent transform scales with the
        // map and nothing in the engine touches our raw children (they are not
        // widgets). The manager copies the live 2x2 the engine writes into a REAL
        // WorldMapItem widget of the same parent - that is the exact native
        // adaptation curve (incl. clamps), no reference zoom needed.
        float cur_fx = 1.0f;      // last applied basis factor, x row
        float cur_fy = 1.0f;      // last applied basis factor, y row
        float sample_fx = 0.0f;   // last successfully sampled widget scale
        float sample_fy = 0.0f;
        float sample_zoom = 0.0f; // MapView.zoom at that sample (zoom-ratio fallback)
        float last_dump_zoom = 0.0f; // last zoom the diagnostic probe logged at
        bool sample_logged = false;
        std::vector<goblin::NativeMarkerPoint> pending;
        std::vector<V3NativeObject> objects;
        std::unordered_map<uint64_t, size_t> by_row;
        std::unordered_set<uint64_t> queued;
        // ── dead-generation guards ───────────────────────────────────────────────────
        // The display-object vtable of THIS generation's children, captured lazily from the first
        // live one. A child our own reference kept alive across a map close, whose memory the
        // engine later freed and reused, passes every heap-range check but reads back a foreign
        // vtable here - so we never write a matrix into whatever now owns that block.
        uint64_t child_vtable = 0;
        // The parent (WorldMapItem container) vtable, captured lazily once the parent is genuinely
        // alive. Unlike child_vtable (shared by thousands of marker sprites) there is exactly one
        // such container, so a match is strong proof the cached parent pointer is still THIS live
        // parent. A freed parent reads 0 here, which is the reliable death signal.
        uint64_t parent_vtable = 0;
    };
    V3NativeManager g_v3_native;
    std::atomic<uintptr_t> g_v3_custom_child{0};
    std::atomic<uint32_t> g_v3_visual_state{0}; // 0 waiting, 1 attempted
    struct V3VisualSnapshot
    {
        uintptr_t wrapper = 0;
        uintptr_t target_parent = 0;
        uintptr_t old_parent = 0;
        uintptr_t child = 0;
        uint64_t target_before = 0;
        uint64_t target_after = 0;
        uint64_t old_before = 0;
        uint64_t old_after = 0;
        uint64_t child_parent_after = 0;
        float old_tx = 0.0f;
        float old_ty = 0.0f;
        float new_tx = 0.0f;
        float new_ty = 0.0f;
        float map_mid_x = 0.0f;
        float map_mid_y = 0.0f;
        float reference_zoom = 0.0f;
        float base_m0 = 0.0f;
        float base_m1 = 0.0f;
        float base_m4 = 0.0f;
        float base_m5 = 0.0f;
        bool positioned = false;
    };
    V3VisualSnapshot g_v3_visual;
    std::atomic<bool> g_v3_visual_published{false};

    struct V3ScaleState
    {
        uintptr_t child = 0;
        uintptr_t target_parent = 0;
        float reference_zoom = 0.0f;
        float base_m0 = 0.0f;
        float base_m1 = 0.0f;
        float base_m4 = 0.0f;
        float base_m5 = 0.0f;
        float target_tx = 0.0f;
        float target_ty = 0.0f;
        float pivot_tx = 0.0f;
        float pivot_ty = 0.0f;
    };
    V3ScaleState g_v3_scale;
    std::atomic<bool> g_v3_scale_ready{false};

    bool v3_read64(uintptr_t addr, uint64_t &out)
    {
        __try
        {
            out = *reinterpret_cast<const uint64_t *>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            return false;
        }
    }

    bool v3_read32(uintptr_t addr, uint32_t &out)
    {
        __try
        {
            out = *reinterpret_cast<const uint32_t *>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            return false;
        }
    }

    bool v3_heap_ptr(uint64_t p)
    {
        return p >= 0x10000 && p < 0x7fffffffffffULL;
    }

    bool v3_read8(uintptr_t addr, uint8_t &out)
    {
        __try
        {
            out = *reinterpret_cast<const uint8_t *>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            return false;
        }
    }

    bool v3_executable_ptr(uint64_t p)
    {
        if (!v3_heap_ptr(p))
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void *>(p), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT)
            return false;
        const DWORD protect = mbi.Protect & 0xff;
        return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
               protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool v3_copy_ascii(uintptr_t addr, char *dst, size_t capacity)
    {
        if (!v3_heap_ptr(addr) || !dst || capacity < 2)
            return false;
        __try
        {
            size_t i = 0;
            for (; i + 1 < capacity; ++i)
            {
                const unsigned char c = *reinterpret_cast<const unsigned char *>(addr + i);
                if (c == 0)
                    break;
                if (c < 0x20 || c > 0x7e)
                    return false;
                dst[i] = static_cast<char>(c);
            }
            dst[i] = 0;
            return i != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            dst[0] = 0;
            return false;
        }
    }

    uint32_t v3_attach_movie_detour(void *a1, void *a2, void *a3, const char *export_name,
                                    void *a5, int requested_depth, void *init_object)
    {
        const V3AttachMovieContext saved = g_v3_movie_ctx;
        auto &ctx = g_v3_movie_ctx;
        ctx = {};
        ctx.active = goblin::variants::kNativeMarkers;
        ctx.requested_depth = requested_depth;
        if (ctx.active)
        {
            v3_copy_ascii(reinterpret_cast<uintptr_t>(export_name),
                          ctx.export_name, sizeof(ctx.export_name));
            uint64_t entries = 0;
            if (init_object)
            {
                v3_read64(reinterpret_cast<uintptr_t>(init_object), entries);
                v3_read64(reinterpret_cast<uintptr_t>(init_object) + 8, ctx.init_count);
            }
            if (v3_heap_ptr(entries) && ctx.init_count <= 256)
            {
                const uint32_t n = static_cast<uint32_t>((std::min<uint64_t>)(ctx.init_count, 4));
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uintptr_t entry = static_cast<uintptr_t>(entries) + i * 0x38;
                    uint64_t tagged_key = 0, key_header = 0;
                    v3_read64(entry, tagged_key);
                    const uintptr_t key_node = static_cast<uintptr_t>(tagged_key & ~3ULL);
                    if (v3_heap_ptr(key_node) && v3_read64(key_node, key_header))
                    {
                        const uint64_t key_len = key_header & 0x7fffffffffffffffULL;
                        if (key_len > 0 && key_len < sizeof(ctx.key[i]))
                            v3_copy_ascii(key_node + 0xc, ctx.key[i], sizeof(ctx.key[i]));
                    }
                    v3_read64(entry + 8, ctx.value[i][0]);
                    v3_read64(entry + 16, ctx.value[i][1]);
                }
            }
        }
        const uint32_t result = o_v3_attach_movie(a1, a2, a3, export_name, a5,
                                                   requested_depth, init_object);
        g_v3_movie_ctx = saved;
        return result;
    }

    void v3_note_movie_attach(uintptr_t wrapper, uintptr_t parent, uint64_t count)
    {
        const auto &ctx = g_v3_movie_ctx;
        if (!ctx.active)
            return;
        const int live_layer = goblin::maphover::map_layer();
        const uintptr_t prev_parent = g_v3_target_parent.load(std::memory_order_relaxed);
        const int prev_layer = g_v3_target_layer.load(std::memory_order_relaxed);
        const uint64_t prev_count = g_v3_target_count.load(std::memory_order_relaxed);
        if (strcmp(ctx.export_name, "WorldMapItem") == 0)
        {
            if (parent == prev_parent)
            {
                // Same list growing: track its count. Stamp the layer LATE -
                // map_layer() is still -1 while the build burst runs, and a
                // -1-stamped target let a count=2 side clip steal the anchor
                // the moment the dialog resolved (13:39 retarget storm).
                if (count > prev_count)
                    g_v3_target_count.store(count, std::memory_order_release);
                if (prev_layer < 0 && live_layer >= 0)
                    g_v3_target_layer.store(live_layer, std::memory_order_relaxed);
                // Quick reopen that reuses the SAME movie: a burst arriving on this parent while
                // map_closed is set proves the parent is alive and the game is populating it right
                // now, which a teardown never does (it removes, it does not burst). So the gate can
                // be cleared and v3_viewport_reconcile may re-attach the children we kept. This is
                // sound where the old vtable heuristic was not: the signal is live game activity on
                // THIS parent, not a pointer that might have been recycled.
                if (g_v3_map_closed.load(std::memory_order_relaxed))
                    g_v3_map_closed.store(false, std::memory_order_relaxed);
            }
            else if (prev_parent == 0 || count > prev_count ||
                     (count >= 100 &&
                      g_v3_map_closed.load(std::memory_order_relaxed)) ||
                     (count >= 100 && prev_layer >= 0 && live_layer >= 0 &&
                      live_layer != prev_layer))
            {
                // Re-anchor ONLY on: first target / monotonically-largest list /
                // rebuild after a REAL teardown / an actual known layer switch.
                // Tiny side clips (count<100) can never steal the target.
                if (prev_parent)
                    spdlog::info("[v3movie] RETARGET parent 0x{:X} -> 0x{:X} "
                                 "count={} layer={} mapClosed={}",
                                 prev_parent, parent, count, live_layer,
                                 g_v3_map_closed.load(std::memory_order_relaxed));
                g_v3_map_closed.store(false, std::memory_order_relaxed);
                g_v3_target_wrapper.store(wrapper, std::memory_order_relaxed);
                g_v3_target_parent.store(parent, std::memory_order_relaxed);
                g_v3_target_layer.store(live_layer, std::memory_order_relaxed);
                g_v3_target_count.store(count, std::memory_order_release);
            }
        }
        if (!(count == 500 || count == 6400 || count == 6468 || count % 1000 == 0))
            return;
        spdlog::info("[v3movie] parent=0x{:X} wrapper=0x{:X} count={} export='{}' "
                     "requestedDepth={} initCount={}",
                     parent, wrapper, count,
                     ctx.export_name[0] ? ctx.export_name : "<unreadable>",
                     ctx.requested_depth, ctx.init_count);
        if (count == 6400 || count == 6468)
        {
            const uint32_t n = static_cast<uint32_t>((std::min<uint64_t>)(ctx.init_count, 4));
            for (uint32_t i = 0; i < n; ++i)
                spdlog::info("[v3movie]   init[{}] key='{}' raw=0x{:016X}/0x{:016X}",
                             i, ctx.key[i][0] ? ctx.key[i] : "?",
                             ctx.value[i][0], ctx.value[i][1]);
        }
    }

    uintptr_t v3_parent_root(uintptr_t object, uint32_t &levels)
    {
        levels = 0;
        uintptr_t current = object;
        uintptr_t seen[16]{};
        while (v3_heap_ptr(current) && levels < 16)
        {
            seen[levels++] = current;
            uint64_t next = 0;
            if (!v3_read64(current + 0x38, next) || !v3_heap_ptr(next))
                return current;
            for (uint32_t i = 0; i < levels; ++i)
                if (seen[i] == next)
                    return current;
            current = static_cast<uintptr_t>(next);
        }
        return current;
    }

    bool v3_hold_ref(uintptr_t child, uint32_t &before, uint32_t &after)
    {
        // Scaleform::RefCountNTSImpl lives at DisplayObject+0 and stores its
        // non-atomic count at +8. Native move callers hold a Ptr<> across
        // FUN_1410c8440; without this extra reference, removing the old parent
        // can destroy the child before the new parent inserts it.
        __try
        {
            auto *refs = reinterpret_cast<uint32_t *>(child + 8);
            before = *refs;
            if (before == 0 || before >= 0x10000000)
                return false;
            after = ++*refs;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            before = after = 0;
            return false;
        }
    }

    bool v3_drop_held_ref(uintptr_t child, uint32_t &before, uint32_t &after)
    {
        __try
        {
            auto *refs = reinterpret_cast<uint32_t *>(child + 8);
            before = *refs;
            // A successful attach leaves one target-parent reference in
            // addition to ours. Never manufacture a zero-count object here;
            // retain the diagnostic reference if the contract was violated.
            if (before < 2 || before >= 0x10000000)
            {
                after = before;
                return false;
            }
            after = --*refs;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            before = after = 0;
            return false;
        }
    }

    uint32_t v3_guarded_attach(uintptr_t wrapper, uintptr_t child)
    {
        // Render order follows child-list order: index 0 parked our icons
        // BEHIND every native pin, so append at the END of the parent's list
        // (index == current count) to draw on top, like the param pipeline did.
        uint64_t parent = 0, count = 0;
        if (!v3_read64(wrapper + 0x18, parent) || !v3_heap_ptr(parent) ||
            !v3_read64(static_cast<uintptr_t>(parent) + 0xe0, count) ||
            count > 65536)
            count = 0;
        // Keep SEH in a destructor-free leaf; MSVC rejects __try in the
        // spdlog-heavy caller because that function owns C++ temporaries.
        __try
        {
            o_v3_attach(reinterpret_cast<void *>(wrapper),
                        reinterpret_cast<void *>(child),
                        static_cast<uint32_t>(count));
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return static_cast<uint32_t>(GetExceptionCode());
        }
    }

    void v3_record_candidate(uintptr_t wrapper, uintptr_t parent, uintptr_t caller,
                             uint64_t count)
    {
        // Every growing list passes these milestones once. Retain its wrapper here
        // and read the final live count after the build settles; do not take a lock
        // on all ~50k insertion calls.
        if (!goblin::maphover::map_dialog() || (count != 20 && count != 1000) ||
            g_v3_candidate_lock.test_and_set(std::memory_order_acquire))
            return;

        for (uint32_t i = 0; i < g_v3_candidate_count; ++i)
        {
            if (g_v3_candidates[i].parent == parent)
            {
                g_v3_candidates[i].wrapper = wrapper;
                g_v3_candidates[i].caller = caller;
                g_v3_candidate_lock.clear(std::memory_order_release);
                return;
            }
        }
        if (g_v3_candidate_count < V3_CANDIDATE_CAP)
            g_v3_candidates[g_v3_candidate_count++] = {wrapper, parent, caller, count};
        g_v3_candidate_lock.clear(std::memory_order_release);
    }

    constexpr float V3_HIDDEN_MAP_POS = -100000.0f;

    bool v3_position_child(uintptr_t child, float map_x, float map_y,
                           float base_tx, float base_ty,
                           const float *basis = nullptr,
                           float fx = 1.0f, float fy = 1.0f)
    {
        using GetMatrixFn = const float *(void *);
        using SetMatrixFn = void(void *, const float *);
        uint64_t vt = 0, get_addr = 0, set_addr = 0;
        if (!v3_read64(child, vt) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x18, set_addr) ||
            !v3_heap_ptr(get_addr) || !v3_heap_ptr(set_addr))
            return false;
        // Dead-generation guard: adopt the first live child's vtable as this generation's
        // signature and refuse any child that does not carry it. A child kept alive by our own
        // reference across a close, whose block the engine has since freed and handed to something
        // else, passes the heap checks above - and writing a matrix into it corrupts whatever now
        // lives there. Cleared by the manager reset.
        if (g_v3_native.child_vtable == 0)
            g_v3_native.child_vtable = vt;
        else if (vt != g_v3_native.child_vtable)
            return false;

        float matrix[8]{};
        bool ok = false;
        __try
        {
            auto *get_matrix = reinterpret_cast<GetMatrixFn *>(get_addr);
            auto *set_matrix = reinterpret_cast<SetMatrixFn *>(set_addr);
            const float *current = get_matrix(reinterpret_cast<void *>(child));
            memcpy(matrix, current, sizeof(matrix));
            // Restore the authored centring translation captured before staging;
            // do not assume the source timeline left the offscreen matrix intact.
            // fx/fy is the sampled native-widget scale: the native icon chain is
            // layer * widgetMatrix * authoredBasis, our raw child collapses that
            // to layer * (widgetScale * authoredBasis). The map anchor lives in
            // parent (map) space and must NOT be scaled; the centring pivot and
            // the authored 2x2 basis are icon-local and scale with the widget.
            matrix[3] = base_tx * fx + map_x * 20.0f;
            matrix[7] = base_ty * fy + map_y * 20.0f;
            if (basis)
            {
                matrix[0] = basis[0] * fx;
                matrix[1] = basis[1] * fx;
                matrix[4] = basis[2] * fy;
                matrix[5] = basis[3] * fy;
            }
            set_matrix(reinterpret_cast<void *>(child), matrix);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    bool v3_stage_source_child(uintptr_t child, float &base_tx, float &base_ty,
                               float *basis = nullptr)
    {
        using GetMatrixFn = const float *(void *);
        using SetMatrixFn = void(void *, const float *);
        uint64_t vt = 0, get_addr = 0, set_addr = 0;
        if (!v3_read64(child, vt) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x18, set_addr) ||
            !v3_heap_ptr(get_addr) || !v3_heap_ptr(set_addr))
            return false;

        bool ok = false;
        __try
        {
            auto *get_matrix = reinterpret_cast<GetMatrixFn *>(get_addr);
            auto *set_matrix = reinterpret_cast<SetMatrixFn *>(set_addr);
            float matrix[8]{};
            memcpy(matrix, get_matrix(reinterpret_cast<void *>(child)), sizeof(matrix));
            base_tx = matrix[3];
            base_ty = matrix[7];
            if (basis)
            {
                basis[0] = matrix[0];
                basis[1] = matrix[1];
                basis[2] = matrix[4];
                basis[3] = matrix[5];
            }
            constexpr float STAGING_TWIPS = -2000000.0f;
            matrix[3] += STAGING_TWIPS;
            matrix[7] += STAGING_TWIPS;
            set_matrix(reinterpret_cast<void *>(child), matrix);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    void v3_native_merge_snapshot(const std::vector<goblin::NativeMarkerPoint> &snapshot,
                                  bool initial);

    // The REAL Scaleform parent of our transplanted children: the manager's
    // attach parent is a C++/AS-side object whose +0x28/+0x30 is NOT a display
    // list (the first probe pass silently bailed on it). Our children carry a
    // backref to the true GFx sprite at child+0x38 - its +0x28/+0x30 IS the
    // display list holding both our children and the native icon siblings.
    uintptr_t v3_native_gfx_parent()
    {
        for (const auto &obj : g_v3_native.objects)
        {
            if (!v3_heap_ptr(obj.child)) continue;
            uint64_t par = 0;
            if (v3_read64(obj.child + 0x38, par) && v3_heap_ptr(par))
                return static_cast<uintptr_t>(par);
        }
        return 0;
    }

    // GetMatrix (DisplayObject vtbl+0x10) into m8[8], SEH-guarded.
    bool v3_read_matrix(uintptr_t node, float *m8)
    {
        uint64_t vt = 0, get_addr = 0;
        if (!v3_read64(node, vt) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
            !v3_heap_ptr(get_addr))
            return false;
        bool ok = false;
        __try
        {
            using GetMatrixFn = const float *(void *);
            const float *cur = reinterpret_cast<GetMatrixFn *>(get_addr)(
                reinterpret_cast<void *>(node));
            memcpy(m8, cur, sizeof(float) * 8);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    bool v3_native_node_is_ours(uint64_t node)
    {
        for (const auto &obj : g_v3_native.objects)
            if (obj.child == node)
                return true;
        return false;
    }

    // Read the live 2x2 the engine wrote into a REAL native pin node of our
    // target parent. The parent is a sprite: display list base @+0x28, count
    // @+0x30, exactly the layout create_native_icon_instance scans (our own
    // transplanted children live in the same list - skipped by pointer).
    bool v3_native_sample_widget(float &fx, float &fy)
    {
        const uintptr_t gparent = v3_native_gfx_parent();
        uint64_t base = 0, count = 0;
        if (!gparent ||
            !v3_read64(gparent + 0x28, base) ||
            !v3_read64(gparent + 0x30, count) ||
            !v3_heap_ptr(base) || count == 0 || count > 65536)
            return false;
        const uint64_t scan = count < 24 ? count : 24;
        for (uint64_t i = 0; i < scan; ++i)
        {
            uint64_t node = 0;
            if (!v3_read64(base + i * 8, node) || !v3_heap_ptr(node) ||
                v3_native_node_is_ours(node))
                continue;
            float m[8]{};
            if (!v3_read_matrix(static_cast<uintptr_t>(node), m))
                continue;
            const float sx = m[0] < 0.0f ? -m[0] : m[0];
            const float sy = m[5] < 0.0f ? -m[5] : m[5];
            if (sx > 0.0001f && sx < 1000.0f && sy > 0.0001f && sy < 1000.0f)
            {
                fx = sx;
                fy = sy;
                return true;
            }
        }
        return false;
    }

    // One-line-per-node probe of where the engine writes the zoom adaptation:
    // logs the node-level and first-child-level 2x2 of the first two native
    // pins plus our first object. Called on >2% zoom moves (debug only), so a
    // single min->max zoom sweep in game pins the level and the exact curve.
    void v3_native_dump_probe(float zoom)
    {
        const uintptr_t gparent = v3_native_gfx_parent();
        uint64_t base = 0, count = 0;
        if (!gparent ||
            !v3_read64(gparent + 0x28, base) ||
            !v3_read64(gparent + 0x30, count) ||
            !v3_heap_ptr(base) || count == 0 || count > 65536)
        {
            static bool bail_logged = false;
            if (!bail_logged)
            {
                bail_logged = true;
                spdlog::info("[v3probe] no readable gfx parent (gp=0x{:X} base=0x{:X} "
                             "count={}) - dump unavailable this session",
                             gparent, base, count);
            }
            return;
        }
        int dumped = 0;
        const uint64_t scan = count < 24 ? count : 24;
        for (uint64_t i = 0; i < scan && dumped < 2; ++i)
        {
            uint64_t node = 0;
            if (!v3_read64(base + i * 8, node) || !v3_heap_ptr(node) ||
                v3_native_node_is_ours(node))
                continue;
            float m[8]{};
            if (!v3_read_matrix(static_cast<uintptr_t>(node), m))
                continue;
            uint32_t depth = 0;
            v3_read32(static_cast<uintptr_t>(node) + 0x14, depth);
            uint64_t cbase = 0, ccount = 0, child0 = 0;
            v3_read64(static_cast<uintptr_t>(node) + 0x28, cbase);
            v3_read64(static_cast<uintptr_t>(node) + 0x30, ccount);
            if (v3_heap_ptr(cbase) && ccount > 0 && ccount < 4096)
                v3_read64(cbase, child0);
            float c[8]{};
            const bool cgot =
                v3_heap_ptr(child0) && v3_read_matrix(static_cast<uintptr_t>(child0), c);
            spdlog::info("[v3probe] zoom={:.4f} native[{}] node=0x{:X} depth={} "
                         "m=({:.4f},{:.4f}) t=({:.0f},{:.0f}) kids={} "
                         "child0 m=({:.4f},{:.4f})",
                         zoom, dumped, node, depth, m[0], m[5], m[3], m[7],
                         ccount, cgot ? c[0] : 0.0f, cgot ? c[5] : 0.0f);
            ++dumped;
        }
        if (!g_v3_native.objects.empty())
        {
            const auto &obj = g_v3_native.objects.front();
            float m[8]{};
            if (v3_heap_ptr(obj.child) && v3_read_matrix(obj.child, m))
                spdlog::info("[v3probe] zoom={:.4f} ours node=0x{:X} "
                             "m=({:.4f},{:.4f}) t=({:.0f},{:.0f}) curF=({:.4f},{:.4f})",
                             zoom, obj.child, m[0], m[5], m[3], m[7],
                             g_v3_native.cur_fx, g_v3_native.cur_fy);
        }
    }

    // g_v3_native owns four heap containers (pending / objects / by_row / queued) that are freed wholesale
    // by v3_native_reset and grown from the factory pulse and the engine's place hook, with no lock
    // anywhere in this file. Whether that is a live race or accidentally single-threaded depends purely on
    // which threads reach those sites, and nothing in the code states it: the factory slots are
    // thread_local (so the author expected several threads), yet v3_native_tick gates on that same TLS (so
    // the gate only works if it is one thread). Report each site's thread once - at most a dozen lines for
    // the whole process - so the next run settles it instead of us guessing. Not gated on debug logging:
    // this is the input to a heap-corruption decision, exactly the case where a quiet build must still say.
    void v3_note_thread(const char *site)
    {
        struct SiteTid { std::atomic<const char *> site; std::atomic<uint32_t> tid; };
        static SiteTid seen[12];
        const uint32_t tid = GetCurrentThreadId();
        for (auto &s : seen)
        {
            const char *sp = s.site.load(std::memory_order_relaxed);
            if (sp == site && s.tid.load(std::memory_order_relaxed) == tid)
                return; // already reported this site/thread pair
            if (sp == nullptr)
            {
                const char *expect = nullptr;
                if (s.site.compare_exchange_strong(expect, site, std::memory_order_relaxed))
                {
                    s.tid.store(tid, std::memory_order_relaxed);
                    spdlog::info("[v3] thread map: {} runs on tid {}.", site, tid);
                }
                return;
            }
        }
    }

    // MEASURED 2026-07-27: tick, reset and the factory pulse all ran on one thread (tid 27276), and the
    // second thread seen in v3_place_detour (27080) belongs to another movie's load, where the
    // thread_local g_v3_factory_active is 0 - so it never enters the capture block and never touches the
    // manager. The four containers are therefore single-threaded IN PRACTICE, which is why they carry no
    // lock. That is an invariant of the engine's behaviour, not of our code: if a game update moves the
    // map build onto a worker, every container site races and the heap goes. So watch it instead of
    // hoping - this costs one atomic compare per mutation and says so out loud if it ever changes.
    std::atomic<uint32_t> g_v3_owner_tid{0};
    void v3_check_owner(const char *site)
    {
        const uint32_t tid = GetCurrentThreadId();
        uint32_t expect = 0;
        if (g_v3_owner_tid.compare_exchange_strong(expect, tid, std::memory_order_relaxed))
            return; // first mutator seen - this thread owns the manager
        if (expect == tid)
            return;
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1, std::memory_order_relaxed) < 4)
            spdlog::error("[v3native] MANAGER TOUCHED FROM A SECOND THREAD: {} on tid {}, owner tid {}. "
                          "The unlocked containers now race - expect heap corruption.",
                          site, tid, expect);
    }

    void v3_factory_clear_request(bool drop_held_ref)
    {
        for (auto &s : g_v3_factory_slots)
        {
            if (drop_held_ref && s.held && v3_heap_ptr(s.child))
            {
                uint32_t before = 0, after = 0;
                v3_drop_held_ref(s.child, before, after);
            }
            s = V3FactorySlot{};
        }
        g_v3_factory_active = 0;
    }

    void v3_native_reset()
    {
        // NEVER touch the abandoned children here. Under the all-layers model
        // a reset only ever happens when the manager re-anchors to a NEW
        // parent, i.e. the old movie generation is torn down or dying - its
        // objects may already be freed AND reallocated, so a SetMatrix "park"
        // writes into foreign live engine memory without faulting (SEH cannot
        // see it) and corrupts the rebuild: crash 21:39 (null-write inside the
        // engine's own map-build chain on reopen-after-freeze). Layer switches
        // no longer reset at all, so there is nothing visible to park anyway.
        // Same reasoning for held factory refs: dropping them would write a
        // refcount into the dead movie's memory - leak the (dead) ref instead.
        v3_note_thread("v3_native_reset (frees all four containers)");
        v3_check_owner("v3_native_reset");
        v3_factory_clear_request(false);
        g_v3_native = V3NativeManager{};
    }

    bool v3_native_seed_from_live_callback()
    {
        const int layer = goblin::maphover::map_layer();
        const uintptr_t wrapper = g_v3_target_wrapper.load(std::memory_order_acquire);
        const uintptr_t parent = g_v3_target_parent.load(std::memory_order_relaxed);
        const int target_layer = g_v3_target_layer.load(std::memory_order_relaxed);
        if (g_v3_native.seeded &&
            g_v3_native.wrapper == wrapper && g_v3_native.parent == parent)
            return true; // same live session (incl. quick reopen) - keep managing
        // The manager survives map close. If the map closed and no fresh burst
        // re-anchored the target yet, both manager and target may point into a
        // torn-down movie - never build there. RM2 factory pulses exist ONLY
        // during a build burst (proven 12:37 session: zero creations after the
        // burst), so on a stale manager the reseed must happen INLINE below,
        // not in the map-frame tick - by then the pulses are gone.
        if (g_v3_map_closed.load(std::memory_order_relaxed))
            return false;
        uint64_t wrapper_parent = 0, live_count = 0;
        // At 100 WorldMapItems this is unambiguously the active native marker
        // parent, while hundreds of exact sprite-171 callbacks still remain in
        // the stock build burst to act as safe factory pulses.
        if (layer < 0 || layer > 2 ||
            !v3_heap_ptr(wrapper) || !v3_heap_ptr(parent) ||
            !v3_read64(wrapper + 0x18, wrapper_parent) || wrapper_parent != parent ||
            !v3_read64(parent + 0xe0, live_count) || live_count < 100)
            return false;
        if (target_layer < 0)
            // The burst ran before map_layer() resolved; stamp the live layer
            // (informational only - the shared parent hosts every layer).
            g_v3_target_layer.store(layer, std::memory_order_relaxed);

        v3_native_reset();
        g_v3_native.layer = layer;
        g_v3_native.wrapper = wrapper;
        g_v3_native.parent = parent;
        g_v3_native.observed_count = live_count;
        g_v3_native.seeded = true;
        g_v3_native.build_started_ms = GetTickCount64();
        g_v3_native.next_refresh_ms = g_v3_native.build_started_ms + 200;
        g_v3_native.last_progress_ms = g_v3_native.build_started_ms;
        v3_native_merge_snapshot(goblin::native_marker_snapshot(layer), true);
        // Initial construction happens across exact sprite-171 ExecuteTag calls
        // in this same stock build burst, before the first map-frame callback.
        g_v3_native.frame_budget = g_v3_native.pending.size();
        return true;
    }

    void v3_native_set_visible(V3NativeObject &obj, bool visible)
    {
        if (!v3_heap_ptr(obj.child) || obj.visible == visible) return;
        if (v3_position_child(obj.child,
                              visible ? obj.map_x : V3_HIDDEN_MAP_POS,
                              visible ? obj.map_z : V3_HIDDEN_MAP_POS,
                              obj.base_tx, obj.base_ty,
                              obj.base_m, g_v3_native.cur_fx, g_v3_native.cur_fy))
            obj.visible = visible;
    }

    void v3_native_merge_snapshot(const std::vector<goblin::NativeMarkerPoint> &snapshot,
                                  bool initial)
    {
        if (goblin::icons_hidden())
        {
            for (auto &obj : g_v3_native.objects) v3_native_set_visible(obj, false);
            return;
        }

        size_t visible_count = 0;
        const size_t queued_before = g_v3_native.pending.size();
        for (const auto &point : snapshot)
        {
            if (point.visible) ++visible_count;
            const auto found = g_v3_native.by_row.find(point.original_row_id);
            if (found != g_v3_native.by_row.end())
            {
                // Icon swaps (focus glow / live-loot reveal) CANNOT recreate the
                // child after the build burst - factory pulses are gone. Keep
                // the existing image until the next movie rebuild and only
                // drive visibility (retire+recreate here used to dead-queue
                // the row and stall the refresh).
                V3NativeObject &obj = g_v3_native.objects[found->second];
                // Focus rings are the one kind of child that MOVES after the build burst: the
                // pool is created once and then re-aimed at whatever the player isolates. The
                // stored map position is what set_visible replays, so update it and force the
                // reposition (hide-then-show, since set_visible is a no-op when the flag is
                // already what we ask for). Real markers keep their position for life.
                if ((point.original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT) != 0)
                {
                    float mx = 0.0f, mz = 0.0f;
                    if (goblin::mapproject::to_map(point.area, point.gx, point.gz, point.px,
                                                   point.pz, mx, mz) &&
                        (mx != obj.map_x || mz != obj.map_z))
                    {
                        obj.map_x = mx;
                        obj.map_z = mz;
                        if (obj.visible)
                            v3_native_set_visible(obj, false); // replay from the new position
                    }
                }
                v3_native_set_visible(obj, point.visible);
                continue;
            }
            // At seed time queue EVERY migrated row, hidden ones included:
            // factory pulses exist only during the build burst, so an object
            // that is not created NOW can never be shown by a later category
            // toggle. Hidden rows are created parked offscreen.
            if ((initial || point.visible) &&
                g_v3_native.queued.insert(point.original_row_id).second)
                g_v3_native.pending.push_back(point);
        }
        if (g_v3_native.pending.size() != queued_before)
        {
            g_v3_native.completion_reported = false;
            g_v3_native.settle_frames = 0;
        }
        if (initial)
            // Seed build order = draw order (append = on top), so sort by:
            // (1) visible rows FIRST - the pulse pool is finite and any
            //     shortfall must land on rows the player cannot see anyway;
            // (2) row id DESCENDING - the registry z-order contract is
            //     "lower row id draws on top" (row_id_registry LAYER_ORDER),
            //     so lower ids must be created LAST;
            // (3) a cleared-badge twin right AFTER its base row - the badge
            //     draws above its own icon.
            std::stable_sort(
                g_v3_native.pending.begin(), g_v3_native.pending.end(),
                [](const goblin::NativeMarkerPoint &a,
                   const goblin::NativeMarkerPoint &b) {
                    if (a.visible != b.visible)
                        return a.visible > b.visible;
                    const uint64_t ab =
                        a.original_row_id & ~goblin::NATIVE_CLEARED_KEY_BIT;
                    const uint64_t bb =
                        b.original_row_id & ~goblin::NATIVE_CLEARED_KEY_BIT;
                    if (ab != bb)
                        return ab > bb;
                    return (a.original_row_id & goblin::NATIVE_CLEARED_KEY_BIT) <
                           (b.original_row_id & goblin::NATIVE_CLEARED_KEY_BIT);
                });
        if (initial)
            spdlog::info("[v3native] item categories layer={} seedParentCount={}; "
                         "migrated={} visible={} requested={}",
                         g_v3_native.layer, g_v3_native.observed_count,
                         snapshot.size(), visible_count,
                         g_v3_native.pending.size() - g_v3_native.pending_index);
    }

    void v3_native_tick()
    {
        if (!goblin::variants::kNativeMarkers) return;
        const int layer = goblin::maphover::map_layer();
        if (layer < 0 || layer > 2) return;
        v3_note_thread("v3_native_tick (map frame)");
        v3_check_owner("v3_native_tick");

        const uintptr_t wrapper = g_v3_target_wrapper.load(std::memory_order_acquire);
        const uintptr_t parent = g_v3_target_parent.load(std::memory_order_relaxed);
        // The shared parent hosts every layer's markers - the target's stamp
        // layer says which layer was live at anchor time, nothing more, so the
        // live layer must NOT gate this validation (it silently froze the
        // whole manager on UG/DLC).
        uint64_t wrapper_parent = 0, live_count = 0;
        if (!v3_heap_ptr(wrapper) || !v3_heap_ptr(parent) ||
            !v3_read64(wrapper + 0x18, wrapper_parent) || wrapper_parent != parent ||
            !v3_read64(parent + 0xe0, live_count))
            return;
        if (g_v3_target_layer.load(std::memory_order_relaxed) < 0)
            g_v3_target_layer.store(layer, std::memory_order_relaxed);

        if (g_v3_native.parent != parent)
        {
            v3_native_reset();
            g_v3_native.layer = layer;
            g_v3_native.wrapper = wrapper;
            g_v3_native.parent = parent;
            // Do NOT capture parent_vtable here. At retarget time (count=100, early in the build
            // burst) the container's vtable slot can still read 0 - capturing that stored a zero
            // signature and made the liveness guard reject the parent forever. It is adopted
            // lazily below, the first frame the parent shows a real vtable.
            g_v3_native.parent_vtable = 0;
            g_v3_native.observed_count = live_count;
            return;
        }
        // QUICK REOPEN with no burst: the game reused the same movie and parent, so
        // v3_note_movie_attach never cleared the gate and viewport_reconcile would stay shut on an
        // empty map. Clear it here, but only on PROOF the map is genuinely open again:
        //   * the map dialog's own per-frame hook has advanced past the close stamp (that hook
        //     stops during the close-then-teardown window, so a fresh value cannot come from
        //     there); >8 ms guards the same-tick boundary where both stamps read equal, and
        //   * the cached parent still carries this generation's vtable. A teardown-and-reopen whose
        //     rebuild burst has not landed yet has a fresh heartbeat but a FREED old parent, whose
        //     block no longer holds our signature - so we do not clear, and nothing relinks into it.
        if (g_v3_map_closed.load(std::memory_order_relaxed) && g_v3_native.seeded &&
            !g_v3_native.objects.empty())
        {
            const uint64_t closed_at = g_v3_close_ms.load(std::memory_order_acquire);
            const uint64_t last_live = goblin::maphover::last_activity_ms();
            uint64_t pvt = 0;
            const bool pvt_ok = v3_read64(g_v3_native.parent, pvt) && pvt != 0;
            if (pvt_ok && g_v3_native.parent_vtable == 0)
                g_v3_native.parent_vtable = pvt; // first frame it is genuinely alive
            const bool parent_live = pvt_ok && pvt == g_v3_native.parent_vtable;
            if (last_live > closed_at + 8 && parent_live)
                g_v3_map_closed.store(false, std::memory_order_relaxed);
        }
        if (g_v3_native.layer != layer)
        {
            // Same shared parent hosts every layer's markers: a layer switch
            // is pure show/hide of the right rows - re-snapshot immediately.
            // SAY SO in the log. Nothing else records the displayed layer (the two lines that
            // print one are build-time only), so a report from the DLC map was indistinguishable
            // from one from the Lands Between - which is exactly the wrong thing to be blind to
            // while chasing a DLC-only crash. 0 = overworld, 1 = underground, 2 = Shadow Realm.
            spdlog::info("[v3native] displayed layer {} -> {} (tracked {} markers)",
                         g_v3_native.layer, layer, g_v3_native.objects.size());
            g_v3_native.layer = layer;
            g_v3_native.next_refresh_ms = 0;
        }

        if (!g_v3_native.seeded)
        {
            // Let the game's small remaining vanilla/ERR WorldMapItem burst
            // settle before our count changes the same parent.
            if (live_count != g_v3_native.observed_count)
            {
                g_v3_native.observed_count = live_count;
                g_v3_native.stable_frames = 0;
                return;
            }
            if (++g_v3_native.stable_frames < 8)
                return;
            g_v3_native.seeded = true;
            g_v3_native.build_started_ms = GetTickCount64();
            g_v3_native.next_refresh_ms = g_v3_native.build_started_ms + 200;
            g_v3_native.last_progress_ms = g_v3_native.build_started_ms;
            v3_native_merge_snapshot(goblin::native_marker_snapshot(layer), true);
        }

        // Visibility/category/collection is re-read live. Existing objects are
        // moved offscreen/on-map; newly enabled rows are appended to the queue.
        const uint64_t now_ms = GetTickCount64();

        // Queue watchdog: RM2 factory pulses exist only during a build burst.
        // If queued rows never build (this session's 180-parent incident), the
        // outstanding queue would gate the 500ms refresh forever and kill live
        // hide/show - drop it (rows stay in `queued`, skip-for-session).
        if (g_v3_native.pending_index < g_v3_native.pending.size() &&
            g_v3_native.last_progress_ms != 0 &&
            now_ms - g_v3_native.last_progress_ms > 5000)
        {
            // Say WHY there were no pulses. A pulse needs all of: the icon sprite injected, a live
            // timeline ctx on sprite 171, and an open map. A player report (Linux, DLC map) hit
            // this with 1331 markers dropped, and the message alone could not tell which gate was
            // shut - so print the three of them plus how many children were rejected.
            spdlog::warn("[v3native] {} queued markers never built (no factory pulses); dropping "
                         "the queue to keep live refresh alive. gates: qmark_injected={} "
                         "map_open={} rejected_so_far={} attached={}",
                         g_v3_native.pending.size() - g_v3_native.pending_index,
                         goblin::gfx_probe::icons_injected(),
                         goblin::maphover::map_dialog() != nullptr, g_v3_native.failed,
                         g_v3_native.objects.size());
            g_v3_native.pending_index = g_v3_native.pending.size();
        }
        if (g_v3_native.pending_index >= g_v3_native.pending.size() &&
            now_ms >= g_v3_native.next_refresh_ms)
        {
            g_v3_native.next_refresh_ms = now_ms + 200;
            v3_native_merge_snapshot(goblin::native_marker_snapshot(layer), false);
        }

        // Counter-zoom pass: our children ride the marker layer's parent
        // transform 1:1 (nothing engine-side updates non-widget children).
        // Primary source: the live 2x2 sampled from a real native pin node -
        // IF the engine writes its adaptation there (first session showed
        // identity at the level we sampled; the [v3probe] dump pins the truth).
        // Until a non-identity sample appears, use the measured power law:
        // vs a fully map-locked icon the natives were ~2x smaller at ~3x
        // zoom-in and ~2x bigger at ~3x zoom-out, pivot at the default open
        // zoom -> factor = (PIVOT/zoom)^(ln2/ln3), applied per frame.
        constexpr float V3_ZOOM_PIVOT = 0.70f;
        constexpr float V3_ZOOM_EXP = 0.6309f; // ln(2)/ln(3)
        constexpr float V3_SIZE_TRIM = 0.92f;  // user calibration: a touch smaller
        float fx = 0.0f, fy = 0.0f;
        goblin::mapproject::MapView view{};
        const bool have_view =
            goblin::mapproject::read_view(view) && view.zoom > 0.01f;
        if (have_view)
        {
            const float zd = view.zoom > g_v3_native.last_dump_zoom
                                 ? view.zoom - g_v3_native.last_dump_zoom
                                 : g_v3_native.last_dump_zoom - view.zoom;
            if (zd > 0.02f * view.zoom)
            {
                g_v3_native.last_dump_zoom = view.zoom;
                v3_native_dump_probe(view.zoom);
            }
        }
        if (v3_native_sample_widget(fx, fy) &&
            (fx < 0.98f || fx > 1.02f || fy < 0.98f || fy > 1.02f))
        {
            if (!g_v3_native.sample_logged)
            {
                g_v3_native.sample_logged = true;
                spdlog::info("[v3native] non-identity widget sample fx={:.4f} "
                             "fy={:.4f} zoom={:.4f} - using engine curve",
                             fx, fy, have_view ? view.zoom : 0.0f);
            }
        }
        else if (have_view)
        {
            fx = fy = V3_SIZE_TRIM * powf(V3_ZOOM_PIVOT / view.zoom, V3_ZOOM_EXP);
        }
        else
        {
            fx = fy = 0.0f;
        }
        if (fx > 0.0f && fy > 0.0f)
        {
            fx = std::clamp(fx, 0.01f, 100.0f);
            fy = std::clamp(fy, 0.01f, 100.0f);
            const float dx = fx > g_v3_native.cur_fx ? fx - g_v3_native.cur_fx
                                                     : g_v3_native.cur_fx - fx;
            const float dy = fy > g_v3_native.cur_fy ? fy - g_v3_native.cur_fy
                                                     : g_v3_native.cur_fy - fy;
            if (dx > 0.003f * g_v3_native.cur_fx || dy > 0.003f * g_v3_native.cur_fy)
            {
                g_v3_native.cur_fx = fx;
                g_v3_native.cur_fy = fy;
                for (auto &obj : g_v3_native.objects)
                    if (v3_heap_ptr(obj.child))
                        v3_position_child(obj.child,
                                          obj.visible ? obj.map_x : V3_HIDDEN_MAP_POS,
                                          obj.visible ? obj.map_z : V3_HIDDEN_MAP_POS,
                                          obj.base_tx, obj.base_ty,
                                          obj.base_m, fx, fy);
            }
        }

        // Once both the stock build and our category queue have stopped growing,
        // report the actual parent composition.  Subtracting our known bitmap
        // children gives the remaining heavyweight WorldMapItem population.
        if (g_v3_native.pending_index >= g_v3_native.pending.size() &&
            g_v3_factory_active == 0)
        {
            if (live_count != g_v3_native.settle_count)
            {
                g_v3_native.settle_count = live_count;
                g_v3_native.settle_frames = 0;
            }
            else if (!g_v3_native.completion_reported &&
                     ++g_v3_native.settle_frames >= 30)
            {
                const uint64_t lightweight = g_v3_native.objects.size();
                const uint64_t heavy = live_count >= lightweight ? live_count - lightweight : 0;
                spdlog::info("[v3native] CATEGORIES READY: layer={} "
                             "created={} failed={} parentCount={} inferredHeavy={}",
                             g_v3_native.layer, lightweight, g_v3_native.failed,
                             live_count, heavy);
                g_v3_native.completion_reported = true;
            }
        }

        // A live RM2::Execute callback later in this frame consumes this budget.
        // The timeline ctx is never retained here.
        g_v3_native.frame_budget = 96;
    }

    // Dev-only (debug_logging) one-shot: prove a SOLID-color fill renders with no
    // GPU texture. Routes ONE empty MovieClip (DefineSprite 171, flags1=0x00) through
    // the SAME create -> materialize -> stage -> attach pipeline the icon markers use
    // (the o_v3_place detour stages the materialized child into slot[0]; a raw live_ctx
    // node is already parented and faults on re-attach), then draws an opaque magenta
    // 500px square into its DrawingContext via beginFill (NOT beginBitmapFill) and
    // anchors it at map (0,0). If a filled square appears, route (c) - a native panel
    // drawn from our own display objects (backgrounds/bars/frames) - is viable.
    // Fires ONCE per build (latched on entry) BEFORE the marker loop re-arms slot[0].

    // ── MENU-MOVIE graphics probe (does a MENU movie rasterize solid fills?) ──
    // The worldmap movie does NOT render DrawingContext shape-meshes; this tests
    // whether a system/menu movie DOES. Hooks CSMenuMan::updateTask (the menu UI
    // thread), reads the active menu (*(this+0x80)), scans its fields for reachable
    // GFx display objects (a non-null code ptr at vtable+0x2a0 = the DrawingContext
    // getter present only on Sprite/MovieClip), and draws a big solid rect into each
    // candidate's graphics (SEH-guarded per candidate). If a rect renders on any
    // NON-worldmap menu, route (c) is viable on a menu surface = custom visual
    // controls become possible. Dev-only (debug_logging); this updateTask hook is
    // also the UI-thread beachhead the future native announce/dialogs need.
    using MenuUpdateFn = void(void *menuman, void *dt);
    MenuUpdateFn *o_menu_update = nullptr;
    std::atomic<uint64_t> g_menu_last_active{0};


    // ── STANDALONE settings menu open (F11), Proto 2.0 ──
    // Opens the game's own settings menu via the SAME job-push machinery as the
    // MessageBox (mirrors reference impl FUN_14080fbf0): build the settings-open job
    // (FUN_1408087e0, movie "02_040_OptionSetting" + factory lambda), ref-move
    // (FUN_1407a7b60), push onto the active menu (FUN_1407edfa0). Must run on the
    // menu UI thread with a menu active - so we fire it from the updateTask detour.
    using BuildJobFn = void *(void *out, void *owner, uint8_t flag); // FUN_1408087e0
    using RefMoveFn = void *(void *src, void *dst);                  // FUN_1407a7b60
    using PushJobFn = void *(void *menu, void *out2, void *ctxOut, void *jobPtr, void *tmp); // FUN_1407edfa0
    BuildJobFn *p_build_job = nullptr;
    RefMoveFn *p_refmove = nullptr;
    PushJobFn *p_push_job = nullptr;
    // Set at F11 push; consumed by the NEXT OptionSettingTopDialog ctor to mean
    // "this is OUR open - suppress the game tabs, show only ours".
    std::atomic<int> g_our_f11_open{0};
    void **p_worldchrman_slot = nullptr; // *slot != 0 => a game world is loaded (in-game)
    std::atomic<uintptr_t> g_our_dialog{0}; // our currently-open F11 dialog (anti-restack; cleared by its dtor)
    // The persistent base/root menu (the in-game HUD menu = *(CSMenuMan+0x80) while NO
    // full-screen menu is up). Pushing settings onto the BASE stacks it as an overlay
    // (like it stacks over gameplay); pushing onto the map instead REPLACES the map view.
    std::atomic<uintptr_t> g_base_menu{0};
    // Whatever menu is active this frame (see menu_update_detour) - the generic host.
    std::atomic<uintptr_t> g_active_menu{0};
    // CSMenuMan itself, for the read-only window scan when no window host is found.
    std::atomic<uintptr_t> g_menuman{0};
    // Was the screen PUSHED onto a menu's job stack (no +0xA28 holder to watch) rather than
    // stored in a child slot? Decides how the close is noticed.
    std::atomic<bool> g_form_pushed{false};
    std::atomic<uintptr_t> g_form_job{0}; // the pushed job
    // Liveness by ACTIVITY, not by inspecting the dialog: while our screen is up its row-draw
    // hook runs every frame, so a gap means it is gone. Reading the dialog's vtable instead
    // reported "closed" on a live screen, which dropped the one-screen guard and let presses
    // stack layers.
    // The menu our screen was pushed onto. Liveness is read from here, never from the job or the
    // dialog: those are freed on close, while the host stays.
    std::atomic<uintptr_t> g_form_pushed_host{0};
    // TRUE per-frame liveness: the dialog's own update (FUN_14093F540, its vt+0x10) runs every
    // frame while the dialog exists. Row draws do not - they only happen when the view refreshes,
    // which is why timing them declared a static screen closed half a second after it opened.
    // The host's sequence slot was the next guess and it never reported empty, so the screen is
    // evidently not held there; this needs no slot to be guessed at all.
    // THE FULL SIGNATURE, and it matters. FUN_14093F540 is (this, float dt, uint8_t *consumed) -
    // the same shape as the window input dispatcher it tail-calls: rcx = dialog, xmm1 = dt,
    // r8 = the shared "input still available" byte, which it reads at 0x14093F5D1
    // (`movzx eax, byte [rdi]`, rdi = r8).
    // Declaring the detour with two parameters let the compiler use r8/xmm1 as scratch, so the
    // game got whatever was left there. It survived on luck until this function grew a little and
    // r8 came through as 0 - an access violation at that instruction with RDI=0 (dump 15:27),
    // which looked exactly like "the host we pushed onto is bad" and was not.
    using FormUpdateFn = void(void *dlg, float dt, void *consumed);
    FormUpdateFn *o_form_update = nullptr;
    std::atomic<uint64_t> g_form_last_update{0};

    // Set from the row-build hook once our dialog is known; the heartbeat compares against it
    // rather than the later-declared g_form_dialog.
    std::atomic<uintptr_t> g_form_update_watch{0};
    void log_form_command_table(uintptr_t dlg); // defined with the command helpers below
    void arm_action_log_on_escape();            // defined with the input-action probe below

    // Defined with the screen stack: stamps the heartbeat of whichever of our screens this is.
    void form_dialog_ticked(uintptr_t dlg);

    // Defined with the icon code: re-applies the row icon strips. Called AFTER the dialog's own
    // update, which is the only moment in the frame that is later than the timeline advance.
    void repaint_row_icons_after_advance(uintptr_t dlg);

    void form_update_detour(void *dlg, float dt, void *consumed)
    {
        const uintptr_t d = reinterpret_cast<uintptr_t>(dlg);
        if (d == g_form_update_watch.load(std::memory_order_relaxed))
            g_form_last_update.store(GetTickCount64(), std::memory_order_relaxed);
        form_dialog_ticked(d);
        o_form_update(dlg, dt, consumed);
        // Order inside the frame is what decides whether an icon is visible at all. Writing the
        // strip position from our tick puts it BEFORE the timeline advance, and the advance
        // re-applies the authored matrix (DisplayList::MoveDisplayObject) - so the engine always
        // wrote last and the row showed the blank cell. Measured: the same object, the right x,
        // written every frame, still invisible; and scrolling fixed it because that write comes
        // from the render path. This is the same write, moved to after the update.
        repaint_row_icons_after_advance(d);
    }

    // ── Task #9 CAPTURE: log every menu-job push (FUN_1407edfa0) ──
    // Goal: learn the PUSH TARGET the settings sub-page open (tab 6/7/8 from our
    // map-hosted dialog) uses - that push lands the sub-movie OVER the live map with
    // a clean lifecycle. Once we know the target menu + call-site RA, we open OUR
    // top settings dialog through the same push (F11-direct, no memo hijack).
    // Read-only pass-through; dev-gated on debug_logging. Registered AFTER the
    // p_push_job scan (MinHook rewrites the prologue the AOB matches).
    // Task #9 CAPTURE 2: the GENERIC settings movie-job builder FUN_140808630
    // (out, owner, DESC{id,...}, flag). EVERY movie-based settings sub-open funnels
    // through it (Graphic tab, Brightness, and F11's own OptionSetting is its sibling
    // FUN_1408087e0). The pushJob capture came back empty for sub-pages, so this is
    // the funnel to watch: RA = the tab-select/decide site that opens the sub-page
    // over the map; owner = the target the sub-page attaches to. Read-only pass-through.
    using BuilderFn = void *(void *out, void *owner, void *desc, uint8_t flag,
                             void *p5, void *p6);
    BuilderFn *o_builder = nullptr;
    void *builder_detour(void *out, void *owner, void *desc, uint8_t flag,
                         void *p5, void *p6)
    {
        if (goblin::config::debugLogging)
        {
            const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            uint64_t ownerVt = 0;
            uint32_t descId = 0;
            __try
            {
                if (owner)
                    ownerVt = *reinterpret_cast<uint64_t *>(owner);
                if (desc)
                    descId = *reinterpret_cast<uint32_t *>(desc);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            spdlog::info("[subopen] builder owner=0x{:X} ownerVt=exe+0x{:X} descId=0x{:X} "
                         "flag={} RA=exe+0x{:X} (map=0x{:X})",
                         reinterpret_cast<uintptr_t>(owner),
                         ownerVt >= base ? ownerVt - base : ownerVt, descId, flag,
                         ra - base, reinterpret_cast<uintptr_t>(goblin::maphover::map_dialog()));
        }
        return o_builder(out, owner, desc, flag, p5, p6);
    }

    PushJobFn *o_push_job = nullptr;
    void *pushjob_detour(void *menu, void *out2, void *ctxOut, void *jobPtr, void *tmp)
    {
        if (goblin::config::debugLogging)
        {
            const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            uint64_t menuVt = 0;
            __try
            {
                if (menu)
                    menuVt = *reinterpret_cast<uint64_t *>(menu);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            spdlog::info("[pushjob] menu=0x{:X} vt=exe+0x{:X} job=0x{:X} RA=exe+0x{:X} "
                         "(base=0x{:X} map=0x{:X})",
                         reinterpret_cast<uintptr_t>(menu),
                         menuVt >= base ? menuVt - base : menuVt,
                         reinterpret_cast<uintptr_t>(jobPtr), ra - base,
                         g_base_menu.load(std::memory_order_relaxed),
                         reinterpret_cast<uintptr_t>(goblin::maphover::map_dialog()));
        }
        return o_push_job(menu, out2, ctxOut, jobPtr, tmp);
    }


    // ── OVER-MAP settings via the WorldMapDialog's sub-dialog JOB slot (v24) ──
    // The map runs its sub-dialogs (memo/warp) as a CS::MenuWindowJob held at
    // WorldMapDialog+0xA28 (found by host-scan; vt exe+0x2AA97E8). F11's build-job
    // (FUN_1408087e0) produces exactly a MenuWindowJob for the settings movie. So build
    // a settings MenuWindowJob and STORE it at WMD+0xA28 when the slot is empty; the WMD's
    // own update steps it -> settings movie + dialog + render OVER the live map. Heavily
    // logged so a crash's last line shows how far it got. Dev-only.
    // ── F11-DIRECT settings over the map (v29): create the 02_040 movie ourselves + build
    // the OptionSettingTopDialog against its scene, DEFERRED until the movie async-loads. No
    // memo/marker involvement. The created 02_040 movie renders over the map on its own (the
    // gfx mgr ticks it); we only need to construct the dialog once its scene root is populated
    // (root!=0), else the ctor faults on a null root (the v2/v3 crash). base+RVA calls (dev).
    std::atomic<uintptr_t> g_settings_movie{0};  // our created 02_040 movie (async-loading)
    std::atomic<uintptr_t> g_settings_dialog{0}; // constructed once the movie loaded
    // Armed while OUR keybinding-form child is the one being built/shown, so the row
    // swap below never touches the game's own keybinding screen. Declared here because
    // close_settings() clears it.
    std::atomic<bool> g_form_rows_armed{false};
    // The form's dialog only exists a few frames after its job is stored, so the page
    // title is stamped by the first row draw rather than at open time.
    std::atomic<bool> g_form_title_pending{false};
    // The live CS::KeyConfigDialog, recovered from the row-vector builder's argument
    // (the dialog's own list IS dlg+0x1268) and validated against the class vtable.
    std::atomic<uintptr_t> g_form_dialog{0};
    void set_form_captions(uintptr_t base, uintptr_t dlg, const wchar_t *title,
                           const wchar_t *hint);
    void prepare_form_layout(uintptr_t base, uintptr_t dlg);
    void probe_form_commands(uintptr_t base, uintptr_t dlg);
    // One-shot: dump the form's registered input commands (see probe_form_commands).
    std::atomic<bool> g_cmd_probe_done{false};
    // Live-apply for our native menu rows: snapshot of every row's bool at menu
    // open; the per-frame tick diffs and calls reapply_live_settings() on change
    // (same as the overlay does), close_settings() persists the ini once if
    // anything changed during the session.
    std::vector<uint8_t> g_menu_cfg_snapshot;
    bool g_menu_cfg_dirty = false;

    void menu_cfg_snapshot_take()
    {
        size_t n = 0;
        const goblin::NativeMenuRowDef *rows = goblin::native_menu_rows(&n);
        g_menu_cfg_snapshot.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            g_menu_cfg_snapshot[i] = *rows[i].value ? 1 : 0;
        g_menu_cfg_dirty = false;
    }

    void menu_cfg_apply_if_changed()
    {
        size_t n = 0;
        const goblin::NativeMenuRowDef *rows = goblin::native_menu_rows(&n);
        if (g_menu_cfg_snapshot.size() != n)
            return;
        bool changed = false;
        for (size_t i = 0; i < n; ++i)
        {
            const uint8_t cur = *rows[i].value ? 1 : 0;
            if (cur != g_menu_cfg_snapshot[i])
            {
                g_menu_cfg_snapshot[i] = cur;
                changed = true;
            }
        }
        if (changed)
        {
            g_menu_cfg_dirty = true;
            goblin::reapply_live_settings(); // live-apply, same as the overlay's change path
            spdlog::info("[optmenu] native-menu change applied live");
        }
    }
    std::atomic<uintptr_t> g_settings_job{0};    // the F11 build-job (kept; its +0x10 = a pre-built
                                                  // SceneObjProxy with objIface, the ctor's scene)
    uint8_t *g_settings_scene = nullptr;          // heap SceneObjProxy buffer (persists for the dialog)


    // Tear down the settings job (release + stop stepping). Mirrors the game runner's job
    // release (FUN_141eba200 unref -> vt[0] dtor when refcount hits 0), which closes the
    // dialog + its 02_040 movie. Used by the job-done detection (Back) AND the F11 toggle.

    // Per-frame builder (body after the opttop/alloc decls below): once our 02_040 movie has
    // loaded (scene root != 0), construct the dialog against its scene.

    // ── CommandList child prototype (RE cmdlist_notes.md, session 2026-07-23 night) ──
    // FUN_140747450(win, out, in) = open the native CommandList screen (movie
    // 02_045_PC_CommandList / 01_070_CommandList, dialog CS::CommandSelectDialog) as a
    // CHILD of any CS::MenuWindow: it collects the window's registered input commands
    // via vt+0x30 (base impl FUN_140744f30), builds the MenuJob (generic movie-sub-job
    // FUN_1407acb00) and stores it in the window's child-job holder @win+0xA28; the
    // window's own tick steps/renders it, Back pops it (v27-proven over the map).
    // ── Phase B: hand-made game-ABI std::function objects (MSVC _Func_impl vtable:
    // [0] copy(this,buf)->ptr [1] move [2] invoke [3] target_type [4] destroy(this,
    // dealloc)). The registrar FUN_140744540 and the entry copy-ctor FUN_140741820
    // deep-copy commands by cloning each holder's object via vt[0] into a 0x38
    // inline buffer, so our payload must stay POD and <= 0x30 bytes. Command entry
    // (0x140) layout (cmdlist_reg_re*.txt):
    //   +0x000 fn input-trigger char(byte* consumed) - REQUIRED (dispatcher derefs)
    //   +0x040 fn name/key pack provider             - REQUIRED (collect derefs):
    //          builds pack {DLString name @+0x8, byte @+0x40, DLString key @+0x48};
    //          collect FUN_140744f30 skips the row unless BOTH strings non-empty
    //   +0x080 fn extra (nullable)
    //   +0x0C0 fn action - row decide, DLRefPtr<MenuJob>* invoke(this, out)
    //   +0x100 fn visible-predicate char()           - REQUIRED (collect derefs)
    struct GameFnVt
    {
        void *copy;
        void *move;
        void *invoke;
        void *type;
        void *destroy;
    };
    struct GameFn
    {
        const GameFnVt *vt;
        uintptr_t a;
        uintptr_t b;
    };
    struct GameFnHolder
    {
        uint8_t buf[0x38];
        void *ptr; // null = empty std::function slot
    };
    static void *gamefn_copy(GameFn *self, void *buf)
    {
        auto *d = static_cast<GameFn *>(buf);
        *d = *self;
        return d;
    }
    static void gamefn_destroy(GameFn *, char) {}
    static void *gamefn_type(GameFn *)
    {
        static int s_type_tag;
        return &s_type_tag;
    }
    static char gamefn_trigger_never(GameFn *, void *) { return 0; }
    static char gamefn_pred_true(GameFn *) { return 1; }
    static void *gamefn_action_log(GameFn *self, void **out)
    {
        *out = nullptr; // no sub-job yet - just prove the decide path reaches us
        spdlog::info("[cmdlist] our row (labelId={}) activated", self->a);
        return out;
    }
    // Build the name/key pack: DLString ctor from GR_MenuText (FUN_140760970) with
    // our injected label ids (a = row label = name, b = tab label = key stand-in).
    static void *gamefn_namekey_pack(GameFn *self, uint8_t *out)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        using TextCtorFn = void(void *, uint32_t);
        auto p_text = reinterpret_cast<TextCtorFn *>(base + 0x760970);
        std::memset(out, 0, 0x88);
        p_text(out + 0x8, static_cast<uint32_t>(self->a));
        p_text(out + 0x48, static_cast<uint32_t>(self->b));
        return out;
    }
    static void gamefn_init(GameFnHolder &h, const GameFnVt *vt, uintptr_t a, uintptr_t b)
    {
        auto *f = reinterpret_cast<GameFn *>(h.buf);
        f->vt = vt;
        f->a = a;
        f->b = b;
        h.ptr = h.buf;
    }

    // Register our commands on the dialog via FUN_140744540(win, inputSpec,
    // actionFn, predFn); inputSpec = 3 contiguous holders (trigger/pack/extra).
    // Everything is deep-copied by the registrar, so stack lifetime is fine.
    void register_our_cmdlist_rows(uintptr_t dlg)
    {
        if (goblin::g_menutext_tab_id <= 0 || goblin::g_menutext_row_ids.empty())
        {
            spdlog::info("[cmdlist] no injected GR_MenuText labels - cannot register rows");
            return;
        }
        static const GameFnVt s_trig_vt{reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_trigger_never),
                                        reinterpret_cast<void *>(&gamefn_type),
                                        reinterpret_cast<void *>(&gamefn_destroy)};
        static const GameFnVt s_pack_vt{reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_namekey_pack),
                                        reinterpret_cast<void *>(&gamefn_type),
                                        reinterpret_cast<void *>(&gamefn_destroy)};
        static const GameFnVt s_action_vt{reinterpret_cast<void *>(&gamefn_copy),
                                          reinterpret_cast<void *>(&gamefn_copy),
                                          reinterpret_cast<void *>(&gamefn_action_log),
                                          reinterpret_cast<void *>(&gamefn_type),
                                          reinterpret_cast<void *>(&gamefn_destroy)};
        static const GameFnVt s_pred_vt{reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_pred_true),
                                        reinterpret_cast<void *>(&gamefn_type),
                                        reinterpret_cast<void *>(&gamefn_destroy)};
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        using RegisterFn = void(void *, void *, void *, void *);
        auto p_reg = reinterpret_cast<RegisterFn *>(base + 0x744540);
        struct InputSpec
        {
            GameFnHolder trig;
            GameFnHolder pack;
            GameFnHolder extra;
        };
        size_t added = 0;
        for (int label_id : goblin::g_menutext_row_ids)
        {
            if (label_id <= 0)
                continue;
            InputSpec spec{};
            gamefn_init(spec.trig, &s_trig_vt, 0, 0);
            gamefn_init(spec.pack, &s_pack_vt, static_cast<uintptr_t>(label_id),
                        static_cast<uintptr_t>(goblin::g_menutext_tab_id));
            spec.extra.ptr = nullptr;
            GameFnHolder action{}, pred{};
            gamefn_init(action, &s_action_vt, static_cast<uintptr_t>(label_id), 0);
            gamefn_init(pred, &s_pred_vt, 0, 0);
            p_reg(reinterpret_cast<void *>(dlg), &spec, &action, &pred);
            ++added;
        }
        spdlog::info("[cmdlist] registered {} of our commands on dlg=0x{:X}", added, dlg);
    }

    // ── Our rows ON the native keybinding form (02_160) ──────────────────────────
    // RE (keysetting_*_re.txt): KeyConfigDialog = GenericItemSelectDialog<CSMenuKeySetting>
    // over movie clips "KeySetting/ItemList" (+ ScrollBarV/V2 = native scroll). Its rows
    // are a vector of CSMenuKeySetting items (stride 0x50) built by
    // FUN_140868590(itemVec, list) -> mode 0 FUN_140868990 / mode 1 FUN_1408687d0, which
    // walk a PARAM table (FD4ParamHeaderConstAccessor, row stride 0x18) and per row call
    //   FUN_140866f80(item, list+8, deviceKind, paramRow)  - normal row
    //   FUN_140867010(item, deviceKind, paramRow)          - header row (item+0x18 = 0)
    // then append with FUN_140868fe0(&vec.alloc, item) and finish each with
    // FUN_140867820(item, list+8).
    // Item fields that drive rendering (vt+0x8 = FUN_1408674e0):
    //   +0x08 u32 deviceKind, +0x10 paramRow ptr, +0x18 bind-slot ptr (0 => HEADER row),
    //   +0x20 assigning flag, +0x30/+0x31 conflict flags.
    // paramRow layout used by the renderer: [0] u32 = FMG id in GR_MenuText (row NAME -
    // exactly the bank we already inject our localized labels into), [1] s32 = action
    // index (-1 => header; also the index into the 54-slot bind table via
    // FUN_140866ca0), byte +9 / byte +10 = enabled per device kind.
    // Headers additionally switch the row clip to frame "PadCategory"/"KeyCategory".
    // So OUR rows need nothing exotic: a small array of fake param rows pointing at our
    // injected FMG ids. The value column (Text_1) comes from FUN_140867de0(item, out) -
    // hooked below to hand back our localized On/Off instead of a key name.
    struct FakeParamRow
    {
        int32_t text_id;   // [0] GR_MenuText id - only a fallback (we draw rows ourselves),
                           //     but must stay VALID in case the draw hook is missing.
        int32_t action_ix; // [1] bind-table index; >= 0 keeps the row selectable
        int32_t pad2;
        int32_t pad3;      // bytes +8/+9/+10 = per-device enabled flags
        int32_t pad4;
    };
    static_assert(sizeof(FakeParamRow) == 0x14, "param row stride");

    // Bridge between the native item list and the menu MODEL (goblin_native_menu.*).
    // The native item ctor stores a pointer to the fake param row, and those items can
    // outlive a row rebuild - so the mapping must NOT depend on the array's address:
    //   * the pool is allocated once and never reallocates (a dangling pointer would
    //     otherwise be read by the draw hook), and
    //   * identity travels IN the param row itself: a magic tag plus a generation
    //     counter and the model index, so a stale item from a previous build is
    //     recognised and ignored instead of drawing the wrong row.
    struct FormItem
    {
        FakeParamRow param;
    };
    // How many items the last build actually put in the view list. The grid's own count runs
    // ONE build behind (measured: page 0 built 30 items with count=0, page 1 built 86 with
    // count=30), and with 2 columns a count of 30 caps the list at 15 rows - which is exactly
    // what a 43-row page showed. So the count is restated from this, AFTER the view refresh.
    std::atomic<uint32_t> g_form_landed{0};
    // before<<32 | after, so one log line shows whether the write to the grid's count survives.
    // Defined with the grid layout further down; both the first build and every refresh use it.
    void sync_grid_extent(uintptr_t dlg, uint32_t items);
    constexpr uint32_t kFormMagic = 0x4D464752; // 'MFGR'
    constexpr size_t kFormPoolMax = 512;        // hard cap: 97 ini entries + extras
    // ONE POOL PER NESTING LEVEL. A native item keeps a POINTER to its fake param row, so with a
    // single shared pool a child screen's build overwrote the rows the parent's items still point
    // at - in game the parent came back with its icons gone and needed a second confirm press.
    // Each level therefore owns its rows for as long as that screen lives.
    constexpr size_t kMaxNest = 6;
    std::vector<FormItem> g_form_pools[kMaxNest];
    size_t g_form_depth = 0;

    std::vector<FormItem> &form_pool()
    {
        return g_form_pools[g_form_depth < kMaxNest ? g_form_depth : kMaxNest - 1];
    }
    // One generation per pool: a rebuild on level 2 must not invalidate the items level 0 still
    // holds. A single counter did, and the parent came back with dead rows.
    uint32_t g_form_generation[kMaxNest] = {};

    // Which pool does this param row live in? The pools are reserved once and never move, so
    // their address ranges identify the level an item belongs to - no tag needed, and an item
    // from a level that is not the live one is simply left alone.
    int pool_level_of(const void *param)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(param);
        for (size_t l = 0; l < kMaxNest; ++l)
        {
            const std::vector<FormItem> &p = g_form_pools[l];
            if (p.empty())
                continue;
            const uintptr_t b = reinterpret_cast<uintptr_t>(p.data());
            if (a >= b && a < b + p.size() * sizeof(FormItem))
                return static_cast<int>(l);
        }
        return -1;
    }

    // ── ONE NATIVE SCREEN PER PAGE ───────────────────────────────────────────────────
    // Every page is a real screen, and EVERY screen - the root as much as a sub-page - is
    // stored in its host window's SEQUENCE slot (+0x10). That slot is not a detail: it is what
    // the engine's own input gate reads. The window input dispatcher FUN_140745570 runs a
    // window's commands only when
    //     job not done (win+0x1E8) AND sequence slot EMPTY (FUN_1407A9230 on win+0x10)
    //     AND vt[0x50](win) == 0   (that one is `*(u64*)(win+0x98) > 1`, a counter - it does
    //                               NOT look at the child-job holder +0xA28)
    // so a screen parked anywhere else leaves its host still eating the same input. Measured at
    // the title screen: pushed onto the menu's job stack, our screen took input AND the main
    // menu behind it took the same presses and opened its own pages under ours.
    // Vanilla agrees - its "Settings -> Key Assignments" opener FUN_14094FD40 ends in
    // `p_seq(window + 0x10, job)`, the same call we make. (It stores a SEQUENCE of three jobs:
    // ours plus two lambdas that handle the page transition. We store the screen alone, which is
    // why the host stays visible behind our screen instead of being swapped out.)
    //
    // Everything hangs off ONE stack. Each entry point that carries a dialog pointer (row build,
    // decide, the dialog's own per-frame update) resolves it to a level, so "which page is live"
    // is read from the engine rather than tracked in parallel with it. That was the flaw in the
    // first attempt: an arm flag, a screen->page map and three close-detection schemes drifting
    // apart.
    //
    // Cursor + first visible row of a list, saved per screen (both are plain grid fields; the
    // offsets and how they were found are documented with the grid layout below).
    struct ListPos
    {
        uint32_t cursor;
        int32_t top_row;
    };

    struct Screen
    {
        uintptr_t dlg = 0;  // its dialog - known once its first row build arrives
        int32_t page = 0;   // the model page this screen shows
        uintptr_t host = 0; // the window whose sequence slot holds it
        uintptr_t job = 0;
        ListPos pos{0, 0};  // saved while a child screen is over it
        uint64_t beat = 0;  // last time its own update ran (0 = not seen yet)
        uint64_t opened = 0;
        uint64_t shown = 0; // when its dialog first appeared
        bool hidden = false; // stepped aside while a child screen is over it
        bool shifted = false;  // the centring shift has been applied to this screen
        float shift_tx = 0.0f; // the tx we want the row section to sit at (twips)
    };

    // UI thread only: every writer (row build, decide, dialog update, the menu tick that polls
    // F8) runs on the menu thread.
    std::vector<Screen> g_screens;

    // ── why a brand-new screen must ignore input for a moment ────────────────────────
    // A window's input state is its own (the dispatcher refreshes win+0x120 each frame), so a
    // window that did not exist yet when a button went down sees that button as newly pressed on
    // its first frame. The window it was opened FROM does not: it has been watching the same
    // button all along, which is why Q closing a child does not also close the parent.
    // Vanilla never notices because its sub-screens arrive with a movie load from disk - by then
    // the button is long released. Ours comes up in ONE frame (the movie is already resident, it
    // is the same one the parent uses), so the confirm that opened a page immediately confirmed
    // that page's first row. Measured: parent decide 10:59:07.303 -> child dialog .322 -> child
    // decide on row 0 at .323.
    // Until a new window's input snapshot can be seeded from its parent, a fresh screen simply
    // does not act on input for this long.
    constexpr uint64_t kNewScreenInputGraceMs = 250;

    int screen_level(uintptr_t dlg)
    {
        if (!dlg)
            return -1;
        for (size_t i = 0; i < g_screens.size(); ++i)
            if (g_screens[i].dlg == dlg)
                return static_cast<int>(i);
        return -1;
    }


    // ── HUD-after-close diagnostic ───────────────────────────────────────────────────────────────
    // Opening our screen over GAMEPLAY pushes a job onto the base HUD menu (*(CSMenuMan+0x80)); after
    // our screen closes the game's HUD stays hidden until some native menu opens. The engine therefore
    // keeps a piece of state that our close never puts back. Rather than guess which, snapshot the base
    // menu (and CSMenuMan) when we push, and again once every screen of ours is gone, then log the
    // offsets that differ and stayed different - that names the field. Debug-logging only.
    constexpr size_t kHudSnapBytes = 0x400;
    uint8_t g_hud_snap_menu[kHudSnapBytes];
    uint8_t g_hud_snap_man[0x200]; // covers the whole menu-state array at +0x90..+0xD6
    std::atomic<uintptr_t> g_hud_snap_base{0};
    std::atomic<int> g_hud_snap_state{0}; // 1 = taken at push, 2 = compared after close

    void *g_hud_orig_job = nullptr; // the HUD menu's own sequence-slot job, kept alive across our screen
    std::atomic<uint64_t> g_hudwatch_until{0}; // keep dumping the HUD state window until this tick count
    uint8_t g_hudwatch_last[64] = {};
    std::atomic<uint64_t> g_hudwatch_next{0};

    // ── HUD mode: restore what our over-gameplay screen leaves behind ────────────────────────────
    // ROOT CAUSE, established 2026-07-28 by reading and then poking the LIVE process:
    // `CS::CSFeManImp+0x78` is the HUD's own visibility mode - 3 = HUD on in gameplay, 1 = hidden
    // under a menu, 0 = what is left after our pushed screen closes. In the stuck state the engine
    // genuinely believes the HUD is off (mode 0, the 31 per-widget flags cleared, menu ids 7 and 26
    // demoted to registered-only); writing 3 back brought the HUD up instantly AND STUCK, which also
    // proves the mode is edge-driven rather than recomputed per frame. The engine reaches 3 again only
    // through its own 0 -> 1 -> 3 transition, which is why opening and closing any native menu fixes it.
    //
    // So the fix is symmetric cleanup, not a policy: capture the mode before we push and put that
    // exact value back once every screen of ours is gone. Two guards keep it honest - we only write
    // when the mode is still 0 (so a legitimate "menu on top" state is never overridden), and the
    // singleton is only trusted when the CSFeAutoHideCtrl vtable is where we expect it at +0x4E70,
    // so on a game update that moves the layout this quietly does nothing instead of corrupting.
    void **g_feman_slot = nullptr;   // resolved by AOB in the setup below
    uint8_t g_hud_mode_at_push = 0;  // 0 = nothing captured

    uintptr_t feman_checked()
    {
        if (!g_feman_slot)
            return 0;
        __try
        {
            const uintptr_t f = reinterpret_cast<uintptr_t>(*g_feman_slot);
            if (!v3_heap_ptr(f))
                return 0;
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if (*reinterpret_cast<uintptr_t *>(f + 0x4E70) != base + 0x2A9CC50)
                return 0; // layout moved - do not touch anything
            return f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void hud_mode_capture()
    {
        g_hud_mode_at_push = 0;
        const uintptr_t f = feman_checked();
        if (!f)
            return;
        __try
        {
            g_hud_mode_at_push = *reinterpret_cast<uint8_t *>(f + 0x78);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void hud_mode_restore(const char *via)
    {
        const uint8_t want = g_hud_mode_at_push;
        g_hud_mode_at_push = 0;
        if (!want) // nothing captured, or the HUD was already off before us - leave it alone
            return;
        const uintptr_t f = feman_checked();
        if (!f)
            return;
        __try
        {
            uint8_t *mode = reinterpret_cast<uint8_t *>(f + 0x78);
            if (*mode != 0)
                return; // the engine is in a state of its own now; ours is not the last word
            *mode = want;
            spdlog::info("[hud] mode was left at 0 by our screen; restored the value from before it "
                         "({}) via {}",
                         want, via);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void log_autohide(const char *when); // defined below, next to the snapshot pair
    void log_menuman_map(const char *when, bool with_objects);

    // ── CSMenuMan, read as a map instead of a hex dump ───────────────────────────────────────────
    // Two facts turn this manager from a wall of bytes into something readable.
    //
    // 1. The menu-state array at CSMenuMan+0x90 is indexed by the MENU ID ITSELF. The engine's
    //    accessor `0x140767DF0(scratch, id)` is three instructions - `mov word [rcx], dx; mov rax, rcx;
    //    ret` - so it only wraps the id; every reader then does `state = [id + this + 0x90]` after a
    //    `cmp id, 0x47` bound check. The state is THREE bits (`and al, 7`, and `or byte[..], 7` exists
    //    at 0x14074571A), with bit 0 = registered, bit 1 = visible; the engine's own hide is
    //    `and byte [id + this + 0x90], 1` at 0x1408D4447, i.e. it KEEPS the registration.
    // 2. The exe ships RTTI, so any pointer in the object can be named offline from its vtable RVA
    //    (scratch/rtti_map.py).
    //
    // So this logs the ids that are registered/visible, and every field that looks like an object,
    // with its vtable RVA. Diffing two of these by hand names both the leftovers and the participants.
    void log_menuman_map(const char *when, bool with_objects)
    {
        if (!goblin::config::debugLogging)
            return;
        const uintptr_t man = g_menuman.load(std::memory_order_relaxed);
        if (!v3_heap_ptr(man))
            return;
        __try
        {
            const uintptr_t exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            char vis[220], reg[220];
            int vn = 0, rn = 0;
            for (int id = 0; id < 0x47; ++id)
            {
                const uint8_t st = *reinterpret_cast<uint8_t *>(man + 0x90 + id);
                if (!st)
                    continue;
                if ((st & 2) && vn < (int)sizeof(vis) - 12)
                    vn += _snprintf_s(vis + vn, sizeof(vis) - vn, _TRUNCATE, "%d(%u) ", id, st);
                else if (!(st & 2) && rn < (int)sizeof(reg) - 12)
                    rn += _snprintf_s(reg + rn, sizeof(reg) - rn, _TRUNCATE, "%d(%u) ", id, st);
            }
            vis[vn] = 0;
            reg[rn] = 0;
            spdlog::info("[menumap] {}: VISIBLE ids [{}] | registered-only ids [{}]", when, vis, reg);
            // Object fields. 0x600 bytes covers the state array and well past it.
            if (!with_objects)
                return;
            for (unsigned off = 0; off < 0x600; off += 8)
            {
                const uintptr_t v = *reinterpret_cast<uintptr_t *>(man + off);
                if (!v3_heap_ptr(v))
                    continue;
                const uintptr_t vt = *reinterpret_cast<uintptr_t *>(v);
                if (vt <= exe_base || vt - exe_base >= 0x10000000)
                    continue;
                spdlog::info("[menumap]   +0x{:X} -> 0x{:X} (vt exe+0x{:X})", off, v, vt - exe_base);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Sampled from the menu-manager update while the watch is armed: prints the state window only when it
    // CHANGES, so the log shows the exact transition that brings the HUD back instead of a wall of dumps.
    void hud_watch_sample(void *menuman)
    {
        const uint64_t until = g_hudwatch_until.load(std::memory_order_acquire);
        if (!until || !menuman)
            return;
        const uint64_t now = GetTickCount64();
        if (now > until)
        {
            g_hudwatch_until.store(0, std::memory_order_release);
            spdlog::info("[hudprobe] watch window closed");
            return;
        }
        if (now < g_hudwatch_next.load(std::memory_order_relaxed))
            return;
        g_hudwatch_next.store(now + 250, std::memory_order_relaxed);
        __try
        {
            const uint8_t *b = reinterpret_cast<const uint8_t *>(menuman) + 0x80;
            if (memcmp(b, g_hudwatch_last, sizeof(g_hudwatch_last)) == 0)
                return;
            char hex[3 * 64 + 1];
            int n = 0;
            for (int i = 0; i < 64 && n < (int)sizeof(hex) - 4; ++i)
                n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", b[i]);
            // show which offsets moved since the previous sample - that is the useful part
            char diff[256];
            int dn = 0;
            for (int i = 0; i < 64 && dn < (int)sizeof(diff) - 24; ++i)
                if (b[i] != g_hudwatch_last[i])
                    dn += _snprintf_s(diff + dn, sizeof(diff) - dn, _TRUNCATE, "+0x%02X:%02X->%02X ",
                                      0x80 + i, g_hudwatch_last[i], b[i]);
            diff[dn] = 0;
            memcpy(g_hudwatch_last, b, sizeof(g_hudwatch_last));
            spdlog::info("[hudprobe] state changed while the HUD was hidden: {}", diff);
            log_autohide("while the HUD was hidden");
            log_menuman_map("while the HUD was hidden", false);
            spdlog::info("[hudprobe]   full window now: {}", hex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // The job stack our over-gameplay screen is pushed onto is a DLFixedVector (capacity 8) at
    // menu+0xD0: the push is 0x1407AA400, which bails with "out of memory." past 8 and keeps its element
    // COUNT at vector+0x48, i.e. menu+0x118, with the elements packed 8 bytes each from align8(vector).
    // The user's reading of the defect is that we never take our job back out - the HUD hides normally
    // under any screen and simply never learns that ours is gone. This prints the count and the slots so
    // that is a measured fact rather than a theory.
    // One line per DLFixedVector: its count and every element, each named by the RVA of its vtable.
    // The exe ships full MSVC RTTI, so scratch/rtti_map.py turns those RVAs into class names offline
    // (`py scratch/rtti_map.py 0x2a93a60` -> CS::MenuWindow), which is how a bare heap address in this
    // log becomes an identified object.
    void log_one_vector(uintptr_t vec, const char *label, uintptr_t exe_base)
    {
        __try
        {
            const uint64_t count = *reinterpret_cast<uint64_t *>(vec + 0x48);
            char slots[420];
            int n = 0;
            for (uint64_t i = 0; i < count && i < 8 && n < (int)sizeof(slots) - 48; ++i)
            {
                const uintptr_t obj = *reinterpret_cast<uintptr_t *>(vec + i * 8);
                unsigned long long vt = 0;
                if (v3_heap_ptr(obj))
                {
                    const uintptr_t vtp = *reinterpret_cast<uintptr_t *>(obj);
                    if (vtp > exe_base && vtp - exe_base < 0x10000000)
                        vt = (unsigned long long)(vtp - exe_base);
                }
                n += _snprintf_s(slots + n, sizeof(slots) - n, _TRUNCATE, "0x%llX(vt exe+0x%llX) ",
                                 (unsigned long long)obj, vt);
            }
            slots[n] = 0;
            spdlog::info("[jobstack]   {}: count={} [{}]", label, count, slots);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // The job stack our over-gameplay screen is pushed onto is a DLFixedVector (capacity 8) at
    // menu+0xD0: the push is 0x1407AA400, which bails with "out of memory." past 8 and keeps its element
    // COUNT at vector+0x48, i.e. menu+0x118, with the elements packed 8 bytes each from align8(vector).
    // There is a SECOND such vector at menu+0x120 (count menu+0x168): 0x140733D70 is the engine's
    // remove-by-value for this container type and its asserts name DLFixedVector.inl, with the element
    // storage starting at align8(base) and the count at base+0x48, so +0x168 is that second vector's
    // count. Measured over an F8 cycle: the +0xD0 vector reads 0 both before and after, while +0x168
    // goes 0 -> 1 and stays, so something of ours settles in the second one. Naming the leftover object
    // is the point of this probe: writing that count back was already tried and did not restore the HUD
    // (docs/research_hud_after_close.md, excluded hypothesis 2), so the leftover matters as an identity,
    // not as a number.
    void log_job_stack(uintptr_t menu, const char *when)
    {
        if (!goblin::config::debugLogging || !v3_heap_ptr(menu))
            return;
        __try
        {
            const uintptr_t exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            unsigned long long menu_vt = 0;
            const uintptr_t mvt = *reinterpret_cast<uintptr_t *>(menu);
            if (mvt > exe_base && mvt - exe_base < 0x10000000)
                menu_vt = (unsigned long long)(mvt - exe_base);
            const uintptr_t seq = *reinterpret_cast<uintptr_t *>(menu + 0x10);
            unsigned long long seq_vt = 0;
            if (v3_heap_ptr(seq))
            {
                const uintptr_t svt = *reinterpret_cast<uintptr_t *>(seq);
                if (svt > exe_base && svt - exe_base < 0x10000000)
                    seq_vt = (unsigned long long)(svt - exe_base);
            }
            spdlog::info("[jobstack] {}: menu 0x{:X} (vt exe+0x{:X}), slot +0x10 = 0x{:X} (vt exe+0x{:X})",
                         when, menu, menu_vt, seq, seq_vt);
            log_one_vector(menu + 0xD0, "vector +0xD0", exe_base);
            log_one_vector(menu + 0x120, "vector +0x120", exe_base);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // ── CS::CSFeAutoHideCtrl, the engine's own HUD hide/fade controller ──────────────────────────
    // Found by name, not by guessing: the exe ships RTTI, so scratch/rtti_map.py lists CS::CSFeAutoHideCtrl,
    // and its vtable (exe+0x2A9CC50) is stored at +0x4E70 of CS::CSFeManImp - the HUD manager - whose
    // singleton pointer the init at exe+0x766068 writes to exe+0x3D6B880 (object size 0x8420).
    //
    // Layout from the controller's own constructor (exe+0x767E60):
    //   +0x08  byte   mode, built as 2
    //   +0x0C  float  6.0 - the idle delay
    //   +0x68  42 slots of 0x40 bytes: dword state at +0x00, byte at +0x2C, dword at +0x30 = -1
    //   +0xAF0 dword, +0xAF4 byte, +0xAF8 byte, +0xAFC dword, +0xB00 float -1.0
    // The FeMan update (exe+0x775170) picks one of three paths by a mode byte: tick (exe+0x768320),
    // plain update (exe+0x767F70) or RESET (exe+0x768240, also the ctor's tail).
    //
    // This is a read-only probe. If the HUD stays hidden because this controller is left in a hidden
    // state that nothing re-evaluates until a menu transition, the fields below will say so, and the fix
    // is then to call the engine's own reset rather than to write any of this memory.
    void log_autohide(const char *when)
    {
        if (!goblin::config::debugLogging)
            return;
        __try
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const uintptr_t feman = *reinterpret_cast<uintptr_t *>(base + 0x3D6B880);
            if (!v3_heap_ptr(feman))
                return;
            // THE HUD MODE. A hardware write watch on menu id 7's state byte caught the hide in the act
            // at exe+0x7754B6 and named its caller chain; the setter that owns that write is
            // exe+0x775320, and it opens with:
            //     eax = byte [feman + 0x82C1]      ; signed override
            //     esi = byte [feman + 0x78]        ; mode
            //     if (al >= 0) esi = eax           ; cmovns - the override wins when not negative
            //     switch (esi) { ... show/hide the menu set for this mode ... }
            // and the per-id writes are `if (registered) { show ? state |= 3 : state &= 1 }`. So the
            // state array is RECOMPUTED from this mode every frame, which is exactly why writing 3 back
            // into the array never brought the HUD back. Same idiom at exe+0x7752DB and exe+0x76E030.
            // The override has only three references in the whole exe and all three are reads, so
            // whoever sets it does so through a computed pointer - i.e. it can only be caught at runtime.
            // THE ACTUAL HUD VISIBILITY, measured rather than assumed. The mode-1 branch of the setter
            // (exe+0x775369..exe+0x775436) clears exactly these per-widget flag bytes, so they ARE the
            // "is this HUD element drawn" state:
            //   singles +0xC8, +0x2B8, +0x4A8, +0x5F8, +0x748, +0xF20, +0x1200, +0x1208,
            //           +0x3648, +0x4D58, +0x4E58, +0x6568
            //   arrays  +0x9E0 stride 0x150 x4, +0x1250 stride 0x128 x8, +0x1F08 stride 0x128 x7
            // Every previous round of this investigation correlated against the PHRASE "while the HUD was
            // hidden", which was a fixed log label and not a measurement - and both candidate switches
            // (the menu-state array and the mode byte) were seen back at their gameplay values while the
            // HUD was believed hidden. This digest ends that: it says what the engine thinks is drawn.
            {
                static const unsigned singles[] = {0xC8,   0x2B8,  0x4A8,  0x5F8,  0x748,  0xF20,
                                                   0x1200, 0x1208, 0x3648, 0x4D58, 0x4E58, 0x6568};
                char d[220];
                int n = 0;
                int on = 0, total = 0;
                for (unsigned off : singles)
                {
                    const uint8_t v = *reinterpret_cast<uint8_t *>(feman + off);
                    ++total;
                    if (v)
                        ++on;
                    if (n < (int)sizeof(d) - 16)
                        n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "%X:%u ", off, v);
                }
                struct Arr { unsigned base, stride, count; };
                static const Arr arrays[] = {{0x9E0, 0x150, 4}, {0x1250, 0x128, 8}, {0x1F08, 0x128, 7}};
                for (const Arr &a : arrays)
                {
                    if (n < (int)sizeof(d) - 20)
                        n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "| %X[", a.base);
                    for (unsigned i = 0; i < a.count; ++i)
                    {
                        const uint8_t v = *reinterpret_cast<uint8_t *>(feman + a.base + i * a.stride);
                        ++total;
                        if (v)
                            ++on;
                        if (n < (int)sizeof(d) - 6)
                            n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "%u", v);
                    }
                    if (n < (int)sizeof(d) - 4)
                        n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "] ");
                }
                d[n] = 0;
                spdlog::info("[hudflags] {}: {}/{} widget flags set - {}", when, on, total, d);
            }
            spdlog::info("[feman] {}: mode +0x78={} override +0x82C1={} (effective {})", when,
                         *reinterpret_cast<uint8_t *>(feman + 0x78),
                         *reinterpret_cast<int8_t *>(feman + 0x82C1),
                         *reinterpret_cast<int8_t *>(feman + 0x82C1) >= 0
                             ? *reinterpret_cast<int8_t *>(feman + 0x82C1)
                             : *reinterpret_cast<int8_t *>(feman + 0x78));
            const uintptr_t c = feman + 0x4E70;
            // Trust nothing: the vtable must still be the class we identified, or the offsets moved.
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(c);
            if (vt != base + 0x2A9CC50)
            {
                spdlog::info("[autohide] {}: ctrl vt is exe+0x{:X}, expected exe+0x2A9CC50 - layout moved",
                             when, vt > base ? vt - base : 0);
                return;
            }
            int busy = 0;
            char first[120];
            int n = 0;
            for (int i = 0; i < 42; ++i)
            {
                const uintptr_t slot = c + 0x68 + i * 0x40;
                const uint32_t st = *reinterpret_cast<uint32_t *>(slot);
                const int32_t id = *reinterpret_cast<int32_t *>(slot + 0x30);
                if (st || id != -1)
                {
                    ++busy;
                    if (n < (int)sizeof(first) - 24)
                        n += _snprintf_s(first + n, sizeof(first) - n, _TRUNCATE, "[%d]st=%u id=%d ", i,
                                         st, id);
                }
            }
            first[n] = 0;
            spdlog::info("[autohide] {}: mode={} delay={:.2f} +0xAF0={} +0xAF4={} +0xAF8={} +0xAFC={} "
                         "+0xB00={:.2f} busy={}/42 {}",
                         when, *reinterpret_cast<uint8_t *>(c + 8),
                         *reinterpret_cast<float *>(c + 0xC),
                         *reinterpret_cast<uint32_t *>(c + 0xAF0),
                         *reinterpret_cast<uint8_t *>(c + 0xAF4),
                         *reinterpret_cast<uint8_t *>(c + 0xAF8),
                         *reinterpret_cast<uint32_t *>(c + 0xAFC),
                         *reinterpret_cast<float *>(c + 0xB00), busy, first);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void hud_snapshot_take(uintptr_t base_menu)
    {
        if (!goblin::config::debugLogging || !v3_heap_ptr(base_menu))
            return;
        __try
        {
            // The strongest remaining lead: base menu +0x10 is the SEQUENCE SLOT (the same field we put
            // our own screens into), and after our screen closes it holds a different object than before -
            // the HUD menu's own content was displaced and never put back. Keep a reference to the
            // original so it cannot die while our screen is up, and try restoring it on close.
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            log_job_stack(base_menu, "before our screen is pushed");
            log_autohide("before our screen is pushed");
            log_menuman_map("before our screen is pushed", true);
            // Catch the HIDE in the act. Measured, reproducibly: in gameplay the visible menu ids are
            // {5, 7, 8, 19, 23, 26}, and after our screen closes 7 and 26 are down to "registered only"
            // and stay there until a native menu transition. Writing 3 back into those bytes does NOT
            // bring the HUD back (tried 2026-07-28), so the array is a reflection of state held
            // elsewhere - which means the useful question is who performs the hide, not what the byte
            // says. A hardware write watch on id 7's byte answers exactly that, and the handler now also
            // prints stack values that look like return addresses so the CALLER is named too.
            const uintptr_t man_now = g_menuman.load(std::memory_order_relaxed);
            if (v3_heap_ptr(man_now))
                goblin::watch::request(man_now + 0x90 + 7, GetCurrentThreadId());
            g_hud_orig_job = *reinterpret_cast<void **>(base_menu + 0x10);
            if (v3_heap_ptr(reinterpret_cast<uintptr_t>(g_hud_orig_job)))
                reinterpret_cast<void (*)(void *)>(base + 0x1EBA1C0)(
                    reinterpret_cast<uint8_t *>(g_hud_orig_job) + 8);
            else
                g_hud_orig_job = nullptr;
            memcpy(g_hud_snap_menu, reinterpret_cast<void *>(base_menu), kHudSnapBytes);
            const uintptr_t man = g_menuman.load(std::memory_order_relaxed);
            if (v3_heap_ptr(man))
                memcpy(g_hud_snap_man, reinterpret_cast<void *>(man), sizeof(g_hud_snap_man));
            g_hud_snap_base.store(base_menu, std::memory_order_release);
            g_hud_snap_state.store(1, std::memory_order_release);
            spdlog::info("[hud] snapshot taken of the base menu 0x{:X} before our over-gameplay screen",
                         base_menu);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_hud_snap_state.store(0, std::memory_order_release);
        }
    }

    void hud_snapshot_compare()
    {
        if (g_hud_snap_state.load(std::memory_order_acquire) != 1)
            return;
        g_hud_snap_state.store(2, std::memory_order_release);
        const uintptr_t base_menu = g_hud_snap_base.load(std::memory_order_acquire);
        if (!v3_heap_ptr(base_menu))
            return;
        __try
        {
            int shown = 0;
            for (size_t off = 0; off + 8 <= kHudSnapBytes && shown < 24; off += 8)
            {
                uint64_t before = 0, after = 0;
                memcpy(&before, g_hud_snap_menu + off, 8);
                memcpy(&after, reinterpret_cast<uint8_t *>(base_menu) + off, 8);
                if (before != after)
                {
                    ++shown;
                    spdlog::info("[hud] base menu +0x{:X}: 0x{:X} -> 0x{:X}", off, before, after);
                }
            }
            const uintptr_t man = g_menuman.load(std::memory_order_relaxed);
            if (v3_heap_ptr(man))
            {
                shown = 0;
                for (size_t off = 0; off + 8 <= sizeof(g_hud_snap_man) && shown < 16; off += 8)
                {
                    uint64_t before = 0, after = 0;
                    memcpy(&before, g_hud_snap_man + off, 8);
                    memcpy(&after, reinterpret_cast<uint8_t *>(man) + off, 8);
                    if (before != after)
                    {
                        ++shown;
                        spdlog::info("[hud] CSMenuMan +0x{:X}: 0x{:X} -> 0x{:X}", off, before, after);
                    }
                }
            }
            log_job_stack(base_menu, "after our screen closed");
            log_autohide("after our screen closed");
            log_menuman_map("after our screen closed", true);
            spdlog::info("[hud] compare done - the field that keeps the HUD hidden is among the above");
            if (v3_heap_ptr(man))
            {
                char hex[3 * 64 + 1];
                int n = 0;
                const uint8_t *b = reinterpret_cast<const uint8_t *>(man) + 0x80;
                for (int i = 0; i < 64 && n < (int)sizeof(hex) - 4; ++i)
                    n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", b[i]);
                spdlog::info("[hudprobe] HUD IS NOW HIDDEN (our screen just closed); CSMenuMan+0x80: {}",
                             hex);
                // Keep watching: the HUD comes back on its own once any native menu is opened, and the
                // bytes that change AT THAT MOMENT are the answer. The active-menu dump alone missed it
                // (the pointer at +0x80 does not always change), so sample on a timer for the next
                // minute - every second, only when the window actually differs from the last dump.
                memcpy(g_hudwatch_last, reinterpret_cast<const uint8_t *>(man) + 0x80,
                       sizeof(g_hudwatch_last));
                g_hudwatch_next.store(0, std::memory_order_relaxed);
                g_hudwatch_until.store(GetTickCount64() + 60000, std::memory_order_release);
                // The watch is now armed at PUSH time instead of here, on the byte of menu id 7 - see
                // hud_snapshot_take. Watching after the close was aimed at the restore side, but the
                // measured defect is on the HIDE side: id 7 and id 26 lose their visible bit while our
                // screen is up and never get it back.
            }
            // The two CSMenuMan bytes (+0x97, +0xAA) that go 3 -> 1 across our screen are NOT the gate:
            // writing 3 back was tried on 2026-07-28, both writes fired, and the HUD stayed hidden.
            // Next candidate: the counter at base menu +0x168, which goes up by one per open and never
            // comes down. Same shape of experiment - only act on exactly the delta we recorded.
            // THE MENU-STATE ARRAY IS RULED OUT (2026-07-28, measured three ways). A hardware write watch
            // named the writer (exe+0x734438) and its code gives the semantics: the array at
            // singleton+0x90 is 71 one-byte entries, bit 0 = registered, bit 1 = visible, so 3 = shown,
            // 1 = hidden, 0 = unregistered (`and eax,1` gates it, `or cl,3` shows, `and cl,1` hides).
            // Then: (a) writing 3 back to the two entries that flipped did not restore the HUD;
            // (b) restoring EVERY entry across the whole 71-byte array reported "no entry went from 3 to
            // 1", so our screen does not hide the HUD through this array at all; and (c) when the game
            // itself restores the HUD it ZEROES entries (unregisters them) rather than setting them to 3 -
            // that is cleanup of what our screens left, not the switch.
            // What remains unexplored is the HUD's own objects: it is a set of menus/widgets of its own,
            // and the switch is on that side, not in the menu-state table. Do not spend more runs writing
            // into this array - it has been excluded.
            // TRIED AND REVERTED 2026-07-28. The watch showed the game restoring the HUD by ZEROING the
            // entries our screens left in this window (`+0x94:03->00 +0x97:01->00 +0x98:03->00
            // +0x9B..+0x9E:01->00`), so we did the same for exactly the bytes that were zero before us.
            // It did not bring the HUD back AND it made things worse: pressing ESC right after closing our
            // menu hung the game and crashed it. These entries are live state the menu machinery reads and
            // re-registers, so writing them from our thread races it - no matter which bytes are "right".
            // Conclusion: the HUD must be restored by making the GAME do it (call whatever the menu
            // open/close path calls), not by writing this memory. That is the next line of work, and it
            // needs the function, not another byte.
            // Neither the two CSMenuMan bytes (+0x97/+0xAA, 3 -> 1) nor the counter at +0x168 is the gate:
            // both were written back on 2026-07-28, both writes fired, and the HUD stayed hidden.
            // EXPERIMENT: restore the HUD menu's own sequence-slot job, which the diff shows is a
            // DIFFERENT object after our screen than before. Uses the engine's own slot setter (0x7A9250,
            // which takes its own reference) and then drops the reference we held.
            const uintptr_t base_x = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            void *now_job = *reinterpret_cast<void **>(base_menu + 0x10);
            if (g_hud_orig_job)
            {
                void *held = g_hud_orig_job;
                reinterpret_cast<void (*)(void *, void *)>(base_x + 0x7A9250)(
                    reinterpret_cast<void *>(base_menu + 0x10), &held);
                spdlog::info("[hud] restored the HUD menu's sequence-slot job 0x{:X} (was 0x{:X}) - "
                             "experiment", (uint64_t)g_hud_orig_job, (uint64_t)now_job);
                // inline release (release_job_ref is defined further down in this file): decrement and,
                // if we held the last reference, run the object's own destructor - the same shape.
                auto p_unref_x = reinterpret_cast<int (*)(void *)>(base_x + 0x1EBA200);
                if (p_unref_x(reinterpret_cast<uint8_t *>(g_hud_orig_job) + 8) == 1)
                    (*reinterpret_cast<void (**)(void *)>(
                        *reinterpret_cast<void **>(g_hud_orig_job)))(g_hud_orig_job);
                g_hud_orig_job = nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    Screen *top_screen() { return g_screens.empty() ? nullptr : &g_screens.back(); }

    bool menu_open() { return !g_screens.empty(); }

    // The heartbeat, stamped from the dialog's own per-frame update: it runs for every screen
    // that exists, including one sitting under a child (the input gate stops a parent's
    // COMMANDS, not its update).
    void form_dialog_ticked(uintptr_t dlg)
    {
        const uint64_t now = GetTickCount64();
        const int level = screen_level(dlg);
        if (level >= 0)
            g_screens[static_cast<size_t>(level)].beat = now;
    }

    // Defined further down, next to the primitives they use.
    void open_screen(int32_t page, uintptr_t nest_parent);
    void resume_level(size_t level);
    void close_screen(uintptr_t dlg);
    bool seq_slot_empty(uintptr_t win);

    void build_form_rows()
    {
        goblin::nmenu::rebuild();
        size_t count = 0;
        goblin::nmenu::rows(&count);
        if (count > kFormPoolMax / 2)
            count = kFormPoolMax / 2; // two entries per visual row (left + right column)
        if (form_pool().capacity() < kFormPoolMax)
            form_pool().reserve(kFormPoolMax); // once - the address must stay put
        const size_t level = g_form_depth < kMaxNest ? g_form_depth : kMaxNest - 1;
        ++g_form_generation[level];
        form_pool().clear();
        const int fallback_id = goblin::g_menutext_tab_id;
        for (size_t i = 0; i < count; ++i)
        {
            FormItem it{};
            it.param.text_id = fallback_id;
            it.param.action_ix = static_cast<int32_t>(i % 0x36);
            // Field layout matters: native code that we do NOT replace reads per-device
            // "enabled" bytes out of this row - the mode-0 path tests the byte at +0x10 and
            // mode 1 the byte at +0x11 (verify_nesting.md 2.2), and the conflict pass reads
            // +9/+0xA/+0xB. So keep all of those NON-ZERO and carry our identity in the
            // remaining bits: magic in pad2 (its bytes +9/+0xA/+0xB are 'G','F','M' - all
            // non-zero), generation in pad3, and the row index in the HIGH half of pad4
            // whose low bytes stay 0x01.
            it.param.pad2 = static_cast<int32_t>(kFormMagic);
            it.param.pad3 = static_cast<int32_t>(g_form_generation[level]);
            it.param.pad4 = static_cast<int32_t>((static_cast<uint32_t>(i & 0xFFFF) << 16) |
                                                 0x0101u);
            form_pool().push_back(it);
            // The right column is NOT fed through the grid: real items there scroll together
            // with the left list and read as (disabled) buttons. We paint those clips
            // directly instead - see paint_right_panel().
            form_pool().push_back(FormItem{});
        }
    }

    // Which model row does this native item belong to? Reads the identity out of the
    // param row (magic + generation + index), so stale items simply fail the check.
    const goblin::nmenu::Row *model_row_of(uintptr_t item, size_t *out_index)
    {
        if (form_pool().empty())
            return nullptr;
        const FakeParamRow *row = nullptr;
        __try
        {
            row = *reinterpret_cast<FakeParamRow **>(item + 0x10);
            if (!row || static_cast<uint32_t>(row->pad2) != kFormMagic)
                return nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        // Which screen does this item belong to? Only the LIVE one may be drawn or acted on
        // from the model - a parent's items keep their own pool and their last-drawn text, and
        // rendering them against the child's page would show the wrong row.
        const int level = pool_level_of(row);
        if (level < 0 || static_cast<size_t>(level) != g_form_depth)
            return nullptr;
        if (static_cast<uint32_t>(row->pad3) != g_form_generation[level])
            return nullptr; // an item left over from a previous build
        const uint32_t tag = static_cast<uint32_t>(row->pad4);
        const size_t ix = tag >> 16;
        if (out_index)
            *out_index = ix;
        if ((tag & 0x8000u) != 0)
            return goblin::nmenu::right_row(ix); // right-column preview line
        size_t count = 0;
        const goblin::nmenu::Row *rows = goblin::nmenu::rows(&count);
        if (!rows || ix >= count)
            return nullptr;
        return &rows[ix];
    }

    bool our_form_item(uintptr_t item) { return model_row_of(item, nullptr) != nullptr; }

    using BuildItemsFn = void(void *itemVec, void *list);
    BuildItemsFn *o_build_items = nullptr;
    using RowRenderFn = void *(void *item, void *rowProxy);
    RowRenderFn *o_row_render = nullptr;

    // ── Drawing a row ourselves (hook of the item's vt+0x8, FUN_1408674e0) ───────────
    // Everything the native renderer does is available as small clip-proxy helpers, and
    // crucially FUN_14074a000 takes a RAW wchar_t* - so our rows are not limited to FMG
    // strings and can show numbers, names, anything:
    //   FUN_14074a2f0(parentProxy, outBuf, "Child/Path") - resolve a child clip; the
    //       result is used as (out + 8) for text and destroyed via FUN_140D7F850(out+0x28)
    //   FUN_14074a000(resolved + 8, wchar_t*)            - set the text field's text
    //   FUN_140733340(proxy, visible) / FUN_1407331E0(proxy, grayed)
    //   FUN_1407499E0(proxy + 0x18, "FrameName")         - gotoAndStop by frame label
    //       ("PadCategory"/"KeyCategory" = the section-header look)
    //   FUN_140733150(proxy)                             - is the clip actually there
    // Types are declared here so the SEH body below stays POD-only (MSVC C2712).
    using ResolveFn = void *(void *parent, void *out, const char *path);
    using SetTextFn = void(void *field, const wchar_t *text);
    using SetVisibleFn = void(void *proxy, char on);
    using SetGrayFn = void(void *proxy, char grayed);
    using GotoFrameFn = void(void *proxy, const char *frame);
    using ProxyValidFn = char(void *proxy);
    using ProxyDtorFn = void(void *proxy);

    // One-shot probe: log which child clips a row actually has, so the next step (icons,
    // bars, extra columns) works from facts instead of guesses.
    std::atomic<bool> g_row_probe_done{false};

    const char *const kRowClipNames[] = {"Text_0",   "Text_1",    "Text_2", "ItemIcon",
                                         "Conflict", "Assigning", "Icon",   "Bar",
                                         "HitArea",  "Cursor",    "Lock",   "CursorLock"};
    constexpr size_t kRowClipCount = sizeof(kRowClipNames) / sizeof(kRowClipNames[0]);

    // POD-only: fill found[i] with "does the row have this child clip".
    void probe_row_clips_raw(uintptr_t base, void *rowProxy, char *found)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        __try
        {
            for (size_t i = 0; i < kRowClipCount; ++i)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, kRowClipNames[i]);
                found[i] = p_valid(r);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void probe_row_clips(uintptr_t base, void *rowProxy)
    {
        char found[kRowClipCount] = {};
        probe_row_clips_raw(base, rowProxy, found);
        std::string list;
        for (size_t i = 0; i < kRowClipCount; ++i)
        {
            list += kRowClipNames[i];
            list += found[i] ? "=yes " : "=no ";
        }
        spdlog::info("[form] row clips: {}", list);
    }

    // POD-only row draw. Mirrors the native renderer's call order (frame style -> visible
    // -> Text_0 -> Text_1 -> hide Conflict -> destroy the row proxy).
    //
    // The row clip (sprite cid 189 in 02_160) has FOUR labelled style frames, and they
    // differ in which text fields exist (gfx_02_160_dump.txt):
    //   "Normal"      (frame 1)  Text_0 left 532px + Text_1 right 260px  <- value rows
    //   "Grayout"     (frame 10) same, dimmed                            <- read-only rows
    //   "PadCategory" (frame 19) ONE wide Text_0 (610px), NO Text_1      <- section header
    //   "KeyCategory" (frame 28) second header variant
    // So a row that needs to show a value must NOT use a category frame, or the value
    // field is simply not there. Both text fields are html=1 EditText (MenuFont_01 24pt),
    // which is why passing markup is worth testing.
    using SetScaleFn = void(void *proxy, float sx, float sy);

    // Optional graphic progress bar: the row's Conflict sprite is the only spare graphic
    // in the 02_160 row clip, so show it, squash it to the fraction and tint it
    // (recon_draw_primitives.md: setScale 0x733280, setColor 0x74A1D0).
    void draw_row_bar_clip(uintptr_t base, void *rowProxy, int done, int total)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_visible = reinterpret_cast<SetVisibleFn *>(base + 0x733340);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        auto p_scale = reinterpret_cast<SetScaleFn *>(base + 0x733280);
        float frac = 0.f;
        if (total > 0)
            frac = static_cast<float>(done) / static_cast<float>(total);
        if (frac < 0.f)
            frac = 0.f;
        if (frac > 1.f)
            frac = 1.f;
        __try
        {
            uint8_t buf[0x60] = {};
            void *r = p_resolve(rowProxy, buf, "Conflict");
            if (p_valid(r))
            {
                // Scale only: 0x74A1D0 turned out to be a TEXT-colour setter (its single call
                // site targets a text field, and its four terms are the ADD half of a colour
                // transform), so tinting a plain clip with it is off-label. The clip's own art
                // provides the colour.
                p_visible(r, 1);
                p_scale(r, frac, 0.35f);
            }
            p_dtor(buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void draw_our_row(uintptr_t base, void *rowProxy, const char *styleFrame,
                      const wchar_t *label, const wchar_t *value, bool plate)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_settext = reinterpret_cast<SetTextFn *>(base + 0x74A000);
        auto p_visible = reinterpret_cast<SetVisibleFn *>(base + 0x733340);
        auto p_frame = reinterpret_cast<GotoFrameFn *>(base + 0x7499E0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        __try
        {
            p_visible(rowProxy, 1);
            if (styleFrame)
                p_frame(reinterpret_cast<uint8_t *>(rowProxy) + 0x18, styleFrame);
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, "Text_0");
                if (p_valid(r))
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, label);
                p_dtor(buf + 0x28);
            }
            {
                // ALWAYS write the value field, even when empty: the row clips are reused
                // across pages, so skipping the write leaves the PREVIOUS row's value on
                // screen (seen in-game as values from the parent page bleeding through).
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, "Text_1");
                if (p_valid(r)) // absent on the category frames
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, value ? value : L"");
                p_dtor(buf + 0x28);
            }
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, "Conflict");
                if (p_valid(r))
                    p_visible(r, plate ? 1 : 0);
                p_dtor(buf + 0x28);
            }
            p_dtor(reinterpret_cast<uint8_t *>(rowProxy) + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // POD-only worker (an SEH frame must not hold objects with destructors - MSVC C2712).
    // Mirrors FUN_140868990's shape: clear the vector, then append items. The form's grid
    // is TWO columns wide (left clip cid 189 has Text_0+Text_1, the right one cid 182 has
    // only Text_1) and the native builder appends items in PAIRS - a real item plus an
    // EMPTY filler (FUN_1408686c0) that keeps the right column blank. We do the same, so
    // all our rows stay in the left column and the native scrollbar handles overflow.
    // No finisher pass (FUN_140867820): it only computes key-conflict flags, which would
    // light the Conflict marker on rows that bind nothing.
    bool build_our_form_items(uintptr_t obj, void *list, const FormItem *rows, size_t count)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void *vec = reinterpret_cast<void *>(obj + 8); // &allocator, as the game passes
        __try
        {
            using ClearFn = void(void *, void *, uintptr_t, uintptr_t);
            using AppendFn = void(void *, void *);
            using NormalCtorFn = void *(void *out, void *listCtx, uint32_t kind, const void *row);
            using EmptyCtorFn = void *(void *out);
            auto p_clear = reinterpret_cast<ClearFn *>(base + 0x868f20);
            auto p_append = reinterpret_cast<AppendFn *>(base + 0x868fe0);
            auto p_normal = reinterpret_cast<NormalCtorFn *>(base + 0x866f80);
            auto p_empty = reinterpret_cast<EmptyCtorFn *>(base + 0x8686c0);
            uint8_t scratch[8] = {};
            p_clear(vec, scratch, *reinterpret_cast<uintptr_t *>(obj + 0x10),
                    *reinterpret_cast<uintptr_t *>(obj + 0x18));
            *reinterpret_cast<uintptr_t *>(obj + 0x18) =
                *reinterpret_cast<uintptr_t *>(obj + 0x10); // end = begin (emptied)
            uint8_t item[0x50];
            for (size_t i = 0; i < count; ++i)
            {
                std::memset(item, 0, sizeof(item));
                p_normal(item, list, 0, &rows[i].param); // param row is the first member
                p_append(vec, item);
                // The pool alternates left/right; a right entry with our magic is a real
                // preview line, a zeroed one means "keep this half blank".
                const bool have_right = (i + 1) < count &&
                                        static_cast<uint32_t>(rows[i + 1].param.pad2) == kFormMagic;
                std::memset(item, 0, sizeof(item));
                if (have_right)
                    p_normal(item, list, 0, &rows[i + 1].param);
                else
                    p_empty(item); // right-column filler
                p_append(vec, item);
                ++i; // the right entry was consumed with this pair
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // POD-only: remember the dialog if this list really is dlg+0x1268 (the refresh path
    // also builds into temporary stack lists).
    // Set when the screen's dialog first appears: the movie is parsed by then, so the load
    // interception can stand down until the next open. Acted on outside the SEH frame below.
    std::atomic<bool> g_movie_open_settled{false};

    // The dialog behind a row-item list, or 0 when this list belongs to something else. The
    // class is CHECKED (vtable), never assumed: the refresh path also builds into temporary
    // stack lists, and the player's own key-binding screen uses the very same builder.
    uintptr_t keyconfig_dialog_of(uintptr_t listObj)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            const uintptr_t cand = listObj - 0x1268;
            if (*reinterpret_cast<uintptr_t *>(cand) == base + 0x2B0AC40)
                return cand;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return 0;
    }

    // ── a child screen must REPLACE the page it came from ────────────────────────────
    // Vanilla gets this from the two extra jobs it wraps around the screen (FUN_14094FD40 stores
    // a sequence of three: a lambda, the screen, a lambda) - those handle the page transition.
    // We store the screen alone, so both movies render and the child appeared on top of its
    // parent. Hiding is cheaper than reproducing the sequence and needs nothing new: the root
    // timeline of 02_160 places exactly these named clips (movie dump, "=== ROOT timeline ==="),
    // and one unnamed one at depth 342 (cid 165, the MENU_TitleBase plate) that can only be
    // reached through the movie root itself - so the root is tried first and the named list is
    // the fallback that is known to work (it is how the stray column captions are blanked).
    const char *const kRootClips[] = {"BG",         "StatusBar", "Help",     "SelectKey",
                                      "ActionHelp", "KeySetting", "MenuTitle"};

    // ── when a page may come back ────────────────────────────────────────────────────
    // Vanilla never faces this: it does not hide the page at all. Its opener stores a SEQUENCE of
    // three jobs (FUN_14094FD40) - a transition lambda, the screen, a transition lambda - and the
    // second lambda is what hands the page back, at the moment the engine knows the transition is
    // over. We store the screen alone and hide the page ourselves, so the "when" is ours to
    // answer, and the obvious answer is wrong: the parent's sequence slot empties while the child
    // is still playing its closing animation, which is the white frame that showed up.
    // A fixed wait was only a stand-in. The real signal is the child's own DEATH: its dialog
    // stamps a heartbeat every frame from its update (form_update_detour), and when that stops the
    // movie is gone - there is nothing left to flash. So the page comes back on the first frames
    // after the child stops updating, not on a timer.
    // The root timeline's settled state, between its intro and its close animation (movie dump:
    // f2 'FadeIn', f9 'Loop', f12 'FadeOut').
    constexpr const char *kSettledLabel = "Loop";

    // Turn off the dark rectangle behind the rows. It is the 'BG' child of the row section
    // ('KeySetting' -> 'BG', sprite 169 at depth 1 inside sprite 198). Removing that placement from the
    // movie instead made the screen hang and crash - the dialog needs the child to exist - so we leave it
    // in place and only set it invisible, with the same primitive that shows and hides whole screens.
    // Called ONCE per screen, from the point where the dialog identifies itself: the movie is live there
    // (its rows are being built) and nothing is tearing down, which is what made the earlier per-tick
    // version crash on close.
    void hide_row_panel(uintptr_t dlg)
    {
        if (!dlg)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_visible = reinterpret_cast<SetVisibleFn *>(base + 0x733340);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        __try
        {
            void *root = reinterpret_cast<void *>(dlg + 0x120);
            if (!p_valid(root))
                return;
            uint8_t sect_buf[0x60] = {};
            void *sect = p_resolve(root, sect_buf, "KeySetting");
            if (sect && p_valid(sect))
            {
                uint8_t bg_buf[0x60] = {};
                void *bg = p_resolve(sect, bg_buf, "BG");
                if (bg && p_valid(bg))
                {
                    p_visible(bg, 0);
                    spdlog::info("[form] row panel hidden (KeySetting/BG) on dlg 0x{:X}", dlg);
                }
                else
                    spdlog::info("[form] row panel: 'BG' did not resolve under 'KeySetting'");
                p_dtor(bg_buf + 0x28);
            }
            else
                spdlog::info("[form] row panel: 'KeySetting' did not resolve on dlg 0x{:X}", dlg);
            p_dtor(sect_buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] row panel: SEH while hiding it");
        }
    }

    void set_screen_visible(uintptr_t dlg, bool on)
    {
        if (!dlg)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_visible = reinterpret_cast<SetVisibleFn *>(base + 0x733340);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        bool root_done = false;
        __try
        {
            if (p_valid(root))
            {
                p_visible(root, on ? 1 : 0);
                root_done = true;
                // EXPERIMENT (2026-07-28): shift the whole screen right. The key-binding host lays out in
                // the LEFT half (two-column screen), the graphics host is centred. The previous attempt
                // targeted the per-row clip inside draw_row_icon_own, which never runs on this path (it
                // logged nothing), so this moves the MOVIE ROOT instead - the same proxy setVisible above
                // works on. Set kScreenShiftX to 0 to switch it off; a value that sticks means centring is
                // one constant, a value that snaps back means the engine re-lays the root every frame.
                // (the shift experiment lives in the tick now - this path is not reached for a screen
                //  that is never hidden, which is why two attempts here logged nothing)
                // Coming back, PLAY THE INTRO instead of snapping on. The first F8 open looks
                // smooth for exactly one reason: the movie is created playing and runs its own
                // intro (the root timeline has FrameLabel 'FadeIn' at frame 2, and frames 2+
                // re-place every top-level clip). A page we hid ourselves has no such moment, so
                // it appeared at full brightness in one frame - which is what read as a flash
                // when it landed on top of the closing screen, and as an abrupt pop when it
                // landed after it.
                // Vanilla never needs this: its page-aside/page-back is a method on the SETTINGS
                // dialog (the transition lambdas call *(win+0x1DA0)->vt[0x10](bool) and
                // win->vt[0xA8]()), and our parent is a second copy of the key-binding screen,
                // whose vtable ends at +0x98 - there is no such method to call.
                // The root timeline is a THREE-part state machine, not an intro:
                //     f2 'FadeIn'  ->  f9 'Loop'  ->  f12 'FadeOut'  (to f19)
                // The engine plays FadeIn when a screen opens, leaves it on Loop while it is up
                // and plays FadeOut when it closes. Jumping to FadeIn was tried and it ran
                // straight THROUGH Loop into FadeOut, because nothing stopped it - in game the
                // page appeared for a frame or two and then faded itself away, leaving only the
                // backdrop.
                // So go to the settled state and STOP there: the page cannot wander into its own
                // close animation, and the screen on its way out still covers the moment it
                // appears. Its own FadeOut later still works - the engine starts that itself.
                using GotoAndStopFn = void(void *proxy, const char *label);
                if (on)
                    reinterpret_cast<GotoAndStopFn *>(base + 0x7499E0)(
                        reinterpret_cast<uint8_t *>(root) + 0x18, kSettledLabel);
            }
            // ONLY if the root would not take it. Walking the named clips instead is not
            // equivalent: prepare_form_layout deliberately HIDES two of them (Help and
            // SelectKey - the latter is the "press a key" overlay), so turning the whole list
            // back on flashed that overlay for a frame on every return from a sub-page. The root
            // carries one flag and leaves every per-clip decision the layout made intact.
            for (const char *name : kRootClips)
            {
                if (root_done)
                    break;
                if (on && (std::strcmp(name, "Help") == 0 || std::strcmp(name, "SelectKey") == 0))
                    continue; // the layout wants these off - never turn them back on
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, name);
                if (p_valid(r))
                    p_visible(r, on ? 1 : 0);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] SEH turning screen 0x{:X} {}", dlg, on ? "back on" : "off");
            return;
        }
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true, std::memory_order_acq_rel))
            spdlog::info("[form] screen visibility: movie root {}",
                         root_done ? "accepted setVisible (per-clip state untouched)"
                                   : "did not validate - falling back to named clips");
    }

    // Forget every screen above `level`. Called when a level that is not the top builds or takes
    // input again, which can only mean the screens over it are gone.
    void pop_above(size_t level)
    {
        while (g_screens.size() > level + 1)
        {
            // A screen we have just asked for is not a corpse: the parent often rebuilds its own
            // rows while its child's movie is still coming up, and dropping the entry there would
            // leave the child with no level to land on.
            if (!g_screens.back().dlg && GetTickCount64() - g_screens.back().opened < 3000)
                return;
            spdlog::info("[form] level {} (page {}) closed", g_screens.size() - 1,
                         g_screens.back().page);
            g_screens.pop_back();
            if (g_screens.empty())
                hud_snapshot_compare(); // names the state our close leaves behind (HUD stays hidden)
        }
    }

    void build_items_detour(void *itemVec, void *list)
    {
        const uintptr_t obj = reinterpret_cast<uintptr_t>(itemVec);
        const uintptr_t dlg = menu_open() ? keyconfig_dialog_of(obj) : 0;
        int level = dlg ? screen_level(dlg) : -1;
        if (dlg && level < 0)
        {
            // Not a screen we know. It is OURS only if we have just asked for one and its dialog
            // has not identified itself yet; anything else is the game's own screen and must be
            // left completely alone.
            Screen *pending = top_screen();
            if (pending && !pending->dlg)
            {
                pending->dlg = dlg;
                pending->beat = GetTickCount64();
                pending->shown = pending->beat;
                level = static_cast<int>(g_screens.size()) - 1;
                log_form_command_table(dlg);
                spdlog::info("[form] level {} is dialog 0x{:X} (page {})", level, dlg,
                             pending->page);
                g_movie_open_settled.store(true, std::memory_order_release);
                hide_row_panel(dlg); // the dark rectangle behind our rows
                // The page it came from steps aside NOW - the child's movie exists as of this
                // build, so there is no frame with neither of them on screen.
                if (level > 0)
                {
                    Screen &below = g_screens[static_cast<size_t>(level) - 1];
                    set_screen_visible(below.dlg, false);
                    below.hidden = true;
                }
            }
        }
        else if (dlg && static_cast<size_t>(level) + 1 < g_screens.size())
            pop_above(static_cast<size_t>(level)); // a parent rebuilding = its child has gone
        // A list that is not a dialog's own: the refresh path also builds into TEMPORARY stack
        // lists. Those belong to whichever screen is live and must carry ITS rows - handing them
        // to the game would fill them with the real key binds. Their build must NOT rebuild the
        // model though: doing that bumped the pool generation while the screen's REAL items were
        // still pointing at the old one, which made every row unrecognisable a moment after the
        // screen was drawn (in game: a parent that came back with dead rows).
        const bool temp_list = !dlg && menu_open();
        if (temp_list)
            level = screen_level(g_form_dialog.load(std::memory_order_acquire));
        if (level < 0)
        {
            static std::atomic<int> s_passthrough{0};
            if (s_passthrough.fetch_add(1) < 3)
                spdlog::info("[form] row build passed through to the game (menu {}) - this is "
                             "what leaves the real keybinding list untouched",
                             menu_open() ? "open" : "closed");
            o_build_items(itemVec, list);
            return;
        }
        // From here the model, the pool and the view all describe THIS screen.
        g_form_depth = static_cast<size_t>(level) < kMaxNest ? static_cast<size_t>(level)
                                                             : kMaxNest - 1;
        if (dlg)
        {
            g_form_dialog.store(dlg, std::memory_order_release);
            g_form_update_watch.store(dlg, std::memory_order_release);
        }
        if (g_movie_open_settled.exchange(false, std::memory_order_acq_rel))
            goblin::own_movie::disarm();
        if (!temp_list)
        {
            goblin::nmenu::set_page(g_screens[static_cast<size_t>(level)].page);
            build_form_rows(); // the pool must match the page this screen was just put on
        }
        if (build_our_form_items(obj, list, form_pool().data(), form_pool().size()))
        {
            // How many items actually LANDED, versus how many we appended. The progress page
            // builds 86 items (43 visual rows) but only ~15 rows were reachable in game, so
            // either the item vector truncates or the grid's extent disagrees. The vector is
            // {begin @+0x10, end @+0x18} with stride 0x50; the grid's own view of the list is
            // count/columns/rows at +0xD0/+0xD8/+0xDC.
            size_t landed = 0;
            uint32_t g_count = 0, g_cols = 0, g_rows = 0;
            __try
            {
                const uintptr_t b = *reinterpret_cast<uintptr_t *>(obj + 0x10);
                const uintptr_t e = *reinterpret_cast<uintptr_t *>(obj + 0x18);
                if (e >= b)
                    landed = static_cast<size_t>((e - b) / 0x50);
                const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
                if (dlg)
                {
                    const uintptr_t grid = dlg + 0xA38;
                    g_count = *reinterpret_cast<uint32_t *>(grid + 0xD0);
                    g_cols = *reinterpret_cast<uint32_t *>(grid + 0xD8);
                    g_rows = *reinterpret_cast<uint32_t *>(grid + 0xDC);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            g_form_landed = static_cast<uint32_t>(landed);
            spdlog::info("[form] rows built: {} (page {}, list=0x{:X}) - landed {} items, grid "
                         "count={} cols={} rows={}",
                         form_pool().size(), goblin::nmenu::current_page(), obj, landed, g_count,
                         g_cols, g_rows);
        }
        else
        {
            spdlog::warn("[form] SEH building our rows - falling back to native");
            o_build_items(itemVec, list);
        }
    }

    // ── Independent icon path: our own pixels, our own clip, no movie edited ─────────
    // The spliced route below needs the movie's bytes patched at load. This one does not
    // touch any movie: we build a Scaleform image resource from our own RGBA (RawImage +
    // ScaleformImageResource), create our OWN child sprite in the row via
    // CreateEmptyMovieClip, and fill its drawing context with the resource. Works in any
    // movie, so overhaul mods and game updates cannot collide with it.
    std::atomic<bool> g_icon_diag_done{false};
    std::atomic<bool> g_icon_host_logged{false};
    std::atomic<bool> g_icon_path_probed{false};
    std::atomic<int> g_prehide_report{-1};
    constexpr const char *kOwnIconClip = "MfgIconDraw";
    // -1 means "append": the AS2 implementation turns it into (largest depth in use + 1) and
    // the AS3 one into GetNumChildren(). A positive number is NOT portable - under AS3 it is
    // a child INDEX, and a row clip has only a handful of children, so any index past that
    // range makes AddChildAt fail and the whole call return false.
    constexpr int32_t kOwnIconDepth = -1;
    constexpr float kOwnIconPx = 28.f;

    // iconId -> resource, built once each.
    std::unordered_map<int32_t, void *> g_icon_resources;
    std::atomic<int> g_own_icon_state{0}; // 0 = untried, 1 = working, -1 = unavailable
    // Direct-draw route (native_menu_icons = 3): whether anything has been drawn yet, and a
    // resource to clear with. Both are UI-thread only, like every other draw here.
    bool g_direct_icon_seen = false;
    void *g_blank_resource = nullptr;
    // Which grid slot is being drawn right now. Captured from the engine's own row-path
    // helper (hooked further down as row_path_detour), and needed by every icon route that
    // addresses a row BY PATH from the movie root.
    std::atomic<int32_t> g_row_slot{-1};
    std::atomic<int32_t> g_row_column{0};

    void *icon_resource_for(int32_t icon_id)
    {
        auto it = g_icon_resources.find(icon_id);
        if (it != g_icon_resources.end())
            return it->second;
        // The pixels are already embedded for the map and the overlay: inflate the shared
        // lossless tag for this iconId. Tag payload is {charId u16, fmt u8, w u16, h u16,
        // zlib(ARGB rows)} - premultiplied ARGB, so swizzle to RGBA for the image we build.
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        {
            namespace gen = goblin::generated;
            const gen::MapIconTag *tag = nullptr;
            for (int k = 0; k < gen::MAP_ICON_TAG_COUNT; ++k)
                if (gen::MAP_ICON_TAGS[k].srcIconId == icon_id)
                {
                    tag = &gen::MAP_ICON_TAGS[k];
                    break;
                }
            if (tag && tag->tagLen > 8)
            {
                const unsigned char *b = tag->tag;
                w = b[3] | (b[4] << 8);
                h = b[5] | (b[6] << 8);
                if (w > 0 && h > 0 && w <= 1024 && h <= 1024)
                {
                    std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
                    mz_ulong destlen = static_cast<mz_ulong>(argb.size());
                    if (mz_uncompress(argb.data(), &destlen, b + 7,
                                      static_cast<mz_ulong>(tag->tagLen - 7)) == MZ_OK &&
                        destlen == argb.size())
                    {
                        rgba.resize(argb.size());
                        for (size_t i = 0; i < argb.size(); i += 4)
                        {
                            rgba[i + 0] = argb[i + 1]; // R
                            rgba[i + 1] = argb[i + 2]; // G
                            rgba[i + 2] = argb[i + 3]; // B
                            rgba[i + 3] = argb[i + 0]; // A
                        }
                    }
                }
            }
        }
        if (rgba.empty())
        {
            g_icon_resources[icon_id] = nullptr;
            return nullptr;
        }
        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"MFG_Icon_%05d", icon_id);
        void *res = goblin::sfimage::create_resource(name, w, h, rgba.data());
        g_icon_resources[icon_id] = res;
        return res;
    }

    // OFF. Creating display objects inside someone else's live movie is a dead end, and this
    // is the third crash from it: after the screen closed, Scaleform faulted on poisoned memory
    // (0xC0000005 at exe+0x112CE1A with RCX = 0xCCCCCCCCCC000A56) while tearing the dialog
    // down. The clip and the resolve both worked - the drawing itself was still refused - but
    // the object we added to the row outlives the screen and the engine expects to own
    // everything in that display list. The replacement is the load-time movie transform: the
    // icon becomes an ordinary timeline child that the engine creates and destroys itself, and
    // all we do at runtime is resolve it by name and pick its frame - which is exactly what we
    // already do safely for Text_0 and friends. The code stays as the record of what the live
    // route can and cannot do.
    constexpr bool kEnableOwnIconPath = false;

    // Returns true if the icon was drawn by the independent path.
    bool draw_row_icon_own(uintptr_t base, void *rowProxy, int32_t icon_id)
    {
        if (!kEnableOwnIconPath || icon_id < 0 ||
            g_own_icon_state.load(std::memory_order_relaxed) < 0)
            return false;
        void *res = icon_resource_for(icon_id);
        if (!res)
        {
            static std::atomic<int> s_nores{0};
            if (s_nores.exchange(1) == 0)
                spdlog::info("[sfimage] no image resource for icon {} (art missing or "
                             "RawImage/resource creation refused)", icon_id);
            goblin::sfimage::set_icon_state(goblin::sfimage::IconState::NoImage);
            return false;
        }
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        // Our own child clip only - borrowing one of the row's own clips is not an option:
        // "Conflict" is the RED key-conflict plate, so leaving it visible painted whole rows
        // red, and "CursorLock" has a named child that crashed the movie when re-pointed.
        // The proxy the renderer hands us is the PARENT handle the resolver takes, not the
        // wrapper a resolve RETURNS, so it cannot be asked for its own GFx::Value (the first
        // run showed exactly that: a null interface before the call was even made). Resolving
        // the empty path gives us the row itself as a proper clip proxy.
        // The row proxy carries a second level of indirection: the engine's own text setter is
        // handed proxy+8 and dereferences it before asking for the value. So the object that
        // owns the display value is *(proxy+8), not the proxy we were given. Try both, and
        // dump the raw words once so a wrong guess is visible instead of silent.
        uint8_t self[0x60] = {};
        void *rowClip = p_resolve(rowProxy, self, "");
        const bool have_row = p_valid(rowClip) != 0;
        // EXPERIMENT (2026-07-28): the key-binding host lays its grid out in the LEFT half of the screen
        // (it is a two-column screen: action + bound key), while the graphics host is centred. Shifting
        // each row clip right is the cheapest way to find out whether the position sticks or the engine's
        // own layout overwrites it every frame. Same primitive we already use for the row icon. Set
        // kRowShiftX to 0 to switch the experiment off.
        constexpr int32_t kRowShiftX = 220;
        if (kRowShiftX && have_row)
        {
            using SetPosFn2 = void(void *proxy, int32_t x, int32_t y);
            reinterpret_cast<SetPosFn2 *>(base + 0x733230)(rowClip, kRowShiftX, 0);
            static std::atomic<int> once{0};
            if (once.exchange(1) == 0)
                spdlog::info("[form] row-shift experiment: moving row clips {} px right", kRowShiftX);
        }
        void *inner = *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(rowProxy) + 8);
        if (!g_icon_diag_done.exchange(true, std::memory_order_acq_rel))
        {
            std::string words;
            for (int w = 0; w < 12; ++w)
            {
                char one[24];
                _snprintf_s(one, sizeof(one), _TRUNCATE, "%llX ",
                            static_cast<unsigned long long>(
                                *reinterpret_cast<uint64_t *>(
                                    reinterpret_cast<uint8_t *>(rowProxy) + w * 8)));
                words += one;
            }
            spdlog::info("[sfimage] row proxy vt +0x{:X} inner 0x{:X}, row-as-clip {}",
                         *reinterpret_cast<uintptr_t *>(rowProxy) - base,
                         reinterpret_cast<uintptr_t>(inner), have_row ? "valid" : "invalid");
            spdlog::info("[sfimage] row proxy words: {}", words);
        }
        // The dump settled it: the row handle carries NO display value (words +0x40/+0x48 are
        // zero). It resolves paths against the movie context at +0x20 instead, which is why
        // naming a child works while the handle itself is not a display object - and why it can
        // never be the parent of a clip we create. A resolved child IS a real display object,
        // so the icon gets a home inside one of the row's own clips. HitArea comes first: it
        // spans the row and the menu never draws anything in it.
        // Find the row clip that will host the icon, and only CREATE the child when it is not
        // there yet: CreateEmptyMovieClip appends a brand new sprite on every call, so creating
        // per draw would add one clip per frame. The movie is loaded once for the whole run, so
        // after the first open the clips are simply found again.
        bool made = false;
        const char *host_name = nullptr;
        uint8_t hostbuf[0x60] = {};
        void *hostClip = nullptr;
        for (const char *cand : {"HitArea", "Cursor", "Conflict"})
        {
            std::memset(hostbuf, 0, sizeof(hostbuf));
            void *c = p_resolve(rowProxy, hostbuf, cand);
            if (!p_valid(c))
            {
                p_dtor(hostbuf + 0x28);
                continue;
            }
            uint8_t probe[0x60] = {};
            void *existing = p_resolve(c, probe, kOwnIconClip);
            const bool already = p_valid(existing) != 0;
            p_dtor(probe + 0x28);
            if (already || goblin::sfimage::ensure_child_clip(c, kOwnIconClip, kOwnIconDepth))
            {
                made = true;
                host_name = cand;
                hostClip = c;
                break;
            }
            p_dtor(hostbuf + 0x28);
        }
        if (!g_icon_host_logged.exchange(true, std::memory_order_acq_rel))
            spdlog::info("[sfimage] icon host clip: {}", host_name ? host_name : "none accepted");
        bool drawn = false, resolved = false;
        uint8_t buf[0x60] = {};
        void *r = hostClip ? p_resolve(hostClip, buf, kOwnIconClip) : nullptr;
        resolved = r && p_valid(r) != 0;
        if (resolved)
        {
            using SetPosFn = void(void *proxy, int32_t x, int32_t y);
            reinterpret_cast<SetPosFn *>(base + 0x733230)(r, 2, 4);
            drawn = goblin::sfimage::draw_into(r, res, 0.f, 0.f, kOwnIconPx, kOwnIconPx);
            if (drawn)
                reinterpret_cast<SetVisibleFn *>(base + 0x733340)(r, 1);
        }
        p_dtor(buf + 0x28);
        p_dtor(hostbuf + 0x28);
        p_dtor(self + 0x28);
        {
            static std::atomic<int> s_steps{0};
            if (s_steps.exchange(1) == 0)
                spdlog::info("[sfimage] steps: image=yes clip_created={} clip_resolved={} "
                             "drawn={}", made, resolved, drawn);
        }
        goblin::sfimage::set_icon_state(drawn      ? goblin::sfimage::IconState::Drawn
                                        : resolved ? goblin::sfimage::IconState::NoDraw
                                                   : goblin::sfimage::IconState::NoClip);
        const int want = drawn ? 1 : -1;
        int prev = 0;
        if (g_own_icon_state.compare_exchange_strong(prev, want))
            spdlog::info("[sfimage] own-clip icon path: {}", drawn ? "working" : "unavailable");
        return drawn;
    }

    // ── Icons with NO movie edited at all (native_menu_icons = 3) ───────────────────
    // The two strip variants need the screen's movie rebuilt at load, which means our icon
    // art has to match that movie's row clip byte for byte - a game patch or an overhaul
    // shipping its own 02_160 disables them. This route needs none of that: the pixels
    // become a Scaleform image resource of ours and are drawn straight into the drawing
    // context of a clip the row ALREADY has, so nothing is created, registered or patched.
    //
    // HitArea is the host: it spans the row, the menu never draws anything in it, and unlike
    // Conflict it carries no art of its own that would show through. Drawing must happen
    // AFTER the row's style frame is applied - a gotoAndStop rebuilds the row's display list
    // and takes the drawing with it.
    //
    // The row is reached BY PATH from the movie root, not through the handle the renderer
    // passes: draw_our_row finishes with the native renderer's own `dtor(rowProxy + 0x28)`,
    // so by the time we draw, that handle is dead - the first run said exactly that
    // ("clip=false" for a row whose HitArea the earlier probe had found). The slot to address
    // comes from the engine's own row-path helper, which is already hooked for the strip
    // variants.
    void draw_row_icon_direct(uintptr_t base, int32_t slot, int32_t icon_id)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (!dlg || slot < 0)
            return;
        // Column 0, always: every row of ours is appended as the LEFT half of a pair (the
        // right half is the engine's empty filler). The captured column is simply whichever
        // path the engine formatted last, and the first run caught it on the right-hand one.
        const int32_t column = 0;
        void *res = icon_id >= 0 ? icon_resource_for(icon_id) : nullptr;
        if (res)
            g_blank_resource = res; // any resource will do to clear with a zero-sized rectangle
        bool drawn = false, have_clip = false;
        char path[96];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "KeySetting/ItemList/Item_%d_%d/HitArea", slot,
                    column);
        uint8_t buf[0x60] = {};
        void *clip = p_resolve(reinterpret_cast<void *>(dlg + 0x120), buf, path);
        if (p_valid(clip))
        {
            have_clip = true;
            // A row without an icon still needs the previous row's icon gone: the clips are
            // reused down the list, and a zero-sized rectangle clears the context without
            // drawing anything into it.
            if (res)
                drawn = goblin::sfimage::draw_into(clip, res, 4.f, 4.f, kOwnIconPx, kOwnIconPx);
            else if (g_direct_icon_seen && g_blank_resource)
                goblin::sfimage::draw_into(clip, g_blank_resource, 0.f, 0.f, 0.f, 0.f);
        }
        p_dtor(buf + 0x28);
        if (drawn)
            g_direct_icon_seen = true;
        if (icon_id >= 0)
            goblin::sfimage::set_icon_state(drawn      ? goblin::sfimage::IconState::Drawn
                                            : have_clip ? goblin::sfimage::IconState::NoDraw
                                                        : goblin::sfimage::IconState::NoClip);
        static std::atomic<int> s_reported{0};
        if (icon_id >= 0 && s_reported.exchange(1) == 0)
            spdlog::info("[sfimage] direct icon draw: '{}' clip={} drawn={} (icon {}) - {} "
                         "(value type 0x{:X})", path, have_clip, drawn, icon_id,
                         goblin::sfimage::draw_failure(), goblin::sfimage::last_value_type());
    }

    // Show the category icon in a row, if the movie carries our spliced icon sprite.
    // The sprite has one frame per icon; the frame number for an ini key comes from the
    // generated table. Rows without an icon just hide the clip.
    // POD-only (SEH frames may not hold objects with destructors): which of these paths the
    // engine can resolve from the movie root.
    void probe_icon_paths_raw(uintptr_t base, const char *const *paths, size_t count, char *found)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (!dlg)
            return;
        __try
        {
            void *root = reinterpret_cast<void *>(dlg + 0x120);
            for (size_t i = 0; i < count; ++i)
            {
                uint8_t pb[0x60] = {};
                void *pr = p_resolve(root, pb, paths[i]);
                found[i] = p_valid(pr);
                p_dtor(pb + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Compare the two lookups that disagree, in the same instant and on the same movie:
    // the row handle the renderer gave us, and the explicit path from the movie root. The
    // walker (0xD7F9D0) hands GetMember the value's pdata at +0x28, so if the two pdata
    // pointers differ, the handle simply refers to another instance - which is the only
    // difference left once the type and interface check out.
    void probe_icon_owner_raw(uintptr_t base, void *rowProxy, uint64_t *out)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        __try
        {
            // out[0..2]: row handle type / iface / pdata
            out[0] = *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(rowProxy) + 0x48);
            out[1] = *reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(rowProxy) + 0x40);
            out[2] = *reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(rowProxy) + 0x50);
            // out[3]: does MfgIcon resolve from the handle right now
            uint8_t a[0x60] = {};
            void *ra = p_resolve(rowProxy, a, "MfgIcon");
            out[3] = p_valid(ra) ? 1 : 0;
            p_dtor(a + 0x28);
            const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
            if (!dlg)
                return;
            void *root = reinterpret_cast<void *>(dlg + 0x120);
            // out[4..6]: the same row reached by path - type / pdata / does MfgIcon resolve
            uint8_t b[0x60] = {};
            void *rb = p_resolve(root, b, "KeySetting/ItemList/Item_0_0");
            if (p_valid(rb))
            {
                out[4] = *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(rb) + 0x48);
                out[5] = *reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(rb) + 0x50);
                uint8_t c[0x60] = {};
                void *rc = p_resolve(rb, c, "MfgIcon");
                out[6] = p_valid(rc) ? 1 : 0;
                p_dtor(c + 0x28);
            }
            p_dtor(b + 0x28);
            // out[7..10]: grid cursor and its neighbours, to find the visible-window top
            const uintptr_t grid = *reinterpret_cast<uintptr_t *>(dlg + 0xA38);
            if (grid)
                for (int k = 0; k < 4; ++k)
                    out[7 + k] = *reinterpret_cast<uint32_t *>(grid + 0xD0 + k * 4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void probe_icon_owner(uintptr_t base, void *rowProxy)
    {
        uint64_t v[11] = {};
        probe_icon_owner_raw(base, rowProxy, v);
        spdlog::info("[menuicons] handle: type 0x{:X} iface 0x{:X} pdata 0x{:X} MfgIcon {} | "
                     "by path: type 0x{:X} pdata 0x{:X} MfgIcon {} | grid +0xD0..0xDC: "
                     "{} {} {} {}",
                     v[0], v[1], v[2], v[3] ? "yes" : "no", v[4], v[5], v[6] ? "yes" : "no",
                     v[7], v[8], v[9], v[10]);
    }

    void probe_icon_paths(uintptr_t base)
    {
        const char *const paths[] = {"KeySetting/ItemList/Item_0_0",
                                     "KeySetting/ItemList/Item_0_0/Text_0",
                                     "KeySetting/ItemList/Item_0_0/Conflict",
                                     "KeySetting/ItemList/Item_0_0/MfgIcon"};
        constexpr size_t kCount = sizeof(paths) / sizeof(paths[0]);
        char found[kCount] = {};
        probe_icon_paths_raw(base, paths, kCount, found);
        std::string report;
        for (size_t i = 0; i < kCount; ++i)
        {
            report += paths[i];
            report += found[i] ? "=yes " : "=no ";
        }
        spdlog::info("[menuicons] from movie root: {}", report);
    }

    // The row handle cannot see a child we added to the movie (its value carries type 0, so the
    // path walker never reaches GetMember on it), but the SAME row reached by an explicit path
    // from the movie root resolves it - both were measured side by side, with identical pdata.
    // So icons are addressed by path, and the slot number comes from the engine itself: this is
    // the helper that formats "Item_<slot>_<column>" for the list's own row lookups, so the pair
    // it is called with is exactly the slot about to be drawn.
    struct RowSlotPair
    {
        int32_t slot;
        int32_t column;
    };
    using RowPathFn = void *(void *list, void *out, const RowSlotPair *pair);
    RowPathFn *o_row_path = nullptr;

    void *row_path_detour(void *list, void *out, const RowSlotPair *pair)
    {
        if (pair)
        {
            g_row_slot.store(pair->slot, std::memory_order_release);
            g_row_column.store(pair->column, std::memory_order_release);
        }
        return o_row_path(list, out, pair);
    }

    // ── which grid cell is this row? ─────────────────────────────────────────────────
    // Icons have to be addressed BY PATH from the movie root (the row handle the renderer
    // passes resolves the movie's own declared children but not one we added), so the cell
    // index has to be right. Taking it from the engine's row-path helper did NOT work: that
    // capture is stale by the time we draw, and in game the shift always landed on the same
    // clip - one icon, stuck in the topmost visible row, changing only when the list scrolled.
    //
    // The row handle and the clip reached by path are the SAME display object (both carry the
    // identical pdata at +0x50, measured side by side), so the cell can be identified instead
    // of guessed: resolve each cell once, remember its object, and match. The eleven clips
    // belong to the movie and live as long as it does, so the table is built once.
    constexpr int kGridSlots = 11;
    uint64_t g_slot_obj[kGridSlots] = {};
    bool g_slot_table_ready = false;

    void build_slot_table(uintptr_t base, uintptr_t dlg)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        int found = 0;
        __try
        {
            void *root = reinterpret_cast<void *>(dlg + 0x120);
            for (int s = 0; s < kGridSlots; ++s)
            {
                char path[64];
                _snprintf_s(path, sizeof(path), _TRUNCATE, "KeySetting/ItemList/Item_%d_0", s);
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, path);
                g_slot_obj[s] = p_valid(r)
                                    ? *reinterpret_cast<uint64_t *>(
                                          reinterpret_cast<uint8_t *>(r) + 0x50)
                                    : 0;
                if (g_slot_obj[s])
                    ++found;
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        g_slot_table_ready = found > 0;
        spdlog::info("[menuicons] grid cell table: {} of {} cells identified", found, kGridSlots);
    }

    int slot_of_row(uintptr_t base, uintptr_t dlg, void *rowProxy)
    {
        uint64_t want = 0;
        __try
        {
            want = *reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(rowProxy) + 0x50);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        if (!want)
            return -1;
        for (int pass = 0; pass < 2; ++pass)
        {
            if (!g_slot_table_ready)
                build_slot_table(base, dlg);
            for (int s = 0; s < kGridSlots; ++s)
                if (g_slot_obj[s] == want)
                    return s;
            // A miss means the clips were rebuilt (a fresh open of the screen) - re-measure
            // once, then give up rather than spin.
            g_slot_table_ready = false;
        }
        return -1;
    }

    // rowProxy: the LIVE row handle when the renderer gave us one, else nullptr (the
    // repaint pass has no handle and falls back to the path from the movie root).
    void draw_row_icon(uintptr_t base, int32_t slot, const char *ini_key,
                      void *rowProxy = nullptr)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_visible = reinterpret_cast<SetVisibleFn *>(base + 0x733340);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        using SetFrameNumFn = void(void *proxy, int frame);
        auto p_framenum = reinterpret_cast<SetFrameNumFn *>(base + 0x749980);
        // One-shot: report whether a brand-new spliced clip name resolves at all - that
        // answers the long-standing question and tells us if icons can work this way.
        static std::atomic<int> s_reported{0};
        // The child renders in every row, so it IS in the movie - only the name lookup fails.
        // GFx resolves a name on a display object by property first and then by child name
        // (AS3_GFxValueImpl::GetMember falls back to GetAS3ChildByName), so a timeline child
        // with an instance name should be findable. Bracket the failure once from the movie
        // root: if an explicit path finds it, the row handle is what cannot see it; if even the
        // explicit path fails while its siblings resolve, it is the name itself.
        if (!g_icon_path_probed.exchange(true, std::memory_order_acq_rel))
            probe_icon_paths(base);
        int frame = 0;
        if (ini_key)
            for (const auto &kf : goblin::menu_icon_tags::ICON_FRAME_OF_KEY)
                if (std::strcmp(kf.key, ini_key) == 0)
                {
                    frame = kf.frame;
                    break;
                }
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (!dlg || slot < 0)
        {
            static uint64_t s_last = 0;
            const uint64_t now = GetTickCount64();
            if (now - s_last > 1000)
            {
                s_last = now;
                spdlog::info("[menuicons] no icon drawn: row clip not matched to a grid cell "
                             "(slot {} dlg 0x{:X})", slot, dlg);
            }
            return;
        }
        // Column 0, ALWAYS. Every row of ours is the left half of a pair, and only the left
        // clip (cid 189) carries our icon child - the right one (cid 182) has just a value
        // field. The captured column is whichever path the engine formatted last, and it was
        // observed as 0 in one run and 1 in another: on the 1 runs the resolve targets a clip
        // that has no MfgIcon, so the left column's strip never gets shifted and every row
        // shows the same cell.
        const int32_t column = 0;
        const uint8_t variant = goblin::config::nativeMenuIcons;
        if (variant != 1 && variant != 2)
            return;
        // Icons stop appearing after a row is toggled and come back only on a fresh open, yet
        // neither of the two obvious faults reports itself: the slot resolves and the MfgIcon
        // child is found. So trace what is ACTUALLY applied for a short burst after every view
        // refresh - slot, the ini key it matched, the cell and the x it lands on. A cell of 0 is
        // the blank one, which would look exactly like "the icon vanished".
        auto note = [](const char *what, int32_t s, int32_t f) {
            static uint64_t last = 0;
            const uint64_t now = GetTickCount64();
            if (now - last <= 1000)
                return;
            last = now;
            spdlog::info("[menuicons] no icon drawn: {} (slot {} cell {})", what, s, f);
        };
        // Which cell each slot currently shows, so variant 2 can put the previous one away
        // again. 16 slots x 2 columns covers the list with room to spare.
        static int8_t s_shown[16][2] = {};
        char path[96];
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        // (proxy, int x, int y) - the helper converts to float itself (cvtdq2ps), so passing
        // floats would put the values in the wrong registers entirely.
        using SetPosFn = void(void *proxy, int32_t x, int32_t y);
        __try
        {
            if (variant == 1)
            {
                // One strip of all icons behind a one-cell mask: the icon IS the horizontal
                // offset, and cell 0 is empty, so a row without an icon needs nothing else.
                _snprintf_s(path, sizeof(path), _TRUNCATE,
                            "KeySetting/ItemList/Item_%d_%d/MfgIcon/Strip", slot, column);
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, path);
                if (p_valid(r))
                    reinterpret_cast<SetPosFn *>(base + 0x733230)(
                        r, -frame * goblin::menu_icon_tags::ICON_CELL_PX, 14);
                p_dtor(buf + 0x28);
            }
            else
            {
                // Variant 2 is now the same shape as variant 1 - one named child holding the
                // strip, masked by a sibling - so both are a single resolve and a single move.
                // The placement already sits at (ICON_X, ICON_Y); the position we set is
                // absolute within the row, so the cell offset is folded into x.
                // WHERE the child is resolved from matters. By path from the movie root the shift
                // measurably lands on something that is not what the row draws: after a toggle
                // the trace showed the right cell applied to all eleven slots three times over
                // and the icons stayed blank, while the row's own text - written through the row
                // handle - was correct. So when the renderer hands us the live row handle, ask
                // THAT for its child; the path stays as the route for the repaint pass, which has
                // no handle. The two are compared once per refresh below.
                uint8_t buf[0x60] = {};
                void *r = nullptr;
                if (rowProxy)
                {
                    r = p_resolve(rowProxy, buf, "MfgIcon");
                    if (!p_valid(r))
                    {
                        p_dtor(buf + 0x28);
                        std::memset(buf, 0, sizeof(buf));
                        r = nullptr;
                    }
                }
                if (!r)
                {
                    _snprintf_s(path, sizeof(path), _TRUNCATE,
                                "KeySetting/ItemList/Item_%d_%d/MfgIcon", slot, column);
                    r = p_resolve(root, buf, path);
                }
                const bool ok = p_valid(r) != 0;
                if (ok)
                    // From the origin now: the strip's placement carries no matrix (so nothing
                    // resets it) and the row offset ICON_X/ICON_Y is baked into the strip itself.
                    reinterpret_cast<SetPosFn *>(base + 0x733230)(
                        r, -frame * goblin::menu_icon_tags::ICON_CELL_PX, 0);
                else
                    note("MfgIcon did not resolve in the row", slot, frame);
                // The identity trace that settled this (row handle vs path, both +0x50) is gone
                // now that the answer is known and in the tag: same object, right cell, and the
                // matrix in the placement was what undid it. The two failure notes above stay -
                // they are rate-limited and they are what would speak up if it ever regresses.
                g_prehide_report.store(ok ? 1 : 0, std::memory_order_release);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        // Once per run: which construction is live, plus how many of the icon children the
        // pre-hide pass could actually reach - "resolved 0 of 64" and "hidden but still drawn"
        // are different faults and must not look alike in the log.
        if (s_reported.load() == 0)
            spdlog::info("[menuicons] strip resolved: {} (variant {} slot {} column {}, cell {}, "
                         "x {})",
                         g_prehide_report.load(std::memory_order_acquire) == 1 ? "yes" : "no",
                         variant, slot, column, frame,
                         -frame * goblin::menu_icon_tags::ICON_CELL_PX);
        if (s_reported.exchange(1) == 0)
            spdlog::info("[menuicons] icons via variant {} (slot {} column {})", variant, slot,
                         column);
    }

    void *row_render_detour(void *item, void *rowProxy)
    {
        const uintptr_t it = reinterpret_cast<uintptr_t>(item);
        const goblin::nmenu::Row *row = menu_open() ? model_row_of(it, nullptr) : nullptr;
        if (!row)
        {
            // One of OUR items that is not on the live screen (a parent's row, redrawn while a
            // child is over it): leave it exactly as it was drawn. Handing it to the native
            // renderer would fill it with key-binding data read out of our fake param row.
            uintptr_t param = 0;
            __try
            {
                param = *reinterpret_cast<uintptr_t *>(it + 0x10);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                param = 0;
            }
            if (param && pool_level_of(reinterpret_cast<void *>(param)) >= 0)
                return item;
            return o_row_render(item, rowProxy);
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_row_probe_done.exchange(true, std::memory_order_acq_rel))
            probe_row_clips(base, rowProxy);
        // First draw after opening: the dialog now exists, so stamp the page title.
        if (g_form_title_pending.exchange(false, std::memory_order_acq_rel))
        {
            const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
            if (dlg)
            {
                prepare_form_layout(base, dlg);
                set_form_captions(base, dlg, goblin::nmenu::page_title(), nullptr);
                // First page of a fresh open: the grid was seeded from the bind list.
                sync_grid_extent(dlg, g_form_landed.load(std::memory_order_acquire));
                if (!g_cmd_probe_done.exchange(true, std::memory_order_acq_rel))
                    probe_form_commands(base, dlg);
            }
        }
        // Style per row kind: value rows keep the normal two-field layout, read-only rows
        // use the dimmed frame, and pure separators use the wide category frame.
        const char *style = "Normal";
        switch (row->kind)
        {
        case goblin::nmenu::RowKind::Info:
            style = (row->value && *row->value) ? "Grayout" : "PadCategory";
            break;
        case goblin::nmenu::RowKind::Back:
        case goblin::nmenu::RowKind::SubPage:
        case goblin::nmenu::RowKind::Toggle:
        case goblin::nmenu::RowKind::Number:
        case goblin::nmenu::RowKind::Enum:
        case goblin::nmenu::RowKind::ValueOption:
        case goblin::nmenu::RowKind::Rebind:
        case goblin::nmenu::RowKind::Action:
        case goblin::nmenu::RowKind::Progress:
            style = "Normal";
            break;
        }
        // The live-drawing route needs its host clip BEFORE the style frame is applied (those
        // clips differ per frame); the spliced icon child is the opposite - it must be updated
        // AFTER, because a gotoAndStop rebuilds the row's display list and would hand back a
        // fresh child, visible and playing from frame 1, undoing whatever we just set.
        const bool icon_own = draw_row_icon_own(base, rowProxy, row->icon_id);
        // Identify the grid cell while the row handle is still alive: draw_our_row ends with
        // the native renderer's own proxy dtor, and both icon routes run after it.
        const uintptr_t icon_dlg = g_form_dialog.load(std::memory_order_acquire);
        const int32_t icon_slot = icon_dlg ? slot_of_row(base, icon_dlg, rowProxy) : -1;
        // Keep the caption clear of the icon: the label field starts at x=26.8 and a 32px
        // icon reaches x=34, so a row with a picture gets a small text indent.
        const wchar_t *label = row->label;
        wchar_t indented[512];
        if (row->icon_id >= 0 && label && label[0] &&
            goblin::config::nativeMenuIcons != 0)
        {
            _snwprintf_s(indented, _TRUNCATE, L"    %s", label);
            label = indented;
        }
        draw_our_row(base, rowProxy, style, label, row->value, row->plate);
        if (!icon_own)
        {
            if (goblin::config::nativeMenuIcons == 3)
                draw_row_icon_direct(base, icon_slot, row->icon_id);
            else
                draw_row_icon(base, icon_slot, row->ini_key, rowProxy);
        }
        if (row->kind == goblin::nmenu::RowKind::Progress && goblin::nmenu::graphic_bar())
            draw_row_bar_clip(base, rowProxy, row->collected, row->total);
        return item;
    }

    // The selected model row index, resolved exactly like the form's own handler does:
    //   index = FUN_140739e20(dlg + 0xa38)               (GridControl cursor)
    //   item  = viewList_vt[+0x28](dlg + 0x1268, index)   (MenuViewItemList)
    bool selected_model_index(uintptr_t dlg, size_t *out_index, bool *out_is_preview)
    {
        bool ok = false;
        if (out_is_preview)
            *out_is_preview = false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            using CursorFn = uint32_t(void *grid);
            using ItemAtFn = void *(void *viewList, uint32_t index);
            const uint32_t index = reinterpret_cast<CursorFn *>(base + 0x739e20)(
                reinterpret_cast<void *>(dlg + 0xa38));
            void *viewList = reinterpret_cast<void *>(dlg + 0x1268);
            const uintptr_t vvt = *reinterpret_cast<uintptr_t *>(viewList);
            void *item = (*reinterpret_cast<ItemAtFn **>(vvt + 0x28))(viewList, index);
            const goblin::nmenu::Row *row =
                item ? model_row_of(reinterpret_cast<uintptr_t>(item), out_index) : nullptr;
            if (row)
            {
                ok = true;
                if (out_is_preview)
                    *out_is_preview = row->right_column;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return ok;
    }

    using DecideFn = void(void *dlg);
    DecideFn *o_form_decide = nullptr;

    // Rebuild + redraw the rows in place. dlg+0x1268 = the MenuViewItemList the rows live
    // in, dlg+0x1290 = CSMenuKeyConfig (the bind manager the item ctors take).
    // FUN_140942690 is the game's own view refresh; it READS the grid cursor (grid+0xd4)
    // and never moves it, so the selection survives a rebuild.
    // Write the current page title (and a hint line) into the form's own caption clips.
    // dlg+0x120 is the movie's clip-root proxy - the ctor resolves "MenuTitle/Text",
    // "ActionHelp/Text_0" and friends from it (FUN_14093d290), so we can retarget them.
    void set_form_captions(uintptr_t base, uintptr_t dlg, const wchar_t *title,
                           const wchar_t *hint)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_settext = reinterpret_cast<SetTextFn *>(base + 0x74A000);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        __try
        {
            if (title)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, "MenuTitle/Text");
                if (p_valid(r))
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, title);
                p_dtor(buf + 0x28);
            }
            if (hint)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, "ActionHelp/Text_0");
                if (p_valid(r))
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, hint);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Make the screen look like a settings screen rather than a keybinding screen:
    //  * KeySetting/BG frame 2 = the WIDE two-column panel (frame 1 is the narrow one
    //    that leaves room for the gamepad art). The ctor itself does
    //    FUN_140749980(bgProxy+0x18, mode + 1), so frame 2 is what device mode 1 uses.
    //  * hide the device preview clips (SelectKey/*, Help/*) - a settings list has no
    //    button to point at.
    // recon_02_160_structure.md, section "visual element we want".
    uint32_t g_caption_hits = 0;

    void prepare_form_layout(uintptr_t base, uintptr_t dlg)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_visible = reinterpret_cast<SetVisibleFn *>(base + 0x733340);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        using SetFrameNumFn = void(void *proxy, int frame);
        auto p_framenum = reinterpret_cast<SetFrameNumFn *>(base + 0x749980);
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        static const char *const kHide[] = {
            "SelectKey/Win64", "SelectKey/PS4",  "SelectKey/PS5",     "SelectKey/XboxOne",
            "SelectKey/XboxSeries", "Help/Win64", "Help/PS4",         "Help/PS5",
            "Help/XboxOne",   "Help/XboxSeries", "SelectKey",         "Help"};
        __try
        {
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, "KeySetting/BG");
                if (p_valid(r))
                    p_framenum(reinterpret_cast<uint8_t *>(r) + 0x18, 2);
                p_dtor(buf + 0x28);
            }
            for (const char *name : kHide)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, name);
                if (p_valid(r))
                    p_visible(r, 0);
                p_dtor(buf + 0x28);
            }
            static const char *const kCaptions[] = {
                "KeySetting/StaticText_280005",          "KeySetting/StaticText_280006",
                "KeySetting/ItemList/StaticText_280005", "KeySetting/ItemList/StaticText_280006",
                "KeySetting/BG/StaticText_280005",       "KeySetting/BG/StaticText_280006",
                "StaticText_280005",                     "StaticText_280006"};
            // The RIGHT column - the "second bind" half the gamepad screen does not even have.
            // Our rows are all left-column, so those eleven clips are dead furniture under the
            // caption we just removed. Unlike the captions they ARE named, so a plain hide works.
            for (int r = 0; r < 11; ++r)
            {
                char rp[64];
                _snprintf_s(rp, sizeof(rp), _TRUNCATE, "KeySetting/ItemList/Item_%d_1", r);
                uint8_t rb[0x60] = {};
                void *rr = p_resolve(root, rb, rp);
                if (p_valid(rr))
                    p_visible(rr, 0);
                p_dtor(rb + 0x28);
            }
            for (size_t c = 0; c < sizeof(kCaptions) / sizeof(kCaptions[0]); ++c)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, kCaptions[c]);
                if (p_valid(r))
                {
                    p_visible(r, 0);
                    g_caption_hits |= (1u << c);
                }
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        static bool s_caption_logged = false;
        if (!s_caption_logged)
        {
            s_caption_logged = true;
            spdlog::info("[form] stray column captions blanked: mask 0x{:X}", g_caption_hits);
        }
    }

    // One-shot: log the input commands the form has registered (vector win+0x1F8..+0x200,
    // stride 0x140; the name/key pack lives at entry+0x40 and FUN_140745170 hands it to
    // us). Knowing the real command set is the prerequisite for taking over Back (so it
    // walks our page stack instead of closing) and for adding left/right on value rows -
    // recon_screen_hierarchy.md section 6.
    size_t count_form_commands(uintptr_t dlg, uintptr_t *first, uintptr_t *last)
    {
        __try
        {
            *first = *reinterpret_cast<uintptr_t *>(dlg + 0x1F8);
            *last = *reinterpret_cast<uintptr_t *>(dlg + 0x200);
            if (!*first || *last < *first)
                return 0;
            return static_cast<size_t>((*last - *first) / 0x140);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // POD-only: pull one command's display name into caller-provided storage.
    bool form_command_name(uintptr_t base, uintptr_t entry, wchar_t *out, size_t cap)
    {
        out[0] = 0;
        __try
        {
            using PackFn = void *(uintptr_t entry, void *outPack);
            uint8_t pack[0x90] = {};
            void *p = reinterpret_cast<PackFn *>(base + 0x745170)(entry, pack);
            if (!p)
                return false;
            // pack layout (cmdlist RE): DLString name @+0x8 - inline buffer unless long.
            const uint8_t *str = reinterpret_cast<const uint8_t *>(p) + 8;
            const wchar_t *chars = *reinterpret_cast<const wchar_t *const *>(str);
            const uint64_t len = *reinterpret_cast<const uint64_t *>(str + 0x28);
            if (!chars || len == 0 || len > 256)
                chars = reinterpret_cast<const wchar_t *>(str + 0x10); // inline
            for (size_t i = 0; i + 1 < cap && chars[i]; ++i)
            {
                out[i] = chars[i];
                out[i + 1] = 0;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void probe_form_commands(uintptr_t base, uintptr_t dlg)
    {
        uintptr_t first = 0, last = 0;
        const size_t n = count_form_commands(dlg, &first, &last);
        spdlog::info("[form] registered commands: {}", n);
        for (size_t i = 0; i < n && i < 16; ++i)
        {
            const uintptr_t entry = first + i * 0x140;
            wchar_t name[64] = {};
            const bool ok = form_command_name(base, entry, name, 64);
            char utf8[128] = {};
            if (ok)
                WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
            spdlog::info("[form]   cmd[{}] entry=0x{:X} action=0x{:X} name='{}'", i, entry,
                         *reinterpret_cast<uintptr_t *>(entry + 0xF8), utf8);
        }
    }

    // Paint the preview into the right-hand column by writing the fixed row clips
    // ("KeySetting/ItemList/Item_<n>_1") straight from the movie root. Those slots are not
    // part of the grid's data, so the panel stays put while the left list scrolls, and it
    // never behaves like a row the cursor can land on.
    void paint_right_panel(uintptr_t base, uintptr_t dlg)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_settext = reinterpret_cast<SetTextFn *>(base + 0x74A000);
        auto p_visible = reinterpret_cast<SetVisibleFn *>(base + 0x733340);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        for (int n = 0; n < 11; ++n)
        {
            const goblin::nmenu::Row *row = goblin::nmenu::right_row(static_cast<size_t>(n));
            char path[64];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "KeySetting/ItemList/Item_%d_1", n);
            __try
            {
                uint8_t buf[0x60] = {};
                void *slot = p_resolve(root, buf, path);
                if (p_valid(slot))
                {
                    p_visible(slot, row ? 1 : 0);
                    if (row)
                    {
                        // Strip the row furniture so it reads as text, not as a button.
                        for (const char *child : {"Cursor", "CursorLock", "Conflict", "HitArea"})
                        {
                            uint8_t cbuf[0x60] = {};
                            void *c = p_resolve(slot, cbuf, child);
                            if (p_valid(c))
                                p_visible(c, 0);
                            p_dtor(cbuf + 0x28);
                        }
                        // cid 182 (the right column) has ONE text field, so the label and
                        // the value share it, laid out with HTML rather than two clips.
                        wchar_t line[192];
                        if (row->value && row->value[0])
                            _snwprintf_s(line, _TRUNCATE,
                                         L"<p align=\"left\">%s   <font color=\"#C8B48C\">%s"
                                         L"</font></p>",
                                         row->label ? row->label : L"", row->value);
                        else
                            _snwprintf_s(line, _TRUNCATE, L"<p align=\"left\">%s</p>",
                                         row->label ? row->label : L"");
                        uint8_t tbuf[0x60] = {};
                        void *t = p_resolve(slot, tbuf, "Text_1");
                        if (p_valid(t))
                            p_settext(reinterpret_cast<uint8_t *>(t) + 8, line);
                        p_dtor(tbuf + 0x28);
                    }
                }
                p_dtor(buf + 0x28);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                break;
            }
        }
    }

    // The screen's help line is rewritten by the game every frame from an FMG id (which for
    // our rows resolves to the placeholder "GR_LineHelp(...)"), so we overwrite it AFTER the
    // engine's update with the highlighted row's own description - the text the overlay used
    // to show as a tooltip.
    void paint_right_panel(uintptr_t base, uintptr_t dlg);

    // Off for now. Painting the preview into the right-hand row clips works, but those clips
    // carry the list's own button plate (an unnamed timeline child, so it cannot be hidden by
    // name, and cid 182 has no plate-less frame), and the result reads as a column of dead
    // buttons. The replacement is to draw that half ourselves once the image path is proven;
    // the code stays so the experiment is one flag away.
    constexpr bool kPaintRightPanel = false;

    void paint_help_line()
    {
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (!dlg || !menu_open())
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        size_t ix = 0;
        bool preview = false;
        if (!selected_model_index(dlg, &ix, &preview) || preview)
            return;
        size_t count = 0;
        const goblin::nmenu::Row *rows = goblin::nmenu::rows(&count);
        if (!rows || ix >= count)
            return;
        // With no description of its own a row would blank the block, which reads as a
        // glitch next to the rows that do have one. Fall back to what we know: the row's own
        // caption and its current value.
        wchar_t fallback[256];
        const wchar_t *tip = rows[ix].help;
        if (!tip || !tip[0])
        {
            const wchar_t *label = rows[ix].label ? rows[ix].label : L"";
            const wchar_t *value = rows[ix].value ? rows[ix].value : L"";
            if (value[0])
                _snwprintf_s(fallback, _TRUNCATE, L"%s: %s", label, value);
            else
                _snwprintf_s(fallback, _TRUNCATE, L"%s", label);
            tip = fallback;
        }
        auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
        auto p_settext = reinterpret_cast<SetTextFn *>(base + 0x74A000);
        auto p_valid = reinterpret_cast<ProxyValidFn *>(base + 0x733150);
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(base + 0xD7F850);
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        __try
        {
            // The descriptions come from the ini schema, where the line breaks are AUTHORED so the
            // generated file wraps nicely. In the menu they are HARD breaks, which is what read as
            // "wraps by character count, left of centre" - our newline, not the field's word wrap.
            // Collapse them into spaces and let the field decide where a line ends.
            wchar_t flat[1024];
            size_t fi = 0;
            for (const wchar_t *c = tip; *c && fi + 1 < _countof(flat); ++c)
                flat[fi++] = (*c == 10 || *c == 13) ? L' ' : *c;
            flat[fi] = 0;
            uint8_t buf[0x60] = {};
            void *r = p_resolve(root, buf, "ActionHelp/Text_0");
            if (p_valid(r))
                p_settext(reinterpret_cast<uint8_t *>(r) + 8, flat);
            p_dtor(buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (kPaintRightPanel)
            paint_right_panel(base, dlg);
    }

    // ── the strips have to be re-applied AFTER the engine's own refresh ──────────────
    // Measured (13:19 run): after a toggle our draw runs for all eleven slots and applies the
    // right cell for each - and the icons are still blank. The engine's refresh only marks the
    // view dirty (FUN_140942690 sets dlg+0x11FD); the render that follows re-applies each row's
    // style frame, which rebuilds the row's display list and hands the spliced icon child back at
    // its authored position - the blank cell. So our shift is undone on exactly those refreshes
    // that keep the same rows, and comes back the moment the list is scrolled, because a scroll
    // renders rows through the path where our draw runs last. Fix: repaint the strips on the few
    // ticks AFTER a refresh, when that render has happened.
    std::atomic<int> g_icon_repaint_left{0};

    // POD: the item the view list has bound to a visible slot, or nullptr.
    void *view_item_at(uintptr_t dlg, uint32_t index)
    {
        __try
        {
            using ItemAtFn = void *(void *viewList, uint32_t index);
            void *viewList = reinterpret_cast<void *>(dlg + 0x1268);
            const uintptr_t vvt = *reinterpret_cast<uintptr_t *>(viewList);
            return (*reinterpret_cast<ItemAtFn **>(vvt + 0x28))(viewList, index);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // POD: the grid fields that map a visible slot to a data index.
    bool read_grid_window(uintptr_t dlg, int32_t *top, uint32_t *cols, int32_t *flat_base)
    {
        __try
        {
            // +0x348 first visible row, +0xD8 columns, +0xE0 flat base - the same fields the
            // engine's own slot-to-index helper reads (documented with the grid layout below).
            const uintptr_t grid = dlg + 0xA38;
            *top = *reinterpret_cast<int32_t *>(grid + 0x348);
            *cols = *reinterpret_cast<uint32_t *>(grid + 0xD8);
            *flat_base = *reinterpret_cast<int32_t *>(grid + 0xE0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void repaint_row_icons(uintptr_t dlg)
    {
        const uint8_t variant = goblin::config::nativeMenuIcons;
        if (!dlg || (variant != 1 && variant != 2))
            return;
        int32_t top = 0, flat_base = 0;
        uint32_t cols = 1;
        if (!read_grid_window(dlg, &top, &cols, &flat_base))
            return;
        if (!cols)
            cols = 1;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        for (int slot = 0; slot < kGridSlots; ++slot)
        {
            // index = (slotRow + firstVisible) * columns + slotCol + flatBase, the engine's own
            // slot-to-data mapping (FUN_140739F70); our rows are always column 0.
            const int32_t index = (slot + top) * static_cast<int32_t>(cols) + flat_base;
            if (index < 0)
                continue;
            void *item = view_item_at(dlg, static_cast<uint32_t>(index));
            const goblin::nmenu::Row *row =
                item ? model_row_of(reinterpret_cast<uintptr_t>(item), nullptr) : nullptr;
            if (!row)
                continue;
            draw_row_icon(base, slot, row->ini_key);
        }
    }

    void repaint_row_icons_after_advance(uintptr_t dlg)
    {
        if (!menu_open() || dlg != g_form_dialog.load(std::memory_order_acquire))
            return;
        repaint_row_icons(dlg);
    }

    void refresh_form_view(uintptr_t dlg)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        set_form_captions(base, dlg, goblin::nmenu::page_title(), nullptr);
        __try
        {
            using RebuildFn = void(void *viewList, void *mgr);
            using RefreshFn = void(void *dlg);
            // The HOOKED builder, so our rows are the ones rebuilt.
            reinterpret_cast<RebuildFn *>(base + 0x868590)(reinterpret_cast<void *>(dlg + 0x1268),
                                                          reinterpret_cast<void *>(dlg + 0x1290));
            reinterpret_cast<RefreshFn *>(base + 0x942690)(reinterpret_cast<void *>(dlg));
            // The refresh leaves the grid's item count at the PREVIOUS page's value, so the
            // list is only walkable up to that many items. Restate it from what the rebuild
            // actually landed - after the refresh, never before: writing it first is what let
            // the cursor run past the real end (rows there are never rendered, so our icon
            // strips fell back to their empty cell and the pictures vanished).
            sync_grid_extent(dlg, g_form_landed.load(std::memory_order_acquire));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] SEH refreshing rows after a row action");
        }
    }

    // ── the list position belongs to the PAGE, not to the screen ─────────────────────
    // The grid keeps one cursor for the whole screen, so entering a short sub-page from a
    // scrolled-down long one left the view parked below its last row - the player had to
    // scroll up to see anything. The cursor is a plain field (grid+0xD4, which the game's own
    // reader FUN_140739E20 just returns), so each level's position is remembered on the way
    // down and put back on the way out.
    // GridControl layout, established by disassembling its own helpers:
    //   +0xD0 item COUNT   - FUN_14073A0A0 takes (count - 1) as the highest index
    //   +0xD4 cursor       - a FLAT index over all items (FUN_140739E20 returns it)
    //   +0xD8 columns      - FUN_140739F10 splits a flat index by it
    //   +0xDC visible rows - FUN_14073A1C0 returns rows * columns = page capacity
    // Writing the cursor alone did NOT move the view, and the count is why: the game never
    // needs to change it (a key-binding list has a fixed length), so nothing refreshes it when
    // we swap the rows for a shorter page - the grid keeps paging a list that is no longer
    // there. Both fields are set together.
    constexpr size_t kGridCountOff = 0xD0;
    constexpr size_t kGridCursorOff = 0xD4;
    // THE SCROLL WINDOW, found by following how a slot becomes a data index. FUN_140736F20
    // gets the cursor's row clip as `slot = cursor - FUN_1407374A0(grid, 0)`, and that resolves
    // through FUN_140739F70, which computes
    //     index = (slotRow + *(int*)(grid + 0x348)) * columns + slotCol + *(int*)(grid + 0xE0)
    // So +0x348 is the FIRST VISIBLE ROW and +0xE0 a flat base offset. Neither the cursor nor
    // the item count takes part - which is exactly why setting those two moved nothing.
    constexpr size_t kGridTopRowOff = 0x348;
    // TOTAL ROWS, and the field the clamp actually consults. Found by scanning the grid for a
    // stale value: with 86 items in the list this still read 15 - the row count of the page we
    // replaced - which is why the list stopped at 15 rows no matter what was written to +0xD0.
    // That one is only a mirror. The pair {first visible @+0x348, total @+0x34C} sits together.
    constexpr size_t kGridTotalRowsOff = 0x34C;
    constexpr size_t kGridBaseOff = 0xE0;

    // State the extent of the list we just built. Called AFTER the game's own refresh, which
    // leaves both fields describing the previous page.
    void sync_grid_extent(uintptr_t dlg, uint32_t items)
    {
        if (!items)
            return;
        __try
        {
            const uintptr_t grid = dlg + 0xA38;
            uint32_t cols = *reinterpret_cast<uint32_t *>(grid + 0xD8);
            if (cols == 0)
                cols = 1;
            const uint32_t rows = (items + cols - 1) / cols;
            *reinterpret_cast<uint32_t *>(grid + kGridCountOff) = items;
            *reinterpret_cast<uint32_t *>(grid + kGridTotalRowsOff) = rows;
            // A page that shrank can leave the window past its end, where nothing renders.
            const uint32_t visible = *reinterpret_cast<uint32_t *>(grid + 0xDC);
            int32_t *top = reinterpret_cast<int32_t *>(grid + kGridTopRowOff);
            const int32_t max_top =
                rows > visible ? static_cast<int32_t>(rows - visible) : 0;
            if (*top > max_top)
                *top = max_top;
            uint32_t *cur = reinterpret_cast<uint32_t *>(grid + kGridCursorOff);
            if (*cur >= items)
                *cur = items ? items - 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    ListPos read_list_pos(uintptr_t dlg)
    {
        ListPos p{0, 0};
        __try
        {
            const uintptr_t grid = dlg + 0xA38;
            p.cursor = *reinterpret_cast<uint32_t *>(grid + kGridCursorOff);
            p.top_row = *reinterpret_cast<int32_t *>(grid + kGridTopRowOff);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return p;
    }

    void write_list_pos(uintptr_t dlg, ListPos p)
    {
        __try
        {
            const uintptr_t grid = dlg + 0xA38;
            const uint32_t count = *reinterpret_cast<uint32_t *>(grid + kGridCountOff);
            if (count && p.cursor >= count)
                p.cursor = count - 1;
            if (p.top_row < 0)
                p.top_row = 0;
            *reinterpret_cast<uint32_t *>(grid + kGridCursorOff) = p.cursor;
            *reinterpret_cast<int32_t *>(grid + kGridTopRowOff) = p.top_row;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // With one screen per page the list position needs no stack of its own: each screen keeps its
    // own grid, and a resuming parent is restored from the entry that describes it.

    // ── The screen's own Back button walks OUR page stack ────────────────────────────
    // The window dispatches input by walking its command vector and running the first
    // entry whose trigger accepts the input and whose predicate allows it. Disassembled
    // at 0x140745662, the action call is `MOV RCX,[entry+0xF8]` then `CALL [RAX+0x10]` -
    // ONE argument, and the return value is dropped. So taking Back over is a single
    // pointer write: swap the action holder's impl for one of ours that pops a page and
    // only delegates to the original once we are back at the root.
    //
    // Which entry is Back comes from the game, not from a guess: every trigger is a
    // _Func_impl built by FUN_14075E8B0, which stores {u32 actionId, u8 flags} right
    // after the vptr, and the factory that carries the "Close" caption (GR_KeyGuide
    // 110001, the twin of 110000 "Back") registers action id 0x18. Both the vtable and
    // the id are checked, so an entry of any other shape is skipped rather than misread.
    constexpr uint32_t kInputActionBack = 0x18;
    constexpr uintptr_t kInputTriggerVtable = 0x2A99768;
    constexpr size_t kTriggerActionIdOff = 8;
    constexpr size_t kCmdTriggerOff = 0x38;
    constexpr size_t kCmdActionOff = 0xF8;

    // POD-only: the action id behind one command entry, or false if this is not a trigger
    // of the shape we know.
    bool command_action_id(uintptr_t base, uintptr_t entry, uint32_t *out)
    {
        __try
        {
            const uintptr_t impl = *reinterpret_cast<uintptr_t *>(entry + kCmdTriggerOff);
            if (!impl)
                return false;
            if (*reinterpret_cast<uintptr_t *>(impl) != base + kInputTriggerVtable)
                return false;
            *out = *reinterpret_cast<uint32_t *>(impl + kTriggerActionIdOff);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ── which input action does ESC actually produce? ────────────────────────────────────────────
    // The command table proved that our screen HAS a command accepting two actions (0x25 or 0x35,
    // guard allows) besides Back (0x18), so if ESC mapped to one of those the engine would already be
    // closing us. It does not - therefore either ESC produces some other action id, or nothing at all
    // reaches the screen. exe+0x758500 is the engine's own "is this action present in this input?"
    // predicate, called by every matcher lambda with (InputData*, actionId, capturedState*). Logging
    // what it is ASKED and what it ANSWERS while ESC is physically down settles it.
    //
    // This is a HOT path, so the detour is inert unless armed: one of our screens must be open, the
    // ESC key must have gone down within the last second, and at most kActionLogCap lines are printed
    // per arming. Everything else is a straight tail-call to the original.
    using ActionTestFn = char(void *input, uint32_t action, void *state);
    ActionTestFn *o_action_test = nullptr;
    std::atomic<uint64_t> g_action_log_until{0};
    std::atomic<int> g_action_log_left{0};
    constexpr int kActionLogCap = 60;

    char action_test_detour(void *input, uint32_t action, void *state)
    {
        const char r = o_action_test ? o_action_test(input, action, state) : 0;
        if (g_action_log_until.load(std::memory_order_relaxed) &&
            GetTickCount64() <= g_action_log_until.load(std::memory_order_relaxed) &&
            g_action_log_left.fetch_sub(1, std::memory_order_relaxed) > 0)
            spdlog::info("[action] asked 0x{:X} -> {}", action, r ? "YES" : "no");
        return r;
    }

    // Called from the per-frame upkeep: arms the log for one second on an ESC down-edge.
    void arm_action_log_on_escape()
    {
        static bool was_down = false;
        const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (down && !was_down && goblin::config::debugLogging)
        {
            g_action_log_left.store(kActionLogCap, std::memory_order_relaxed);
            g_action_log_until.store(GetTickCount64() + 1000, std::memory_order_relaxed);
            spdlog::info("[action] ESC went down - logging the next {} action queries", kActionLogCap);
        }
        was_down = down;
    }

    // ── what the screen's command table actually contains ────────────────────────────────────────
    // The dispatcher (0x140745623..0x14074567A) runs a command only when its MATCHER (+0x38) accepts
    // the current input AND its GUARD (+0x138) allows it, then calls the ACTION (+0xF8). Our
    // command_action_id() only recognises ONE matcher lambda (vtable exe+0x2A99768,
    // std::function<bool(const CS::InputData&)>), so any entry built by a different lambda has been
    // invisible to us - which is why we only ever found Back (0x18).
    //
    // The player's report reframes the problem: Q is "one step back", ESC is "close all the way to
    // gameplay", and every native screen closes on ESC - including the very screen we borrow. So the
    // ESC entry should be IN this table; either its matcher is a kind we do not recognise, or its
    // guard refuses in our context. This dump answers which, by listing every entry with its matcher
    // vtable (nameable offline with scratch/rtti_map.py), its guard's verdict and its label.
    bool command_guard_ok(uintptr_t entry, bool *known)
    {
        *known = false;
        __try
        {
            const uintptr_t guard = *reinterpret_cast<uintptr_t *>(entry + 0x138);
            if (!guard)
                return false;
            auto fn = *reinterpret_cast<char (**)(void *)>(*reinterpret_cast<uintptr_t *>(guard) + 0x10);
            if (!fn)
                return false;
            *known = true;
            return fn(reinterpret_cast<void *>(guard)) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // A SECOND matcher shape, decoded from its _Do_call at exe+0x75D2F0 (vtable exe+0x2A997D8): it
    // captures TWO action ids, at +0x8 and +0xC, and matches if EITHER is present in the InputData
    //     edx = [self+8];  if (test(input, edx, self+0x10)) return true;
    //     edx = [self+0xC]; return test(input, edx, self+0x10);
    // That is the shape a "Q or Escape" command would have, and command_action_id() cannot read it
    // because it insists on the single-id lambda's vtable. Returns the count of ids read (0, 1 or 2).
    constexpr uintptr_t kInputTrigger2Vtable = 0x2A997D8;

    int command_action_ids(uintptr_t base, uintptr_t entry, uint32_t *a, uint32_t *b)
    {
        __try
        {
            const uintptr_t impl = *reinterpret_cast<uintptr_t *>(entry + kCmdTriggerOff);
            if (!impl)
                return 0;
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(impl);
            if (vt == base + kInputTriggerVtable)
            {
                *a = *reinterpret_cast<uint32_t *>(impl + 8);
                return 1;
            }
            if (vt == base + kInputTrigger2Vtable)
            {
                *a = *reinterpret_cast<uint32_t *>(impl + 8);
                *b = *reinterpret_cast<uint32_t *>(impl + 0xC);
                return 2;
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // POD-only, so it may hold the __try: MSVC refuses __try in a function that also needs object
    // unwinding (C2712), and the logging below unavoidably builds temporaries.
    unsigned long long command_matcher_vt(uintptr_t entry, uintptr_t base)
    {
        __try
        {
            const uintptr_t m = *reinterpret_cast<uintptr_t *>(entry + kCmdTriggerOff);
            if (!m)
                return 0;
            const uintptr_t v = *reinterpret_cast<uintptr_t *>(m);
            if (v > base && v - base < 0x10000000)
                return (unsigned long long)(v - base);
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void log_form_command_table(uintptr_t dlg)
    {
        if (!goblin::config::debugLogging || !dlg)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uintptr_t first = 0, last = 0;
        const size_t n = count_form_commands(dlg, &first, &last);
        spdlog::info("[cmdtable] screen 0x{:X} has {} command(s)", dlg, n);
        for (size_t i = 0; i < n && i < 32; ++i)
        {
            const uintptr_t entry = first + i * 0x140;
            uint32_t id = 0, id2 = 0;
            const int nids = command_action_ids(base, entry, &id, &id2);
            const unsigned long long mvt = command_matcher_vt(entry, base);
            bool known = false;
            const bool ok = command_guard_ok(entry, &known);
            wchar_t name[64] = {};
            form_command_name(base, entry, name, 64);
            char utf8[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
            char idbuf[24];
            if (nids == 2)
                _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "0x%X or 0x%X", id, id2);
            else if (nids == 1)
                _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "0x%X", id);
            else
                _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "?");
            spdlog::info("[cmdtable]   [{}] matcher vt exe+0x{:X} actionId={} guard={} label='{}'", i,
                         mvt, idbuf, known ? (ok ? "allows" : "REFUSES") : "unreadable", utf8);
        }
    }

    // The Back button is NOT hooked any more. With one real screen per page the engine's own
    // cancel does exactly the right thing (close this screen, hand control to the one beneath),
    // so the action-pointer swap that used to walk our own page stack - and the GetAsyncKeyState
    // trick that tried to tell Esc from Back - are both gone. What is left of that work is
    // invoke_cancel() below, which RUNS the engine's cancel when a row has finished its job.

    // Close a screen of ours the way its own Back button does: run the CANCEL command the window
    // registered (action id 0x18). The dispatcher calls it with one argument and drops the result,
    // so calling it directly is what the player pressing Q does - and it leaves the whole
    // close/unwind path to the engine.
    // Deferred by one frame by its callers: a screen must not be torn down from inside its own
    // confirm handler.
    bool invoke_cancel(uintptr_t dlg)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uintptr_t first = 0, last = 0;
        const size_t n = count_form_commands(dlg, &first, &last);
        for (size_t i = 0; i < n && i < 32; ++i)
        {
            const uintptr_t entry = first + i * 0x140;
            uint32_t id = 0;
            if (!command_action_id(base, entry, &id) || id != kInputActionBack)
                continue;
            __try
            {
                const uintptr_t act = *reinterpret_cast<uintptr_t *>(entry + kCmdActionOff);
                if (!act)
                    return false;
                using InvokeFn = void *(void *self, void *out);
                auto fn = *reinterpret_cast<InvokeFn **>(*reinterpret_cast<uintptr_t *>(act) + 0x10);
                if (!fn)
                    return false;
                void *out = nullptr;
                fn(reinterpret_cast<void *>(act), &out);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
        return false;
    }

    // A screen asked to be closed from its own handler; done on the next tick.
    std::atomic<uintptr_t> g_close_request{0};
    // One-shot: log a screen's registered command ids the first time we see them.
    std::atomic<bool> g_cmd_ids_logged{false};

    void form_decide_detour(void *dlg)
    {
        const uintptr_t d = reinterpret_cast<uintptr_t>(dlg);
        const int level = menu_open() ? screen_level(d) : -1;
        if (level < 0)
        {
            o_form_decide(dlg);
            return;
        }
        // A parent taking input again means its child is gone; put the model back on its page
        // before the row under the cursor is resolved.
        if (static_cast<size_t>(level) + 1 < g_screens.size())
        {
            pop_above(static_cast<size_t>(level));
            resume_level(static_cast<size_t>(level));
        }
        // A screen that has only just appeared must not act on the press that opened it (see
        // kNewScreenInputGraceMs).
        const Screen &me = g_screens[static_cast<size_t>(level)];
        if (me.shown && GetTickCount64() - me.shown < kNewScreenInputGraceMs)
        {
            spdlog::info("[form] level {} ignored a confirm {} ms after appearing (the press that "
                         "opened it)", level, GetTickCount64() - me.shown);
            return;
        }
        // The pool this screen's items live in - stated here rather than assumed, because the row
        // lookup below rejects an item that belongs to any other level.
        g_form_depth = static_cast<size_t>(level) < kMaxNest ? static_cast<size_t>(level)
                                                             : kMaxNest - 1;
        size_t ix = 0;
        bool preview = false;
        if (!selected_model_index(d, &ix, &preview))
        {
            o_form_decide(dlg);
            return;
        }
        if (preview)
            return; // a right-column preview line is display-only
        {
            size_t count = 0;
            const goblin::nmenu::Row *rows = goblin::nmenu::rows(&count);
            spdlog::info("[form] decide row {} of {} (kind {}, key {}) on page {} (level {})", ix,
                         count, ix < count ? static_cast<int>(rows[ix].kind) : -1,
                         ix < count && rows[ix].ini_key ? rows[ix].ini_key : "-",
                         goblin::nmenu::current_page(), level);
        }
        // The model toggles / applies / runs the action and reports whether the rows changed. It
        // never navigates on its own here (screen-per-page mode): a row that leads somewhere asks
        // for a CHILD SCREEN, and a row that finishes an edit asks for this screen to close.
        // The native "assign a key" chain must never run for our rows.
        const bool changed = goblin::nmenu::activate(ix);
        const int32_t child = goblin::nmenu::take_child_page();
        if (child >= 0)
        {
            g_screens[static_cast<size_t>(level)].pos = read_list_pos(d);
            open_screen(child, d);
            return;
        }
        if (goblin::nmenu::take_close_request())
        {
            g_close_request.store(d, std::memory_order_release);
            return;
        }
        if (!changed)
            return;
        build_form_rows();
        refresh_form_view(d);
    }

    // Build a CommandList MenuJob for the window via the game's spec builder
    // FUN_140745ed0. Disasm truth (r2 @0x140745f61..0x140745ff2): on PC the builder
    // stages "01_070_CommandList" (gamepad form: big layout, right-side pad panel)
    // into the spec's movie block, then UNCONDITIONALLY overwrites it with
    // "02_045_PC_CommandList" (the small popup) - there is no runtime branch. The
    // float2 from FUN_140d82770 (fed by the builder's param_3) only sets the form
    // size @spec+0xa80. So: forcePad temporarily repoints the second movie-name lea
    // (rip-relative disp32 @+0x745fd6, lea end +0x745fda) at the 01_070 literal and
    // restores it right after - same-thread only (the game reaches this builder
    // solely from this UI thread's input dispatcher). w > 0 passes a custom size
    // through param_3; otherwise the game default flow (FUN_140757af0) is used.
    void *build_cmdlist_job(uintptr_t base, uintptr_t dlg, bool forcePad, float w, float h)
    {
        uint8_t inFlag = 0;
        uint64_t sizeBuf = 0;
        void *szp = &sizeBuf;
        if (w > 0.f)
        {
            float *f = reinterpret_cast<float *>(&sizeBuf);
            f[0] = w;
            f[1] = h;
        }
        else
        {
            auto p_size = reinterpret_cast<void *(*)(void *, uint64_t *)>(base + 0x757af0);
            szp = p_size(&inFlag, &sizeBuf);
        }
        uint8_t *leaDisp = reinterpret_cast<uint8_t *>(base + 0x745fd6);
        const uintptr_t leaEnd = base + 0x745fda;
        static const wchar_t kPadMovie[] = L"01_070_CommandList";
        int32_t oldDisp = 0;
        bool patched = false;
        if (forcePad)
        {
            oldDisp = *reinterpret_cast<int32_t *>(leaDisp);
            if (leaEnd + static_cast<intptr_t>(oldDisp) == base + 0x2A93B38 &&
                std::memcmp(reinterpret_cast<const void *>(base + 0x2A93B10), kPadMovie,
                            sizeof(kPadMovie)) == 0)
            {
                const int32_t newDisp = static_cast<int32_t>(
                    static_cast<intptr_t>(base + 0x2A93B10) - static_cast<intptr_t>(leaEnd));
                DWORD prot = 0;
                if (VirtualProtect(leaDisp, 4, PAGE_EXECUTE_READWRITE, &prot))
                {
                    *reinterpret_cast<int32_t *>(leaDisp) = newDisp;
                    VirtualProtect(leaDisp, 4, prot, &prot);
                    patched = true;
                }
            }
            if (!patched)
            {
                spdlog::warn("[cmdlist] force-pad movie patch failed (layout mismatch)");
                return nullptr;
            }
        }
        void *job = nullptr;
        auto p_build = reinterpret_cast<void (*)(void *, void **, void *)>(base + 0x745ed0);
        p_build(reinterpret_cast<void *>(dlg), &job, szp);
        if (patched)
        {
            DWORD prot = 0;
            if (VirtualProtect(leaDisp, 4, PAGE_EXECUTE_READWRITE, &prot))
            {
                *reinterpret_cast<int32_t *>(leaDisp) = oldDisp;
                VirtualProtect(leaDisp, 4, prot, &prot);
            }
        }
        return job;
    }

    // Release one DLRefCountObj reference (Unref + vt[0] dtor at rc==1), the pattern
    // used across the job/holder machinery.
    void release_job_ref(uintptr_t base, void *obj)
    {
        if (!obj)
            return;
        auto p_unref = reinterpret_cast<int (*)(void *)>(base + 0x1EBA200);
        if (p_unref(reinterpret_cast<uint8_t *>(obj) + 8) == 1)
            (*reinterpret_cast<void (**)(void *)>(*reinterpret_cast<void **>(obj)))(obj);
    }

    // The live CS::WorldMapDialog, derived from the CS::WorldMapArea the hover hook
    // captures: wmd_update passes r8 = dialogBase + 0x27D8, so the dialog sits 0x27D8
    // below the area (same relation that gave us the map-layer field). Verified by its
    // vtable before use. WorldMapDialog shares the MenuWindow base with our settings
    // dialog (vt+0x28/0x30/0x38/0x40 are the same MenuWindow slots), so it has the same
    // child-job holder at +0xA28 - we can hang the keybinding form straight off the MAP
    // and skip the intermediate F11 menu layer.
    // Does this look like a MenuWindow we can hang a child screen off? Checked rather than
    // assumed: the active menu can be any dialog, and writing a job into a slot that is not a
    // holder would corrupt it. A heap-looking vtable plus an EMPTY child holder is enough - the
    // holder is a DLRefPtr, so an in-range non-null value there means the slot is already taken.
    // POD-only: read the two things the check looks at, so a rejection can be explained.
    bool host_fields(uintptr_t win, uintptr_t *vt, uint64_t *holder)
    {
        __try
        {
            *vt = *reinterpret_cast<uintptr_t *>(win);
            *holder = *reinterpret_cast<uint64_t *>(win + 0xA28);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool usable_host(uintptr_t win)
    {
        if (!v3_heap_ptr(win))
            return false;
        uintptr_t vt = 0;
        uint64_t holder = 0;
        const bool read = host_fields(win, &vt, &holder);
        const bool vt_ok = read && v3_heap_ptr(vt);
        const bool holder_free = read && holder == 0;
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            spdlog::info("[form] host check 0x{:X}: read={} vt=0x{:X} (rva 0x{:X}) holder=0x{:X} "
                         "-> vt_ok={} holder_free={}",
                         win, read, vt, vt > base ? vt - base : 0, holder, vt_ok, holder_free);
        }
        return vt_ok && holder_free;
    }

    uintptr_t map_menu_window()
    {
        void *area = goblin::maphover::map_dialog();
        if (!area)
            return 0;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t cand = reinterpret_cast<uintptr_t>(area) - 0x27D8;
        uintptr_t vt = 0;
        __try
        {
            vt = *reinterpret_cast<uintptr_t *>(cand);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return (vt == base + 0x2B2D7D8) ? cand : 0;
    }


    // Set while the form hangs off the MAP (no F11 menu in the stack): nothing steps our
    // own job then, so the per-frame tick must live-apply and the ini must be saved when
    // the form closes (its child-job holder empties).
    std::atomic<uintptr_t> g_form_host{0};

    // F8 (dev): open the game's real keybinding form (movie 02_160_KeyConfiguration,
    // dialog CS::KeyConfigDialog) as a CHILD WINDOW, with our rows swapped in.
    // Host preference = the MAP itself (WorldMapDialog shares the MenuWindow base, so it
    // has the same child-job holder @+0xA28): that keeps the stack at map + form instead
    // of map + F11 menu + form. Falls back to our F11 menu when the map is not up.
    // Build path mirrors the game's own opener FUN_14094fd40: FUN_1408078f0(outRef,
    // win+0x50, modeByte) -> ref conversions FUN_1407a7e30 / FUN_1407a7b60 -> store in
    // the holder (FUN_1407a9460) with the AddRef/Unref dance, releasing every temp slot.
    // (The native opener FUN_140747450 is NOT usable: its KeyGuide input gate can only
    // pass from the game's own per-tick input dispatch - see cmdlist_gate_re3.txt.)
    // Two ways a window can host another screen (both stepped by the host's own tick):
    //   holder +0xA28 = CHILD, drawn OVER the host (what we use over the live map)
    //   holder +0x10  = SEQUENCE, the screen REPLACES the host and Back returns to it,
    //                   exactly as the game's settings page opens Key Assignments
    //                   (FUN_14094fd40 / FUN_1409411a0 both store into +0x10)
    // Store call differs too: +0xA28 takes FUN_1407a9460(holder, &old, &ref), +0x10 takes
    // FUN_1407a9250(holder, ref).
    // ── Second host: 02_042_PC_GraphicSetting, the screen with REAL widgets ──────────
    // The key-binding form gives us an unlimited scrolling LIST, but its rows are two text
    // fields - a slider or a combo box has to be faked. The graphics screen is the opposite:
    // its page clip carries 15 row slots plus a ScrollBarV, and its rows ARE the engine's own
    // Slider / ComboBox / Button / TextInput widgets.
    //
    // Opening it is the same trick as the key-binding form, with two differences found by
    // decompiling both openers side by side:
    //   * FUN_1408079E0(out, owner, flag) builds an OptionSettingDialog job through the
    //     generic builder FUN_140808630 (movie block {8, 1, L"02_042_PC_GraphicSetting"},
    //     factory FUN_140807B10), where the key-binding form builds a MenuWindow job through
    //     FUN_1407ACB00. `owner` is the SAME host + 0x50 (read off the game's own call site
    //     at 0x14094FB02).
    //   * its tail is FUN_1407418D0 = a DLRefPtr assign, so `out` already holds an owned
    //     reference - the two fd40 conversions the other opener needs are not wanted here.
    // The game itself puts this screen in the SEQUENCE slot (it replaces the settings page);
    // over the live map we want it drawn on top, so it goes in the child slot instead.
    //
    // Its rows come from populate FUN_14095A5D0, and the page it populates is built by the
    // very same page factory (FUN_14095EB90) our settings tab already borrows - with clip
    // index 0xD = "GraphicOption" instead of 3 = "CameraSetting", which is exactly why that
    // one caps out at 12 rows with no scrollbar and this one does not.
    using PopulateGraphicFn = void(void *page, void *ctx);
    PopulateGraphicFn *o_populate_graphic = nullptr;
    // Defined with the settings-tab code further down; both screens are populated the same way.
    void build_our_rows(void *page);
    std::atomic<int> g_pad_rows_to{0}; // F6 scroll test: pad our page to this many rows (0 = off)
    std::atomic<bool> g_graphic_armed{false};
    std::atomic<bool> g_graphic_populated{false};
    std::atomic<uintptr_t> g_graphic_host{0};



    // The populate only runs while the page is built, so disarm as soon as the screen is up -
    // and clear the host once the holder empties, so a second press can open it again.

    // ── the byte that decides who owns the input ─────────────────────────────────────
    // Both screen openers call the same core FUN_1407ACB00(out, owner, desc, factory) and differ
    // only in the DESCRIPTOR they hand it:
    //     F11's settings (input works):  { u32 8, u8 2, L"02_040_OptionSetting" }
    //     our form       (input dead):   { u32 8, u8 1, L"02_160_KeyConfiguration" }
    // and that byte is not a movie version - FUN_1408087E0 sets it to 1 only for the trial movie.
    // It is written by a single instruction with an immediate:
    //     0x140807967  MOV byte ptr [RSP+0x34], 0x1     -> immediate at RVA 0x80796B
    // so the experiment is one byte, flipped around our own call and put straight back. The same
    // trick the CommandList work already used on a rip-relative operand.
    constexpr uintptr_t kFormDescKindImm = 0x80796B;

    // Returns the previous value, or 0 if the patch could not be applied.
    uint8_t patch_form_desc_kind(uint8_t want)
    {
        const uintptr_t at = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) +
                             kFormDescKindImm;
        DWORD old_prot = 0;
        if (!VirtualProtect(reinterpret_cast<void *>(at), 1, PAGE_EXECUTE_READWRITE, &old_prot))
            return 0;
        uint8_t prev = 0;
        __try
        {
            prev = *reinterpret_cast<uint8_t *>(at);
            *reinterpret_cast<uint8_t *>(at) = want;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            prev = 0;
        }
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<void *>(at), 1, old_prot, &tmp);
        return prev;
    }

    // ── OPENING A SCREEN ─────────────────────────────────────────────────────────────
    // The job goes into the host window's SEQUENCE slot (+0x10) - see the note on the screen
    // stack for why that slot and no other. modeByte (dlg+0x1260) does more than swap glyphs:
    // the ctor runs setFrameNumber(KeySetting/BG + 0x18, modeByte + 1), so
    //   mode 0 = NARROW 941px panel (leaves room for the gamepad art, which the right column
    //            then lands on top of), device pictures shown, two scrollbars
    //   mode 1 = WIDE 1298px panel, device pictures hidden, single scrollbar moved right
    // A settings list wants mode 1 (recon_02_160_structure.md), and its per-frame device-panel
    // refresh is mode-0 only, so mode 1 also keeps our rows from being second-guessed.
    constexpr uint8_t kFormMode = 1;

    // ── is this thing a WINDOW we may hand a job to? ─────────────────────────────────
    // Not every "menu" is. *(CSMenuMan+0x80) - what we had been calling the active menu - is not
    // of this family at all: in game its vtable slots read back as string data (measured
    // 0x2AB7958: +0x38 = 65704F6D21435F3A), and storing a job into its +0x10 faulted, which is
    // the "[form] SEH opening the screen" that kept F8 from opening over gameplay. Worse, the
    // failed store left that offset reading non-zero, so every later press answered "already
    // holds a screen".
    // The family marker is the shared slot-0 destructor 0x7342B0 (138 vtables carry it) plus the
    // per-window command notifier 0x745BD0 at +0x38 - both present on WorldMapDialog and
    // KeyConfigDialog, the two windows we know host jobs correctly.
    bool window_like(uintptr_t win)
    {
        if (!v3_heap_ptr(win))
            return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(win);
            if (!v3_heap_ptr(vt) && vt < base)
                return false;
            if (*reinterpret_cast<uintptr_t *>(vt) != base + 0x7342B0)
                return false;
            if (*reinterpret_cast<uintptr_t *>(vt + 0x38) != base + 0x745BD0)
                return false;
            // The sequence holder must read as a DLRefPtr: empty, or something on the heap.
            const uint64_t held = *reinterpret_cast<uint64_t *>(win + 0x10);
            return held == 0 || v3_heap_ptr(static_cast<uintptr_t>(held));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Read-only diagnostic, logged once per open that finds no window: which pointers inside
    // CSMenuMan DO look like windows. The answer is what a root screen can be hosted on when the
    // map is closed, and it is cheaper to read it out of the live game than to guess.
    void log_window_candidates(uintptr_t menuman)
    {
        if (!menuman)
            return;
        int found = 0;
        std::string list;
        for (size_t off = 0; off < 0x400; off += 8)
        {
            uint64_t raw = 0;
            if (!v3_read64(menuman + off, raw)) // POD reader: SEH cannot live in this function
                break;
            const uintptr_t p = static_cast<uintptr_t>(raw);
            if (!window_like(p))
                continue;
            char buf[64];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, " +0x%zX=0x%llX", off,
                        static_cast<unsigned long long>(p));
            list += buf;
            if (++found >= 12)
                break;
        }
        spdlog::info("[form] window-like pointers in CSMenuMan 0x{:X}:{}", menuman,
                     found ? list : std::string(" none"));
    }

    void open_screen(int32_t page, uintptr_t nest_parent)
    {
        if (!nest_parent && menu_open())
        {
            spdlog::info("[form] ignored: the menu is already open ({} screen(s))",
                         g_screens.size());
            return;
        }
        if (g_screens.size() >= kMaxNest)
        {
            spdlog::info("[form] ignored: {} screens deep already", g_screens.size());
            return;
        }
        if (Screen *pending = top_screen())
            if (!pending->dlg)
            {
                spdlog::info("[form] ignored: the screen opened last has not come up yet");
                return;
            }

        // Where does it hang? A sub-page always goes into the page above it; a root screen onto
        // the window that owns the screen the player is looking at.
        uintptr_t host = nest_parent;
        const char *what = "the page above it";
        if (!host)
        {
            host = map_menu_window();
            if (host)
                what = "the map";
        }
        if (!host)
        {
            host = g_settings_dialog.load(std::memory_order_acquire);
            if (host)
                what = "our settings menu";
        }
        if (host && !window_like(host))
        {
            spdlog::info("[form] host 0x{:X} ({}) is not a window - not storing a job in it", host,
                         what);
            host = 0;
        }
        // TRIED AND REVERTED 2026-07-28: hanging our screen off the HUD menu's +0xA28 holder (the
        // structural theory - we displace the HUD screen instead of parenting to it). The holder read as
        // empty and accepted the job, but the screen never came up over gameplay, so that field is not a
        // child holder on this object; window_like() was right to reject it. The HUD defect therefore is
        // NOT about which host we choose, and the next step is to find the engine's own HUD-visibility
        // code rather than to keep trying placements.
        // No window - plain gameplay. PUSH the job onto the active menu instead of storing it in a
        // slot (that needs descriptor byte 2 to receive input at all). window_like() is NOT
        // required here, and the crash that looked like it proved otherwise was ours: the dialog's
        // update hook had the wrong arity, so the game read a byte through a register we had
        // clobbered. Pushing onto this object is what worked before, on this very vtable
        // (exe+0x2AB7958 - it is a menu, not a window, and only the SLOT paths need a window).
        bool pushed = false;
        if (!host && p_refmove && p_push_job)
        {
            host = g_active_menu.load(std::memory_order_acquire);
            if (host)
            {
                pushed = true;
                what = "the active menu (pushed)";
                // Capture the HUD mode BEFORE we disturb anything; hud_mode_restore() puts it back
                // once our screens are gone. See the note on hud_mode_capture.
                hud_mode_capture();
                // Over gameplay this menu IS the HUD menu; snapshot it so the close-side compare can
                // name whatever keeps the HUD hidden afterwards.
                hud_snapshot_take(host);
            }
        }
        if (!host)
        {
            spdlog::info("[form] ignored: there is nothing to host the screen");
            log_window_candidates(g_menuman.load(std::memory_order_acquire));
            return;
        }
        // Is the slot free? Asked with the engine's OWN test - the same one its input gate uses,
        // so "the host holds a screen" and "the host is not reading input" can never disagree.
        // A pushed job is in no slot, so there is nothing to ask.
        if (!pushed && !seq_slot_empty(host))
        {
            spdlog::info("[form] ignored: host 0x{:X} already holds a screen in its sequence slot",
                         host);
            return;
        }
        spdlog::info("[form] opening page {} on {} (0x{:X})", page, what, host);

        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        // From here until the screen is up, a load of this movie is OURS: the interception
        // rebuilds it and leaves every other load (the player's own key-binding screen) alone.
        goblin::own_movie::arm();
        g_own_icon_state.store(0, std::memory_order_release);
        g_icon_diag_done.store(false, std::memory_order_release);
        g_icon_host_logged.store(false, std::memory_order_release);
        // The model serves one page per screen from here on and never navigates by itself.
        goblin::nmenu::set_nested(true);

        Screen s{};
        s.page = page;
        s.host = host;
        s.opened = GetTickCount64();
        g_screens.push_back(s);
        // The root open takes the baseline for the live-apply diff.
        if (g_screens.size() == 1)
            menu_cfg_snapshot_take();

        bool ok = false;
        __try
        {
            void *slotA = nullptr, *slotB = nullptr, *slotC = nullptr;
            auto p_keycfg =
                reinterpret_cast<void *(*)(void **, void *, uint8_t)>(base + 0x8078f0);
            auto p_conv1 = reinterpret_cast<void *(*)(void *, void **)>(base + 0x7A7E30);
            auto p_conv2 = reinterpret_cast<void *(*)(void *, void **)>(base + 0x7A7B60);
            // OWNER: win+0x50, what the game's own key-binding opener passes; the pushed path
            // uses the menu's own +0x10 the way F11's settings screen does.
            void *owner = reinterpret_cast<void *>(host + (pushed ? 0x10 : 0x50));
            // THE byte that decides who owns the input, but only for a pushed screen: parked in
            // a menu's job stack our screen rendered and received nothing until the descriptor
            // said 2 (what 02_040 uses). A screen in a window's sequence slot takes input with
            // the 1 the game itself uses for this movie.
            const uint8_t prev_kind = pushed ? patch_form_desc_kind(2) : 0;
            void *r = p_keycfg(&slotA, owner, kFormMode);
            if (prev_kind)
                patch_form_desc_kind(prev_kind); // restore at once - one call, one patch
            r = p_conv1(r, &slotB);
            r = p_conv2(r, &slotC);
            if (!slotC)
            {
                spdlog::warn("[form] no job built (slotA=0x{:X} slotB=0x{:X})",
                             reinterpret_cast<uintptr_t>(slotA),
                             reinterpret_cast<uintptr_t>(slotB));
                release_job_ref(base, slotB);
                release_job_ref(base, slotA);
            }
            else
            {
                // The sequence slot MOVES the reference out of the DLRefPtr it is handed (the
                // conversion chain returns the address of the last slot), so slotC reads back as
                // 0 afterwards - which is also why the release below is a no-op rather than a
                // double free. Record the job BEFORE the store, or the log shows 0x0.
                auto p_addref = reinterpret_cast<void (*)(void *)>(base + 0x1EBA1C0);
                g_screens.back().job = reinterpret_cast<uintptr_t>(slotC);
                if (pushed)
                {
                    void *out = nullptr, *ctx = nullptr, *jobCopy = nullptr;
                    uint8_t tmp[64] = {};
                    void *job = p_refmove(r, &jobCopy);
                    p_push_job(reinterpret_cast<void *>(host), &out, &ctx, job, tmp);
                    // Third sample of the pair taken by hud_snapshot_take/compare. The push targets
                    // the +0xD0 vector, so if our job is in +0x120 by the very next instruction the
                    // engine moved it there synchronously; if +0xD0 holds it here and +0x120 gets it
                    // later, the move happens on a menu tick. Both readings change where the missing
                    // cleanup belongs, and one line of log separates them.
                    if (goblin::config::debugLogging)
                    {
                        spdlog::info("[jobstack] our job is 0x{:X} (screen 0x{:X}, pushed onto 0x{:X})",
                                     reinterpret_cast<uintptr_t>(job),
                                     reinterpret_cast<uintptr_t>(slotC), host);
                        log_job_stack(host, "immediately after our push");
                    }
                }
                else
                {
                    p_addref(reinterpret_cast<uint8_t *>(slotC) + 8); // see the note above
                    auto p_seq = reinterpret_cast<void (*)(void *, void *)>(base + 0x7A9250);
                    p_seq(reinterpret_cast<void *>(host + 0x10), r);
                }
                release_job_ref(base, slotC);
                release_job_ref(base, slotB);
                release_job_ref(base, slotA);
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] SEH opening the screen");
        }
        if (!ok)
        {
            g_screens.pop_back();
            if (g_screens.empty())
            {
                goblin::nmenu::set_nested(false);
                goblin::own_movie::disarm();
            }
            return;
        }
        g_form_title_pending.store(true, std::memory_order_release);
        spdlog::info("[form] level {} job 0x{:X} {} 0x{:X}", g_screens.size() - 1,
                     g_screens.back().job,
                     pushed ? "pushed onto" : "placed in the sequence slot of", host);
    }

    // Bring a level back to life after the screen above it went away. THE MODEL ONLY: point it
    // at this screen's page and refill this level's pool, and touch nothing that belongs to the
    // engine.
    // The first version also ran the game's own rebuild+refresh on the resuming dialog (and wrote
    // its grid fields). In game the parent then died about a second later - its update stopped,
    // its memory read back as garbage - and the whole menu tore down. The child's teardown is not
    // finished at the moment its slot reports empty, so refreshing the parent right then reaches
    // into a window that is still unwinding. It is not needed either: the engine refreshes the
    // resuming screen by itself (measured - a row build for the parent arrives ~1 s later), and
    // that build lands in our hook with the right page and pool already set.
    void resume_level(size_t level)
    {
        if (level >= g_screens.size())
            return;
        Screen &s = g_screens[level];
        if (!s.dlg)
            return;
        g_form_depth = level < kMaxNest ? level : kMaxNest - 1;
        g_form_dialog.store(s.dlg, std::memory_order_release);
        g_form_update_watch.store(s.dlg, std::memory_order_release);
        goblin::nmenu::set_page(s.page);
        build_form_rows();
        // Normally the page is already back by now - reveal_page_under_closing_top() brings it
        // back as soon as the screen over it starts closing, so there is no frame with neither of
        // them drawn. This is the fallback for the paths that skip that (a screen that died
        // without closing, a level dropped by the heartbeat).
        if (s.hidden)
        {
            s.hidden = false;
            set_screen_visible(s.dlg, true);
        }
        spdlog::info("[form] level {} resumed (page {}) - model only, the engine redraws it",
                     level, s.page);
    }

    void close_screen(uintptr_t dlg)
    {
        if (!invoke_cancel(dlg))
        {
            spdlog::warn("[form] screen 0x{:X} has no cancel command to close it with", dlg);
            return;
        }
        // NO HUD restore here, deliberately. invoke_cancel() reports success as soon as it FINDS and
        // calls the Back command's action, but the engine only ever runs a command when the command's
        // own matcher (+0x38) matches the input AND its guard (+0x138) allows it - both of which we
        // skip. Measured 2026-07-28: ESC logged a successful cancel, the screen stayed up, and the
        // only visible effect was the HUD coming back OVER a still-open menu. So the restore stays on
        // job_finished(), which cannot be true unless the screen really is going away.
    }
    // Per-frame upkeep for a MAP-hosted form: live-apply toggles, and when the form is
    // gone (the map's child-job holder emptied) persist the ini and disarm the row swap.
    // ── "press a key" capture for the rebind page ────────────────────────────────────
    // The screen's own input goes through the window's command vector, which knows nothing
    // about arbitrary keys, so the capture is done here - the same way the overlay does it.
    // The page is entered BY a key press, so nothing is read until every key is up again;
    // otherwise the confirm key would instantly rebind the entry to itself.
    bool g_rebind_armed = false;

    void poll_rebind()
    {
        if (!goblin::nmenu::rebind_pending())
        {
            g_rebind_armed = false;
            return;
        }
        bool any_down = false;
        int pressed = 0;
        for (int vk = 0x07; vk <= 0xFE; ++vk)
        {
            if (!(GetAsyncKeyState(vk) & 0x8000))
                continue;
            any_down = true;
            if (!pressed)
                pressed = vk;
        }
        if (!g_rebind_armed)
        {
            if (!any_down)
                g_rebind_armed = true; // everything released - now we may listen
            return;
        }
        if (!pressed)
            return;
        g_rebind_armed = false;
        // Escape means "leave it alone", which rebind_apply spells as 0.
        goblin::nmenu::rebind_apply(pressed == VK_ESCAPE ? 0u : static_cast<uint32_t>(pressed));
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (dlg)
        {
            build_form_rows();
            refresh_form_view(dlg);
        }
    }

    // ESC does not reach our screen's Back command: the host is the key-binding screen, whose own
    // command table registers action 0x18 for Q (and for the pad's circle), not for Escape, so the
    // engine simply has nothing bound to it there. The screen therefore ignored ESC entirely. Read the
    // key ourselves - the same way poll_rebind() does, because the screen consumes routed input but
    // GetAsyncKeyState reports the physical key - and raise the normal close request, so the close
    // still runs through the engine's own cancel path rather than anything of ours.
    // Guards: not while the rebind page is waiting for a key (there ESC means "leave the binding
    // alone" and poll_rebind owns it), and only on the down-edge so holding ESC cannot close a
    // parent screen right after a child.
    bool g_esc_was_down = false;

    void poll_escape_close(uintptr_t dlg)
    {
        // RETIRED 2026-07-28, kept for the reasoning. Raising a close request on ESC cannot work:
        // close_screen -> invoke_cancel calls the Back command's ACTION directly, and the engine only
        // ever runs an action when that command's matcher AND guard agree (see section 2a of
        // docs/research_native_menu_screens.md), so the call reported success while the screen stayed
        // up - and the only visible effect was the HUD returning over a still-open menu.
        // Measured afterwards, and this is the reason it is off rather than fixed: ESC produces NONE
        // of the nine actions this screen listens for, and in plain gameplay ESC does not reach the
        // action predicate at all - the system menu it opens is handled by a path OUTSIDE the per-screen
        // command tables. So ESC-close is not a defect of ours to repair; it is engine integration we
        // do not have yet, and it belongs with the native-registration work.
        return;
        const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool edge = down && !g_esc_was_down;
        g_esc_was_down = down;
        if (!edge || !dlg || goblin::nmenu::rebind_pending())
            return;
        spdlog::info("[form] Escape pressed - closing screen 0x{:X} through its own cancel", dlg);
        g_close_request.store(dlg, std::memory_order_release);
    }

    // FUN_1407A9230(win + 0x10) - the game's OWN "is the sequence slot free?" test, the one its
    // input dispatcher gates a window's commands on (it stops feeding a window input while the
    // slot holds a screen). That makes it the exact liveness test for a screen we put THERE:
    // non-empty = the child is up, empty = it is gone.
    bool seq_slot_empty(uintptr_t win)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            return reinterpret_cast<char (*)(void *)>(base + 0x7A9230)(
                       reinterpret_cast<void *>(win + 0x10)) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Everything is down: persist, forget the screens, and let the next F8 start clean.
    void menu_torn_down()
    {
        hud_mode_restore("teardown (fallback)"); // in case no normal close was seen
        g_screens.clear();
        hud_snapshot_compare(); // our screens can also leave here, not only through the per-level close
        g_form_depth = 0;
        g_form_dialog.store(0, std::memory_order_release);
        g_form_update_watch.store(0, std::memory_order_release);
        g_form_host.store(0, std::memory_order_release);
        g_cmd_ids_logged.store(false, std::memory_order_release);
        g_close_request.store(0, std::memory_order_release);
        for (std::vector<FormItem> &p : g_form_pools)
            p.clear();
        goblin::nmenu::set_nested(false);
        goblin::own_movie::disarm();
        if (goblin::nmenu::dirty())
        {
            goblin::nmenu::clear_dirty();
            g_menu_cfg_dirty = true; // fold into the single save below
        }
        if (g_menu_cfg_dirty)
        {
            g_menu_cfg_dirty = false;
            goblin::save_config(goblin::g_ini_path);
            spdlog::info("[form] menu closed - settings saved to ini");
        }
        else
            spdlog::info("[form] menu closed (no changes)");
    }

    // ── WHO IS STILL THERE ───────────────────────────────────────────────────────────
    // Two signals, and neither one reads memory that may already be freed:
    //   * HEARTBEAT - a dialog's own per-frame update (its vt+0x10) runs for every screen that
    //     exists, including one sitting under a child (the input gate stops a parent's COMMANDS,
    //     not its update). A screen whose heartbeat stopped is gone. This is the only signal used
    //     for the bottom of the stack, and it needs no host to be inspected at all.
    //   * PARENT'S SEQUENCE SLOT - read only on a parent whose heartbeat is fresh THIS frame, so
    //     the object is known to be alive. Empty slot = the child above it has closed, known one
    //     frame after it happens instead of after a timeout.
    // Inspecting the dialog's vtable is deliberately NOT among them: that read reported "closed"
    // on a live screen and let presses stack layers.
    constexpr uint64_t kBeatStaleMs = 600;
    // How long a screen that has been asked for may take to come up (async movie load).
    constexpr uint64_t kOpenGraceMs = 3000;

    bool beat_fresh(const Screen &s, uint64_t now)
    {
        return s.beat != 0 && now - s.beat < kBeatStaleMs;
    }

    // FUN_1407A9200(&win[0x1E8]) - the engine's own "is this window's job finished?" test, the
    // first of the three gates its input dispatcher checks (0x1407455D5..0x1407455EB). It goes
    // true when the screen starts CLOSING, which is earlier than its slot emptying and earlier
    // than its dialog dying - so it is the moment the page underneath must come back if there is
    // to be no frame with neither of them drawn.
    bool job_finished(uintptr_t win)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            uint64_t held = *reinterpret_cast<uint64_t *>(win + 0x1E8);
            return reinterpret_cast<char (*)(void *)>(base + 0x7A9200)(&held) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Bring the page under the top screen back while that screen is still fading out.
    void reveal_page_under_closing_top()
    {
        if (g_screens.size() < 2)
            return;
        Screen &top = g_screens.back();
        Screen &below = g_screens[g_screens.size() - 2];
        if (!top.dlg || !below.dlg || !below.hidden)
            return;
        if (!job_finished(top.dlg))
            return;
        below.hidden = false;
        set_screen_visible(below.dlg, true);
        spdlog::info("[form] page 0x{:X} back on screen while the screen over it closes",
                     below.dlg);
    }

    // The HUD has to start coming back WITH the close animation, not after it. The engine does that
    // for its own screens; we used to wait for prune_screens() to notice the missing heartbeat, which
    // is a ~600 ms timeout ("level 0 gone (no update for 609 ms)") and read as a visible lag. So use
    // the same event the page-reveal above uses: job_finished() is the engine's own "this screen's job
    // is done" test, true while the screen is still fading out. Restoring here is safe even if the
    // screen somehow lingers, because hud_mode_restore() only writes when the mode is still 0 and
    // clears its captured value, so the later call from menu_torn_down() becomes a no-op.
    // How late is the restore, in milliseconds, measured rather than guessed? The close is started by
    // a key press we do not hook (the engine routes it to the screen), but GetAsyncKeyState reports the
    // physical key regardless of routing, so stamping the down-edge of the two keys that close a screen
    // is enough to turn "sometimes a small delay" into a number. Debug-logging only; it changes nothing.
    uint64_t g_back_key_ms = 0;
    bool g_back_key_was_down = false;

    void note_back_key()
    {
        if (!goblin::config::debugLogging)
            return;
        const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 ||
                          (GetAsyncKeyState('Q') & 0x8000) != 0;
        if (down && !g_back_key_was_down)
            g_back_key_ms = GetTickCount64();
        g_back_key_was_down = down;
    }

    void restore_hud_when_close_starts()
    {
        if (g_screens.size() != 1 || !g_hud_mode_at_push)
            return; // only the root screen going away ends our over-gameplay session
        const Screen &top = g_screens.back();
        if (!top.dlg || !job_finished(top.dlg))
            return;
        if (goblin::config::debugLogging && g_back_key_ms)
            spdlog::info("[hud] the close signal arrived {} ms after the back key went down",
                         GetTickCount64() - g_back_key_ms);
        hud_mode_restore("the close animation");
    }

    void prune_screens()
    {
        const uint64_t now = GetTickCount64();
        if (g_screens.empty())
            return;
        note_back_key();
        restore_hud_when_close_starts();
        // Top down: a screen with no heartbeat is gone. One that has not come up yet has no
        // heartbeat either, so it is judged by its open grace instead.
        while (!g_screens.empty())
        {
            const Screen &top = g_screens.back();
            if (!top.dlg)
            {
                if (now - top.opened < kOpenGraceMs)
                    break;
                spdlog::warn("[form] level {} never came up - dropped", g_screens.size() - 1);
            }
            else
            {
                if (beat_fresh(top, now))
                    break;
                spdlog::info("[form] level {} gone (no update for {} ms)", g_screens.size() - 1,
                             now - top.beat);
            }
            g_screens.pop_back();
            if (!g_screens.empty())
                resume_level(g_screens.size() - 1);
        }
        if (g_screens.empty())
        {
            hud_snapshot_compare(); // the usual exit for an over-gameplay screen
            menu_torn_down();
            return;
        }
        // EXPERIMENT (2026-07-28): shift the screen right to see whether the key-binding host's
        // left-half layout can be centred. Applied HERE, per screen, because the two earlier attempts
        // sat in code paths that never ran for a lone screen and logged nothing. Set to 0 to disable.
        // CENTRING IS NOT DONE AT RUNTIME - see goblin_own_movie.cpp. Three runtime attempts failed and
        // each taught something: the delta must be added to the current position (SetPos is absolute);
        // proxy -> value+0x28 is an AS object handle, not a DisplayObject (calling vt+0x10/+0x18 on it
        // crashed); and even with the authored coordinates, the movie's own timeline RE-PLACES the section
        // a few frames in and wins, while re-applying every tick reaches a dying screen during teardown
        // and crashed on close. The position the engine keeps restoring is the one authored in the MOVIE,
        // so that is where it has to be changed - which costs no runtime writes at all.

        // A live parent whose sequence slot has emptied has lost its child - the fast path, and
        // the reason a return does not wait for a timeout.
        for (size_t i = 1; i < g_screens.size(); ++i)
        {
            const Screen &parent = g_screens[i - 1];
            if (!parent.dlg || !beat_fresh(parent, now))
                break; // cannot ask a parent we are not sure about
            if (!g_screens[i].dlg && now - g_screens[i].opened < kOpenGraceMs)
                break; // its child is still coming up
            if (!seq_slot_empty(parent.dlg))
                continue; // the parent still holds it
            spdlog::info("[form] level {} closed (page {}), {} level(s) dropped", i,
                         g_screens[i].page, g_screens.size() - i);
            g_screens.resize(i);
            resume_level(i - 1);
            break;
        }
    }

    // Per-frame upkeep for the whole menu, whatever it is hosted on.
    void tick_native_menu()
    {
        // A screen that asked to be closed from inside its own confirm handler: doing it here
        // means the handler has long returned before the dialog is torn down.
        const uintptr_t want_close = g_close_request.exchange(0, std::memory_order_acq_rel);
        if (want_close && screen_level(want_close) >= 0)
            close_screen(want_close);
        // Before pruning: a screen that has begun closing means the page under it should already
        // be visible, so the fade-out plays over it instead of over nothing.
        reveal_page_under_closing_top();
        prune_screens();
        if (!menu_open())
            return;
        const Screen &top = g_screens.back();
        if (!top.dlg)
            return; // still coming up
        menu_cfg_apply_if_changed();
        paint_help_line();
        poll_rebind();
        arm_action_log_on_escape();
        poll_escape_close(top.dlg);
        // ── the strips, EVERY frame ──────────────────────────────────────────────────
        // The SDK says why they cannot be written once: DisplayList::MoveDisplayObject
        // (GFx_DisplayList.cpp:363) re-applies the tag's MATRIX whenever the timeline places an
        // object again, and a script-set transform only survives if the object rejects anim
        // moves - which the movie-wide continueAnimation flag undoes for it
        // (GFx_DisplayObject.cpp:1175). Position and scale are both matrix, so both are fair game
        // for the engine; the playhead is NOT in that list, which is the long-term fix (one frame
        // per icon instead of a shifted strip).
        // NOT from here: our tick runs before the timeline advance, so the engine's re-placement
        // of the authored matrix always landed after it. The repaint moved to the tail of
        // form_update_detour, the one point in the frame that is later than the advance.
        if (goblin::config::debugLogging)
        {
            // Scroll telemetry: a top that stops at (totalRows - visible) is a clamp, a top that
            // keeps climbing is the data mapping running out.
            static int32_t s_last_top = -1;
            static uint32_t s_last_cur = 0xFFFFFFFFu;
            int32_t t = 0;
            uint32_t cur = 0, cnt = 0;
            __try
            {
                const uintptr_t grid = top.dlg + 0xA38;
                t = *reinterpret_cast<int32_t *>(grid + 0x348);
                cur = *reinterpret_cast<uint32_t *>(grid + 0xD4);
                cnt = *reinterpret_cast<uint32_t *>(grid + 0xD0);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            if (t != s_last_top || cur != s_last_cur)
            {
                s_last_top = t;
                s_last_cur = cur;
                spdlog::info("[form] scroll: top={} cursor={} count={} (level {})", t, cur, cnt,
                             g_screens.size() - 1);
            }
        }
    }
    void menu_update_detour(void *menuman, void *dt)
    {
        // Arm the action log from HERE too, not only from the form upkeep: the control measurement is
        // ESC pressed in PLAIN GAMEPLAY, where no screen of ours exists and the upkeep does not run.
        // Whatever id answers YES there is the action ESC really produces - and the first run proved
        // it is none of the nine our screen listens for.
        arm_action_log_on_escape();
        // F11 (dev-only): open the native settings menu on this UI thread. GATED to
        // IN-GAME with the map open - the settings dialog reads gameplay-state
        // singletons that are NULL at the title screen (opening it from the main menu
        // crashes in the async job). The open map also guarantees a valid active menu
        // to push onto.
        // Cache the persistent base/root menu: while in-game with NO full-screen menu
        // up (map closed), *(CSMenuMan+0x80) is the base HUD menu. Settings pushed onto
        // THIS stacks as an overlay (so it can later overlay the map instead of replacing it).
        // What *(CSMenuMan+0x80) is depends on the state, and it is NOT always a window: in game
        // its vtable slots read back as string data, so a job may only be PUSHED onto it, never
        // stored in a slot of it (window_like() is the gate for that).
        g_menuman.store(reinterpret_cast<uintptr_t>(menuman), std::memory_order_release);
        hud_watch_sample(menuman); // armed right after our screen closes; prints only on change
        // ONE-SHOT: locate the STATIC slot that holds this singleton. With its RVA we can x-ref the code
        // that touches CSMenuMan+0x90.. offline and find the function the game itself uses to clear those
        // entries (writing them ourselves does not restore the HUD and races the menu machinery). Scans
        // the module's own image for a qword equal to the live pointer.
        if (goblin::config::debugLogging && menuman)
        {
            static std::atomic<int> once{0};
            if (once.exchange(1) == 0)
            {
                __try
                {
                    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
                    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(exe + dos->e_lfanew);
                    const uintptr_t want = reinterpret_cast<uintptr_t>(menuman);
                    int found = 0;
                    const auto *sec = IMAGE_FIRST_SECTION(nt);
                    for (unsigned k = 0; k < nt->FileHeader.NumberOfSections && found < 6; ++k, ++sec)
                    {
                        if ((sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) == 0)
                            continue;
                        const uintptr_t from = exe + sec->VirtualAddress;
                        const uintptr_t to = from + sec->Misc.VirtualSize;
                        for (uintptr_t a = from; a + 8 <= to; a += 8)
                        {
                            if (*reinterpret_cast<const uintptr_t *>(a) != want)
                                continue;
                            char nm[9] = {};
                            memcpy(nm, sec->Name, 8);
                            spdlog::info("[hudprobe] CSMenuMan pointer 0x{:X} is stored at exe+0x{:X} "
                                         "(section {})", want, a - exe, nm);
                            ++found;
                        }
                    }
                    if (!found)
                        spdlog::info("[hudprobe] CSMenuMan pointer 0x{:X} not found in any data section",
                                     want);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
        }
        // HUD PROBE (2026-07-28): the state bytes that change across our screen live in an ARRAY around
        // CSMenuMan+0x90 (no instruction stores 3 at a fixed +0x97, so they are indexed entries), and
        // writing them back did not bring the HUD back. So stop guessing which byte and let the GAME show
        // us: the user's own observation is that opening any native menu restores the HUD. Dump this
        // window whenever the active menu changes, and once more right after our screen closes. The diff
        // between "HUD hidden after our close" and "HUD back after a native menu" is the answer.
        if (goblin::config::debugLogging && menuman)
        {
            static uintptr_t s_last_active = 0;
            __try
            {
                const uintptr_t act = *reinterpret_cast<uintptr_t *>(
                    reinterpret_cast<uintptr_t>(menuman) + 0x80);
                if (act != s_last_active)
                {
                    s_last_active = act;
                    char hex[3 * 64 + 1];
                    int n = 0;
                    const uint8_t *b = reinterpret_cast<const uint8_t *>(menuman) + 0x80;
                    for (int i = 0; i < 64 && n < (int)sizeof(hex) - 4; ++i)
                        n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", b[i]);
                    spdlog::info("[hudprobe] active menu -> 0x{:X}; CSMenuMan+0x80: {}", act, hex);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        if (menuman)
        {
            __try
            {
                const uintptr_t a = *reinterpret_cast<uintptr_t *>(
                    reinterpret_cast<uintptr_t>(menuman) + 0x80);
                if (a && a != g_our_dialog.load(std::memory_order_relaxed) &&
                    a != g_form_host.load(std::memory_order_relaxed))
                    g_active_menu.store(a, std::memory_order_release);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        if (menuman && p_worldchrman_slot && *p_worldchrman_slot && !goblin::maphover::map_dialog())
        {
            __try
            {
                const uintptr_t a = *reinterpret_cast<uintptr_t *>(
                    reinterpret_cast<uintptr_t>(menuman) + 0x80);
                // Only cache a base that is NOT our own settings dialog (avoid caching a
                // menu we opened). Skip while our dialog is alive.
                if (a && a != g_our_dialog.load(std::memory_order_relaxed))
                    g_base_menu.store(a, std::memory_order_release);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (goblin::config::nativeMenu)
        {
            // F8: open OUR menu - the game's keybinding screen carrying our rows, on the
            // map when it is up and over whatever else is on screen otherwise. F8 only OPENS:
            // closing belongs to the screen's own Back/Esc, and a press while it is up is
            // ignored (making it a toggle stacked a second screen on the first, because the
            // close is not instantaneous).
            static bool s_f8_down = false;
            const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (f8 && !s_f8_down)
                open_screen(goblin::nmenu::kPageRoot, 0);
            s_f8_down = f8;
            // F6 (the graphics screen as a second host) was REMOVED 2026-07-28: measured in game, its
            // list caps at the same sixteen items and does not scroll, so it bought visible rows and
            // nothing else. Its reverse engineering, and the two things worth harvesting from it
            // (centring, and its slider/combo widgets), are in
            // docs/research_f6_graphics_host_retired.md.
            tick_native_menu(); // liveness, live-apply, help line, key capture, save
        }

        // The menu-graphics probe that used to run here has been removed: under the log key it
        // called the engine's drawing primitives to fill an opaque square into whatever menu had
        // just become active. That is a visible mutation of the game's UI, not a diagnostic - and
        // its question (can we draw our own filled shapes into a native screen?) is answered in
        // docs/research_retired_native_ui_experiments.md.
        o_menu_update(menuman, dt);
    }

    // ── NATIVE SETTINGS-MENU tab injection, Proto 0 (read-only observe) ──
    // The options menu (CS::OptionSettingTopDialog) builds its tabs as a DATA list
    // CS::MenuViewItemList<CS::MenuOptionCategory> (cap 10) - the same list machinery
    // as the memo dialog we already populate. This detour on its ctor FUN_140966120
    // (0x966120) runs AFTER the game builds its ~7 tabs and (dev-only) logs the live
    // tab list: count @dialog+0x1760, entries @dialog+0x1208 stride 0x88, each a
    // MenuOptionCategory {vtable, label, id@+0x40}. Read-only: confirms the hook fires
    // on options-open + the offsets are right before we append our own tab.
    using OptTopCtorFn = void *(void *dialog, void *scene, uint8_t a, void *b);
    OptTopCtorFn *o_opttop_ctor = nullptr;
    using OptTopDtorFn = void(void *dialog); // FUN_140966980 - clears our anti-restack tracker
    OptTopDtorFn *o_opttop_dtor = nullptr;
    // The game's own category build/destruct primitives + the tab-append (HOOKED).
    using BuildCatFn = void *(void *out, void *a, void *b, void *c); // FUN_140807d60 (category, label FMG 110000)
    using DtorCatFn = void(void *category);                          // FUN_1408691a0 (destruct our temp)
    using AppendTabFn = void(void *listBase, void *category, void *c, void *d); // FUN_140967b50 (copy into vector, cap 10)
    BuildCatFn *p_build_cat = nullptr;
    DtorCatFn *p_dtor_cat = nullptr;
    AppendTabFn *o_append_tab = nullptr; // trampoline to the original append
    // We inject our tab DURING the options ctor (as one extra append), so the ctor's
    // OWN view-couple renders it - no need to re-resolve the templated couple fn (it
    // has ~31 identical instantiations, unresolvable by AOB). Single UI thread.
    std::atomic<uintptr_t> g_opt_ctor_dialog{0};
    // Our tab's category id. Game categories use 0..9 (= the page-open dispatch
    // FUN_14093c590 switch keys); an out-of-range id falls to the switch default
    // (no page) until our own page hook serves it.
    constexpr int32_t kOurCategoryId = 64;
    bool g_opt_tab_added = false; // one inject per ctor
    bool g_opt_reentrant = false; // guard our own re-entrant append
    bool g_opt_suppress = false;  // this ctor is OUR F11 open -> game tabs suppressed

    // Add our category to the tab list (once), via the trampoline (no recursion).
    void inject_our_tab(void *listBase)
    {
        if (g_opt_tab_added || g_opt_reentrant || !p_build_cat || !p_dtor_cat)
            return;
        g_opt_reentrant = true;
        __try
        {
            alignas(16) uint8_t catBuf[0x100];
            memset(catBuf, 0, sizeof(catBuf));
            p_build_cat(catBuf, nullptr, nullptr, nullptr); // full valid category (label "System", id 0)
            // Relabel: the category label is an INLINE 0x38-byte DLString at cat+0x8
            // (FUN_140807d60 builds it via MenuTextCtor FUN_140760970(out, fmgId), bank
            // GR_MenuText). Re-run that ctor in place with OUR injected GR_MenuText id
            // (goblin_messages expands the FMG at startup). Constructing over the copied
            // "System" string is safe (the ctor initializes, doesn't read old bytes); its
            // SSO content just gets replaced. Skipped if the FMG merge didn't run.
            if (goblin::g_menutext_tab_id > 0)
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                using MenuTextCtorFn = void *(void *out, uint32_t fmgId);
                auto p_menu_text = reinterpret_cast<MenuTextCtorFn *>(base + 0x760970);
                p_menu_text(catBuf + 0x8, static_cast<uint32_t>(goblin::g_menutext_tab_id));
            }
            // Unique category id (cat+0x40). The page-open dispatch FUN_14093c590
            // switches on this id (game pages 0..9; unknown id hits the default case
            // = NO page opens, harmless). Our own page hook keys on this id later.
            *reinterpret_cast<int32_t *>(catBuf + 0x40) = kOurCategoryId;
            o_append_tab(listBase, catBuf, nullptr, nullptr);
            p_dtor_cat(catBuf);
            g_opt_tab_added = true;
            spdlog::info("[optmenu] our tab added (suppress={} labelId={})",
                         g_opt_suppress, goblin::g_menutext_tab_id);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[optmenu] SEH on tab inject");
        }
        g_opt_reentrant = false;
    }

    // Hook of the tab-append FUN_140967b50. In OUR F11 open (g_opt_suppress) we SKIP
    // every game tab and add only ours. In a normal game-settings open we pass through
    // untouched (game tabs stay, we do not inject). Only fires for the options tab list.
    void append_tab_detour(void *listBase, void *category, void *c, void *d)
    {
        const uintptr_t dlg = g_opt_ctor_dialog.load(std::memory_order_relaxed);
        const bool ours_list = dlg && !g_opt_reentrant && p_build_cat &&
                               reinterpret_cast<uintptr_t>(listBase) == dlg + 0x1208;
        if (g_opt_suppress && ours_list)
        {
            inject_our_tab(listBase); // add ours once; SKIP the game tab (no o_append_tab)
            return;
        }
        o_append_tab(listBase, category, c, d); // normal path (game settings untouched)
    }

    void opttop_dtor_detour(void *dialog)
    {
        if (reinterpret_cast<uintptr_t>(dialog) == g_our_dialog.load(std::memory_order_acquire))
            g_our_dialog.store(0, std::memory_order_release); // our menu closed -> F11 can open again
        o_opttop_dtor(dialog);
    }

    // ── OUR PAGE in the native settings menu (rows = native checkboxes on config bools) ──
    // Page-open chain (RE 2026-07-23, pagedispatch_re.txt / rowappend_re.txt): tab decide
    // -> FUN_140967370 -> FUN_14093b760(pageCtl, catId) "show page for category id". The
    // page cache is a FIXED 10-slot array @pageCtl+0x68 indexed by catId UNCHECKED - our
    // id 64 would index out of bounds, so the detour must intercept BEFORE the read.
    // For kOurCategoryId we show the BORROWED page id 3 (page-open switch FUN_14093c590
    // case 3 -> FUN_14093b810: generic list page, clip #5, populate FUN_140957ef0) with
    // the populate-swap armed: the game builds a 100% native page (factory + view-couple
    // + render) whose ROWS are ours. In our F11 menu the game tabs are suppressed, so
    // cache slot 3 is otherwise unused; in the game's own settings menu our tab doesn't
    // exist and both detours pass through untouched.
    using ShowPageFn = void(void *pageCtl, int catId);
    ShowPageFn *o_show_page = nullptr;
    using PopulatePageFn = void(void *page, void *ctx);
    PopulatePageFn *o_populate_page = nullptr;
    std::atomic<int> g_our_page_mode{0}; // 1 = populate-swap armed (our tab decided)
    constexpr int kBorrowedPageCat = 3;

    // Append our rows to a freshly-built page. Row primitives (base+RVA, dev):
    //   0x760970 MenuTextCtor(out, fmgId)        - DLString from GR_MenuText (label)
    //   0x760790 (out, fmgId)                    - DLString from the help bank
    //   0x9543B0 (out)                           - build a temp ComboItemList<bool,2>
    //            (the native ON/OFF item pair; ~0xA8 bytes: vft@0, items@+8 stride
    //            0x48, count u64 @+0xA0)
    //   0x948FA0 (page, labelPack, valuePtr, comboItems, defaultPtr, u8 flag=1) -
    //            build + append one ON/OFF combo row (native checkbox) bound
    //            DIRECTLY to the byte at valuePtr (list cap 16); reference populate
    //            is the System page FUN_140958c50.
    //   0x742C90 label-pack dtor (the game destructs its pack after each append).
    void build_our_rows(void *page)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        using TextCtorFn = void *(void *out, uint32_t fmgId);
        using ComboItemsCtorFn = void *(void *out);
        using AppendComboFn = void *(void *page, void *labelPack, void *valuePtr,
                                     void *comboItems, void *defaultPtr, uint8_t flag);
        using PackDtorFn = void(void *pack);
        auto p_label = reinterpret_cast<TextCtorFn *>(base + 0x760970);
        auto p_help = reinterpret_cast<TextCtorFn *>(base + 0x760790);
        auto p_combo_items = reinterpret_cast<ComboItemsCtorFn *>(base + 0x9543B0);
        auto p_append_combo = reinterpret_cast<AppendComboFn *>(base + 0x948FA0);
        auto p_pack_dtor = reinterpret_cast<PackDtorFn *>(base + 0x742C90);
        size_t n = 0;
        const goblin::NativeMenuRowDef *rows = goblin::native_menu_rows(&n);
        int added = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const int label_id = (i < goblin::g_menutext_row_ids.size())
                                     ? goblin::g_menutext_row_ids[i]
                                     : 0;
            if (label_id <= 0)
                continue;
            // Label pack = {DLString label @0, DLString help @+0x38} (0x70 bytes),
            // the shape the game's row builders (e.g. FUN_140956830) produce. Help
            // id = a valid game one for now (0x22308, the reference populate's);
            // our own help strings need a help-bank injection later.
            alignas(16) uint8_t pack[0x70];
            memset(pack, 0, sizeof(pack));
            p_label(pack, static_cast<uint32_t>(label_id));
            p_help(pack + 0x38, 0x22308);
            // Native ON/OFF combo pair (temp, destructed after the append copies it).
            alignas(16) uint8_t comboBuf[0x100];
            memset(comboBuf, 0, sizeof(comboBuf));
            p_combo_items(comboBuf);
            // Per-row schema default; static storage - the row may keep the pointer.
            static uint8_t s_row_defaults[64];
            if (i < 64)
                s_row_defaults[i] = rows[i].def ? 1 : 0;
            p_append_combo(page, pack, rows[i].value, comboBuf,
                           &s_row_defaults[i < 64 ? i : 0], 1);
            // Destruct the temp ComboItemList exactly like the game populate does:
            // per item (stride 0x48, count @+0xA0) call vt[0](item, 0).
            const uint64_t cnt = *reinterpret_cast<uint64_t *>(comboBuf + 0xA0);
            for (uint64_t k = 0; k < cnt && k < 2; ++k)
            {
                uint8_t *item = comboBuf + 8 + k * 0x48;
                auto vt = *reinterpret_cast<void ***>(item);
                if (vt && vt[0])
                    reinterpret_cast<void (*)(void *, int)>(vt[0])(item, 0);
            }
            p_pack_dtor(pack);
            ++added;
        }
        // SCROLL TEST (2026-07-28, F6 host only): the graphics screen shows 13 rows with no scrollbar,
        // and we do not know whether one appears by itself once the rows outgrow the page. Pad with
        // recycled labels up to g_pad_rows_to and look. The engine's list may have a fixed capacity, in
        // which case this overflows it - the user asked for the answer either way. Only the F6 path sets
        // the target; F8 is untouched.
        const int pad_to = g_pad_rows_to.load(std::memory_order_relaxed);
        if (pad_to > added && !goblin::g_menutext_row_ids.empty())
        {
            static bool s_filler_value = false;
            static uint8_t s_filler_default = 0;
            const int real_rows = added;
            while (added < pad_to)
            {
                const int label_id = goblin::g_menutext_row_ids[added % goblin::g_menutext_row_ids.size()];
                if (label_id <= 0)
                    break;
                alignas(16) uint8_t pack[0x70];
                memset(pack, 0, sizeof(pack));
                p_label(pack, static_cast<uint32_t>(label_id));
                p_help(pack + 0x38, 0x22308);
                alignas(16) uint8_t comboBuf[0x100];
                memset(comboBuf, 0, sizeof(comboBuf));
                p_combo_items(comboBuf);
                p_append_combo(page, pack, &s_filler_value, comboBuf, &s_filler_default, 1);
                const uint64_t cnt = *reinterpret_cast<uint64_t *>(comboBuf + 0xA0);
                for (uint64_t k = 0; k < cnt && k < 2; ++k)
                {
                    uint8_t *item = comboBuf + 8 + k * 0x48;
                    auto vt = *reinterpret_cast<void ***>(item);
                    if (vt && vt[0])
                        reinterpret_cast<void (*)(void *, int)>(vt[0])(item, 0);
                }
                p_pack_dtor(pack);
                ++added;
            }
            spdlog::info("[optmenu] scroll test: padded {} real rows to {} on the graphics host",
                         real_rows, added);
        }
        spdlog::info("[optmenu] our page populated: {} native rows (of {})", added, n);
    }

    void show_page_detour(void *pageCtl, int catId)
    {
        if (catId != kOurCategoryId)
        {
            o_show_page(pageCtl, catId);
            return;
        }
        g_our_page_mode.store(1, std::memory_order_release);
        __try
        {
            o_show_page(pageCtl, kBorrowedPageCat); // populate fires inside on first build
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[optmenu] SEH showing our page");
        }
        g_our_page_mode.store(0, std::memory_order_release);
    }

    void populate_page_detour(void *page, void *ctx)
    {
        if (!g_our_page_mode.load(std::memory_order_acquire))
        {
            o_populate_page(page, ctx);
            return;
        }
        __try
        {
            build_our_rows(page);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[optmenu] SEH populating our page");
        }
    }

    // ── MAP SUB-DIALOG JOB step (FUN_1407ad1c0, RVA 0x7ad1c0) - the map's own "create +
    // register a sub-dialog over the map" mechanism (v19). On first step (job+0x130 == 0) it
    // optionally builds a movie (Path B, desc @job+0x58), invokes the factory (+0xa8 Path B /
    // +0xe8 Path A), stores the dialog @job+0x130, and attaches it for render/tick via
    // FUN_140733ef0(*(job+0x50), dialog). This diagnostic dumps a live job's config so we can
    // reproduce a Path-B (own-movie) settings job on the map's runner = REAL settings over the
    // map. Read-only; logs the first few DISTINCT jobs seen, dev-gated.
    using JobStepFn = void *(void *job, void *p2, void *p3, void *p4);
    JobStepFn *o_jobstep = nullptr;
    std::array<std::atomic<uintptr_t>, 8> g_jobs_seen{};
    void jobstep_dump(uintptr_t job)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            auto q = [&](uint32_t off) { return *reinterpret_cast<uint64_t *>(job + off); };
            spdlog::info("[jobstep] job=0x{:X} +0x10(scene)=0x{:X} +0x50(rendctl)=0x{:X} "
                         "+0x58..64(moviedesc)=0x{:X}/0x{:X} +0x68(movie)=0x{:X} "
                         "+0xa8(factB)=0x{:X} +0xe8(factA)=0x{:X} +0x130(dlg)=0x{:X}",
                         job, q(0x10), q(0x50), q(0x58), q(0x60), q(0x68),
                         q(0xa8), q(0xe8), q(0x130));
            const uint64_t dlg = q(0x130);
            if (dlg > 0x10000)
            {
                const uint64_t vt = *reinterpret_cast<uint64_t *>(dlg);
                spdlog::info("[jobstep]   dlg vt=exe+0x{:X}", vt > base ? vt - base : vt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[jobstep] SEH dumping job 0x{:X}", job);
        }
    }
    void *jobstep_detour(void *job, void *p2, void *p3, void *p4)
    {
        if (goblin::config::debugLogging && job)
        {
            const uintptr_t j = reinterpret_cast<uintptr_t>(job);
            bool known = false;
            int freeSlot = -1;
            for (int i = 0; i < (int)g_jobs_seen.size(); ++i)
            {
                const uintptr_t v = g_jobs_seen[i].load(std::memory_order_relaxed);
                if (v == j) { known = true; break; }
                if (v == 0 && freeSlot < 0) freeSlot = i;
            }
            if (!known && freeSlot >= 0)
            {
                g_jobs_seen[freeSlot].store(j, std::memory_order_relaxed);
                jobstep_dump(j);
            }
        }
        return o_jobstep(job, p2, p3, p4);
    }

    void *opttop_ctor_detour(void *dialog, void *scene, uint8_t a, void *b)
    {
        const uintptr_t d = reinterpret_cast<uintptr_t>(dialog);
        g_opt_tab_added = false;
        // Consume the F11 flag: if OUR open, suppress game tabs (show only ours);
        // a normal game-settings open leaves this false -> game tabs untouched.
        g_opt_suppress = g_our_f11_open.exchange(0, std::memory_order_acq_rel) != 0;
        g_opt_ctor_dialog.store(d, std::memory_order_relaxed); // arm the append hook during the ctor
        // (Task #5 scene-swap experiment REVERTED: handing the ctor the MAP scene crashed - the dialog
        // does more than name-bind from the scene; it assumes the settings-movie/job context. Full
        // re-host of the interactive dialog needs proper construction+registration in the map movie.)
        void *ret = o_opttop_ctor(dialog, scene, a, b);        // appends fire here (suppressed if ours)
        g_opt_ctor_dialog.store(0, std::memory_order_relaxed);
        const bool was_suppress = g_opt_suppress;
        g_opt_suppress = false;
        // Track OUR dialog (F11 open) so the anti-restack guard + its dtor can find it.
        if (was_suppress)
            g_our_dialog.store(d, std::memory_order_release);
        if (goblin::config::debugLogging && dialog)
        {
            __try
            {
                const uint64_t count = *reinterpret_cast<uint64_t *>(d + 0x1760);
                spdlog::info("[optmenu] ctor done: dialog=0x{:X} tabCount={} ours={} suppress={}",
                             d, count, g_opt_tab_added, was_suppress);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                spdlog::warn("[optmenu] SEH reading tab count");
            }
        }
        return ret;
    }

    // ── MEMO-DIALOG control, Task #5 Proto 0 (Route B groundwork) ──
    // The map's WorldMapMemoSelectDialog is created INSIDE the worldmap movie and
    // renders + routes input OVER the visible map - exactly the surface our native
    // settings list needs. Contract (V4b_settings_panel.md sec 2 + memo_ctor_re.raw):
    // items inline @dlg+0x1248 stride 0x1A8, count u64 @+0x3370 (HARD cap 20, assert
    // above), decide slot A = fn @item+0x88 + i32 this-offset @+0x90 + optional
    // predicate @+0x98; the invoker (0x9D4680) calls fn(dialog + off, item, arg) and
    // treats a NULL predicate as pass. Close request = **(u8**)(dlg+0xA68) = 1.
    // Proto 0 proves control on the LIVE dialog the game opens (map -> place memo):
    // post-ctor we relabel item 0 in place and point its decide at our fn; selecting
    // it must log + close the dialog. The show-site hook captures the host object -
    // every creator argument derives from it (Proto 1 = open the dialog ourselves).
    // FUN_1409d0a80 is the _Func_impl invoke body of the map's sub-dialog factory
    // std::function<MenuWindow*(SceneObjProxy const&)>: it tail-returns the created
    // dialog (rax). Typed to RETURN the dialog so we can substitute our own.
    using MemoShowFn = void *(void *ctx, void *scene);
    MemoShowFn *o_memo_show = nullptr;
    // Game heap allocator thunk FUN_141eb9ed0(rcx=size, rdx=align, r8=allocatorObj):
    // it does allocObj->vtable[0x50](allocObj, size, align). The allocator object is a
    // runtime-initialized singleton whose global ptr lives at DAT_143d87350; the memo
    // creator + MenuWindow base ctor use the same one.
    using GameAllocFn = void *(uint32_t size, uint32_t align, void *allocObj);
    GameAllocFn *p_game_alloc = nullptr;
    void **p_alloc_global = nullptr; // &DAT_143d87350; *p_alloc_global = the allocator obj
    // ROUTE A EXPERIMENT: when set, an open of the map's place-marker (memo) sub-dialog
    // is HIJACKED - instead of the memo dialog we allocate + construct the REAL
    // OptionSettingTopDialog with the SAME map SceneObjProxy and return IT into the
    // map's ownership pipeline, so the map ticks/renders/routes-input to our settings
    // dialog exactly as it would the memo (native checkboxes/tabs over the live map).
    // Dev-only (debug_logging). OptionSettingTopDialog object size = 0x18a0 (from the
    // settings build-job factory: FUN_141eb9ed0(0x18a0,8) then ctor FUN_140966120).
    // ON: functional settings dialog over the map (graft from ItemList) + refcount=1 close
    // fix, diagnostics removed. Deploy INJECTOR ONLY.
    constexpr bool HIJACK_MEMO_WITH_SETTINGS = false; // v29: settings now open via F11 + our own
                                                      // 02_040 movie; leave the marker dialog alone
    constexpr uint32_t OPTTOP_SIZE = 0x18a0;
    using MemoCtorFn = void *(void *dlg, void *host, void *scene, uint8_t flag,
                              void *p5, void *p6, void *p7, void *p8, void *p9,
                              void *p10); // FUN_1409d21c0
    MemoCtorFn *o_memo_ctor = nullptr;
    // View-couple FUN_1409d1ff0 (a template with ~31 byte-identical instantiations -
    // resolved via its UNIQUE call site inside the memo ctor, not by prologue): wires
    // the Scaleform "ItemList" to the item data + selection. Re-run after any runtime
    // repopulate/relabel.
    using MemoCoupleFn = void(void *listProxy /*dlg+0xA78*/, void *itemList /*dlg+0x1240*/,
                              uint32_t sel, uint8_t b);
    MemoCoupleFn *p_memo_couple = nullptr;

    constexpr size_t MEMO_OFF_LISTPROXY = 0xA78; // "ItemList" scene proxy
    constexpr size_t MEMO_OFF_LIST = 0x1240;     // BasicViewItemList<_Item,20>
    constexpr size_t MEMO_OFF_ITEMS = 0x1248;    // inline items
    constexpr size_t MEMO_ITEM_STRIDE = 0x1A8;
    constexpr size_t MEMO_OFF_COUNT = 0x3370;    // u64, HARD CAP 20
    constexpr size_t MEMO_OFF_CLOSEREQ = 0xA68;  // u8** -> write 1 to close
    constexpr size_t MEMO_IT_PAYLOAD = 0x48;     // u32 per-row payload
    constexpr size_t MEMO_IT_FN = 0x88;          // decide fn (slot A)
    constexpr size_t MEMO_IT_FNOFF = 0x90;       // i32 this-offset for the call
    constexpr size_t MEMO_IT_PRED = 0x98;        // predicate fn (0 = always pass)

    std::atomic<uintptr_t> g_memo_host{0};  // captured show-site host (Proto 1 seed)
    std::atomic<uintptr_t> g_memo_scene{0}; // captured map scene at show time
    std::atomic<uintptr_t> g_memo_ctx{0};   // captured show-site ctx (the map sub-dialog controller)
    std::atomic<uintptr_t> g_memo_dialog{0}; // last constructed dialog (log only)

    // Route A groundwork: find WHERE the map stores its current sub-dialog. When a row
    // is selected the dialog is fully live + owned by the map, so scan the controller
    // objects (ctx, host, *(ctx), *(host)) for a qword == the dialog ptr and log the
    // offsets. That offset is the map's "current sub-dialog" member; the map's per-frame
    // update reads it to tick/render/route-input to the dialog (the mechanism our
    // OptionSettingTopDialog must join). Read-only, SEH-guarded.
    void memo_scan_one_root(const char *tag, uintptr_t base, uintptr_t dialog)
    {
        if (!v3_heap_ptr(base))
            return;
        __try
        {
            for (uint32_t off = 0; off <= 0x5000; off += 8)
                if (*reinterpret_cast<uintptr_t *>(base + off) == dialog)
                    spdlog::info("[memoproto]   OWNER {}+0x{:X} == dialog", tag, off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[memoproto] SEH scanning {} for dialog", tag);
        }
    }

    void memo_scan_owner_for_dialog(uintptr_t dialog)
    {
        const uintptr_t ctx = g_memo_ctx.load(std::memory_order_relaxed);
        const uintptr_t host = g_memo_host.load(std::memory_order_relaxed);
        memo_scan_one_root("ctx", ctx, dialog);
        memo_scan_one_root("host", host, dialog);
        __try
        {
            if (v3_heap_ptr(ctx))
                memo_scan_one_root("*ctx", *reinterpret_cast<uintptr_t *>(ctx), dialog);
            if (v3_heap_ptr(host))
                memo_scan_one_root("*host", *reinterpret_cast<uintptr_t *>(host), dialog);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // In-place relabel of the item's owned label string (from the ctor's textObj;
    // a basic_string with a stateful FD4 allocator in a _Compressed_pair, verified
    // against the _Item ctor 0x9d3890 + the memo-ctor stack mirror: header @+0x10,
    // allocator @+0x18, SSO buf 8 wchars @+0x20 OR heap ptr when cap > 7, size
    // @+0x30, cap @+0x38).

    // Our decide handler; the invoker calls fn(dialog + *(i32*)(item+0x90), item, arg),
    // so with +0x90 = 0 rcx is the dialog itself. Proves callback + close control.

    // Post-ctor Proto 0 mutation: log the live item table, then take over item 0
    // (relabel + our decide, predicate cleared) and re-run the view-couple so the
    // Scaleform list re-reads the data ([VERIFY] redraw semantics outside the ctor).


    // SEH-safe structural dump of a scene proxy: its vtable, root (+0x20), the root's
    // vtable, and +0x28. Tells us whether map_scene() is a live proxy shaped like the
    // memo scene (crash is elsewhere) or stale/garbage (need a fresh map-root scene).

    // Dump what "TabList"/"ItemList"/bogus resolve to under a scene (8 qwords each). The
    // field that DIFFERS between a real name and the bogus name is the resolved handle;
    // the scene under which "TabList" differs from bogus is where our clips bind.

    // A name resolves under `scene` iff the handle field (resolver out+0x30 = dump d[2])
    // is a real value, not the 0xFFFFFFFFFFFFFFFF "not found" sentinel (learned from the
    // scene diagnostic: ItemList -> real ptr, TabList/bogus -> FFFF..FF under the memo scene).

    // Scan an object for EMBEDDED SceneObjProxy members (a qword slot whose value equals
    // the SceneObjProxy vftable) and, for each, test whether our map-ROOT clips resolve
    // under it. The map's own WorldMapDialog stores a chrome proxy rooted at the map
    // movie root (where StatusBar/MenuTitle AND our spliced TabList live) - that is the
    // scene our OptionSettingTopDialog must bind against. `sceneProxyVt` is captured live
    // (= *(memoScene)). Logs any proxy under which StatusBar or TabList resolves.

    // F11-direct deferred builder (fwd-declared above; body here where the opttop/alloc decls
    // are in scope). Once our 02_040 movie async-loads (scene root != 0), construct the dialog
    // against its scene. The movie renders itself over the map; the dialog binds real 02_040
    // clips at ctor -> real settings skin over the live map.

    // Construct the REAL OptionSettingTopDialog against the map's SceneObjProxy and
    // return it into the map's sub-dialog ownership pipeline. Returns null on any
    // failure so the caller can fall back to the memo dialog. SEH-guarded.

    // RENDER DIAGNOSTIC: dump a dialog's identifying fields so we can DIFF our
    // OptionSettingTopDialog against the REAL memo dialog and find the state that gates
    // rendering over the map. Logs the vtable (exe-relative) + the DLReferenceCountObject
    // refcount + every non-zero qword in [0x00..0x120] (the header + base MenuWindow region,
    // where menu-id / owner / scene / visibility live). Read-only, SEH-guarded.


    void v3_native_factory_consume(void *live_ctx, uint32_t frame)
    {
        if (!g_v3_native.seeded || g_v3_native.in_factory)
            return;
        // Fully SYNCHRONOUS pipeline: queue a batch of records, run the
        // engine's own materialization driver on them, transfer the children,
        // neutralize the records - all inside this one pulse. No cross-pulse
        // state; leftovers can only mean a previous pulse aborted mid-way.
        if (g_v3_factory_active != 0)
            v3_factory_clear_request(true);
        if (!g_v3_mat_driver || g_v3_native.frame_budget == 0 ||
            g_v3_native.pending_index >= g_v3_native.pending.size())
            return;
        uint64_t sprite = 0;
        if (!v3_read64(reinterpret_cast<uintptr_t>(live_ctx) + 0x58, sprite) ||
            !v3_heap_ptr(sprite))
            return;
        g_v3_native.in_factory = true;

        // Per-item synchronous pipeline: queue ONE record, run the engine's
        // materialization driver, transfer the child, neutralize the record -
        // then the next item, up to BATCH per pulse. Strictly one in flight
        // keeps the per-icon SHARED tag legal: its depth is re-patched only
        // after the previous record was decoded and neutralized. (The 16-wide
        // variant needed a fresh tag per request - ~9.4k extra movie-heap
        // allocations that all came back as close-teardown frees.)
        uint32_t target_levels = 0;
        const uintptr_t target_root = v3_parent_root(g_v3_native.parent, target_levels);
        uint32_t attempts = 0;
        while (attempts < V3_FACTORY_BATCH &&
               g_v3_native.frame_budget != 0 &&
               g_v3_native.pending_index < g_v3_native.pending.size())
        {
            ++attempts;
            const auto point = g_v3_native.pending[g_v3_native.pending_index++];
            --g_v3_native.frame_budget;
            float mx = 0.0f, mz = 0.0f;
            if (!goblin::mapproject::to_map(point.area, point.gx, point.gz,
                                            point.px, point.pz, mx, mz))
            {
                ++g_v3_native.failed;
                continue;
            }
            const uint32_t char_id =
                goblin::gfx_probe::native_character_id(point.source_icon_id);
            if (char_id == 0)
            {
                ++g_v3_native.failed;
                continue;
            }
            const uint16_t depth = g_v3_native.next_depth++;
            // Arm the slot BEFORE issuing: capture can fire synchronously
            // inside the Execute call.
            auto &s = g_v3_factory_slots[0];
            s = V3FactorySlot{};
            s.depth = depth;
            s.char_id = char_id;
            s.point = point;
            s.map_x = mx;
            s.map_z = mz;
            g_v3_factory_active = 1;
            const uintptr_t node = goblin::gfx_probe::create_native_icon_instance(
                point.source_icon_id, depth, live_ctx, frame);
            bool ok = false;
            if (!v3_heap_ptr(node))
            {
                static uint32_t s_no_node_logged = 0;
                if (s_no_node_logged++ < 4)
                    spdlog::warn("[v3native] request produced no timeline node "
                                 "(row={} depth={})",
                                 point.original_row_id, depth);
            }
            else
            {
                s.node = node;
                if (!s.held || !v3_heap_ptr(s.child))
                {
                    const uint32_t mat_exc = v3_guarded_materialize(
                        live_ctx, static_cast<uintptr_t>(sprite));
                    if (mat_exc)
                    {
                        static bool s_mat_exc_logged = false;
                        if (!s_mat_exc_logged)
                        {
                            s_mat_exc_logged = true;
                            spdlog::warn("[v3native] materialize driver seh=0x{:08X}",
                                         mat_exc);
                        }
                    }
                }
                if (!s.held || !v3_heap_ptr(s.child))
                {
                    static uint32_t s_uncaptured_logged = 0;
                    if (s_uncaptured_logged++ < 4)
                        spdlog::warn("[v3native] record not materialized by driver "
                                     "(row={} depth={})",
                                     s.point.original_row_id, s.depth);
                }
                else if (!v3_heap_ptr(target_root) || target_root != s.root)
                {
                    ++g_v3_native.wrong_contexts;
                    spdlog::warn("[v3native] child root changed: childRoot=0x{:X} "
                                 "targetRoot=0x{:X}",
                                 s.root, target_root);
                }
                else
                {
                    if (s.point.original_row_id & goblin::NATIVE_CLEARED_KEY_BIT)
                    {
                        for (float &m : s.basis)
                            m *= V3_BADGE_SCALE;
                        s.base_tx = s.base_tx * V3_BADGE_SCALE + V3_BADGE_OFF_X;
                        s.base_ty = s.base_ty * V3_BADGE_SCALE + V3_BADGE_OFF_Y;
                    }
                    const uint32_t exc = v3_guarded_attach(g_v3_native.wrapper, s.child);
                    uint64_t child_parent = 0;
                    v3_read64(s.child + 0x38, child_parent);
                    const bool attached = exc == 0 && child_parent == g_v3_native.parent;
                    const bool positioned = attached &&
                        v3_position_child(s.child,
                                          s.point.visible ? s.map_x : V3_HIDDEN_MAP_POS,
                                          s.point.visible ? s.map_z : V3_HIDDEN_MAP_POS,
                                          s.base_tx, s.base_ty, s.basis,
                                          g_v3_native.cur_fx, g_v3_native.cur_fy);
                    if (!positioned)
                        spdlog::warn("[v3native] attach failed: seh=0x{:08X} "
                                     "parent=0x{:X} expected=0x{:X}",
                                     exc, child_parent, g_v3_native.parent);
                    else
                        ok = true;
                }
            }
            if (s.held && v3_heap_ptr(s.child))
            {
                if (goblin::variants::kViewportWindow && ok)
                {
                    // Lever B: KEEP our reference so the child survives while it is
                    // DETACHED (out of view). Without it, detaching (which drops the
                    // parent's reference) would free the child. The manager (obj.child)
                    // now owns this reference; it is released only by movie teardown.
                    s.held = false;
                }
                else
                {
                    uint32_t rb = 0, ra = 0;
                    v3_drop_held_ref(s.child, rb, ra);
                    s.held = false;
                }
            }
            if (ok)
            {
                V3NativeObject obj{};
                obj.row_id = s.point.original_row_id;
                obj.source_icon_id = s.point.source_icon_id;
                obj.child = s.child;
                obj.map_x = s.map_x;
                obj.map_z = s.map_z;
                obj.base_tx = s.base_tx;
                obj.base_ty = s.base_ty;
                memcpy(obj.base_m, s.basis, sizeof(obj.base_m));
                obj.visible = s.point.visible;
                // We kept the build reference above iff B was on -> this child may be
                // safely detached/re-attached by the viewport reconcile.
                obj.ref_held = goblin::variants::kViewportWindow;
                g_v3_native.last_progress_ms = GetTickCount64();
                g_v3_native.by_row[s.point.original_row_id] =
                    g_v3_native.objects.size();
                g_v3_native.objects.push_back(obj);
                const size_t created = g_v3_native.objects.size();
                if (created == 1 || created % 256 == 0 ||
                    g_v3_native.pending_index >= g_v3_native.pending.size())
                    spdlog::info("[v3native] category progress: created={}/{} "
                                 "failed={} lastRow={}{} srcIconId={} "
                                 "map=({:.1f},{:.1f})",
                                 created, g_v3_native.pending.size(),
                                 g_v3_native.failed,
                                 s.point.original_row_id &
                                     ~goblin::NATIVE_CLEARED_KEY_BIT,
                                 (s.point.original_row_id &
                                  goblin::NATIVE_CLEARED_KEY_BIT)
                                     ? "(badge)"
                                     : "",
                                 s.point.source_icon_id, s.map_x, s.map_z);
            }
            else
            {
                ++g_v3_native.failed;
            }
            if (v3_heap_ptr(node))
                goblin::gfx_probe::remove_native_icon_record(depth, live_ctx, frame);
            s = V3FactorySlot{};
            g_v3_factory_active = 0;
        }
        g_v3_native.in_factory = false;
    }

    // Lever B (variants::kViewportWindow): keep out-of-view markers DETACHED so they carry
    // no TreeCacheNode; attach only those within the visible map rect (+ hysteresis).
    // Runs once per map frame on the map UI thread; bounded ops/frame so a big pan
    // reconciles across a few frames instead of one spike. Detaching happens while the
    // movie is alive and rendering, so the freed render nodes recycle on normal frames
    // (async) - unlike the close-time lever C, whose nodes never get a reconcile pass.
    size_t g_v3_vp_cursor = 0; // (unused; reserved for round-robin if needed)
    void v3_viewport_reconcile()
    {
        if (!goblin::variants::kViewportWindow || !g_v3_remove_at)
            return;
        if (!g_v3_native.seeded || g_v3_native.objects.empty() ||
            g_v3_map_closed.load(std::memory_order_relaxed))
            return;
        const uintptr_t wrapper = g_v3_native.wrapper;
        const uintptr_t parent = g_v3_native.parent;
        if (!v3_heap_ptr(wrapper) || !v3_heap_ptr(parent))
            return;
        // Parent-liveness gate. A freed and zeroed parent still passes v3_heap_ptr, and even
        // wrapper+0x18 == parent still holds (that field lives in the wrapper, not in the freed
        // block), so the structural checks alone would let us relink a DEAD container and corrupt
        // the heap. The parent's own vtable slot is the reliable death signal: it reads 0 once
        // freed. Require it non-zero and matching this generation before any engine detach or
        // attach touches the container.
        {
            uint64_t pvt = 0;
            if (!v3_read64(parent, pvt) || pvt == 0 ||
                (g_v3_native.parent_vtable != 0 && pvt != g_v3_native.parent_vtable))
                return;
        }

        goblin::mapproject::MapView view{};
        if (!goblin::mapproject::read_view(view) || !(view.zoom > 0.01f))
            return;

        // Visible map-space rect: screen_x in [0,w] <=> |map_x - cU| <= (1920/2)/zoom
        // (the client-size factor cancels, see mapproject::project). MARGIN (virtual px)
        // is hysteresis so pan jitter near an edge does not thrash attach/detach.
        const float cU = (view.panX + view.snapMidX) / view.zoom;
        const float cV = (view.panZ + view.snapMidZ) / view.zoom;
        constexpr float MARGIN = 400.0f;
        const float halfX = (960.0f + MARGIN) / view.zoom;
        const float halfZ = (540.0f + MARGIN) / view.zoom;

        constexpr size_t DETACH_BUDGET = 1024;
        constexpr size_t ATTACH_BUDGET = 1024;

        auto in_window = [&](const V3NativeObject &o) {
            return o.visible && std::fabs(o.map_x - cU) <= halfX &&
                   std::fabs(o.map_z - cV) <= halfZ;
        };

        // Phase 1: collect out-of-view (or hidden) attached children, batch-detach them
        // via the same engine remove-at leaves lever C uses (one parent scan, descending
        // removal). Their retained reference (kept at build) keeps them alive detached.
        static std::vector<uintptr_t> det_ptrs;
        static std::vector<size_t> det_objs;
        det_ptrs.clear();
        det_objs.clear();
        for (size_t i = 0; i < g_v3_native.objects.size() &&
                           det_objs.size() < DETACH_BUDGET; ++i)
        {
            V3NativeObject &o = g_v3_native.objects[i];
            if (o.ref_held && o.attached && v3_heap_ptr(o.child) && !in_window(o))
            {
                det_ptrs.push_back(o.child);
                det_objs.push_back(i);
            }
        }
        if (!det_ptrs.empty())
        {
            std::sort(det_ptrs.begin(), det_ptrs.end());
            static std::vector<uint32_t> idxbuf;
            idxbuf.assign(det_ptrs.size(), 0);
            uint32_t faulted = 0;
            const uint32_t found = v3_detach_scan(parent, det_ptrs.data(),
                                                  static_cast<uint32_t>(det_ptrs.size()),
                                                  idxbuf.data(),
                                                  static_cast<uint32_t>(idxbuf.size()), &faulted);
            if (faulted)
                spdlog::warn("[v3native] viewport detach: child-vector scan faulted, removing nothing "
                             "(the vector moved under us; {} markers stay attached this pass).",
                             det_objs.size());
            uint32_t skipped = 0;
            v3_detach_remove(wrapper, parent, idxbuf.data(), found, det_ptrs.data(),
                             static_cast<uint32_t>(det_ptrs.size()), &skipped);
            if (skipped)
                spdlog::warn("[v3native] viewport detach: {} of {} indices no longer pointed at our "
                             "children and were SKIPPED (the child vector moved between scan and "
                             "removal; removing them would have freed engine-owned objects).",
                             skipped, found);
            for (size_t j : det_objs)
                g_v3_native.objects[j].attached = false;
        }

        // Phase 2: attach in-view children that are currently detached (append = on top).
        size_t attached_now = 0;
        for (size_t i = 0; i < g_v3_native.objects.size() &&
                           attached_now < ATTACH_BUDGET; ++i)
        {
            V3NativeObject &o = g_v3_native.objects[i];
            if (!o.ref_held || o.attached || !v3_heap_ptr(o.child) || !in_window(o))
                continue;
            const uint32_t exc = v3_guarded_attach(wrapper, o.child);
            uint64_t child_parent = 0;
            v3_read64(o.child + 0x38, child_parent);
            if (exc == 0 && child_parent == parent)
            {
                v3_position_child(o.child, o.map_x, o.map_z, o.base_tx, o.base_ty,
                                  o.base_m, g_v3_native.cur_fx, g_v3_native.cur_fy);
                o.attached = true;
                ++attached_now;
            }
        }
    }

    void v3_try_matrix_batch()
    {
        if (g_v3_matrix_state.load(std::memory_order_acquire) != 0)
            return;
        const uint64_t started = g_v3_matrix_started_ms.load(std::memory_order_acquire);
        if (!started || GetTickCount64() - started < 3500)
            return;
        for (uint32_t i = 0; i < V3_MATRIX_SLOTS; ++i)
            if (!v3_heap_ptr(g_v3_matrix_children[i].load(std::memory_order_acquire)))
                return;

        V3Candidate control{};
        control.count = g_v3_target_count.load(std::memory_order_acquire);
        control.wrapper = g_v3_target_wrapper.load(std::memory_order_relaxed);
        control.parent = g_v3_target_parent.load(std::memory_order_relaxed);
        uint64_t wrapper_parent = 0, live_count = 0;
        if (!v3_heap_ptr(control.wrapper) || !v3_heap_ptr(control.parent) ||
            !v3_read64(control.wrapper + 0x18, wrapper_parent) ||
            wrapper_parent != control.parent || !v3_read64(control.parent + 0xe0, live_count))
            return;
        control.count = live_count;

        // The generated surface dataset contains 6405 of our rows; the live
        // 6468-object container is therefore the leading candidate for the real
        // native marker layer (our rows plus currently eligible game/ERR points).
        // Put the whole visual grid there while the build-scope correlation below
        // independently verifies which parent native buildMarkers populates.
        V3Candidate selected[V3_MATRIX_SLOTS]{};
        for (uint32_t slot = 0; slot < V3_MATRIX_SLOTS; ++slot)
            selected[slot] = control;

        static constexpr const char *CELL[V3_MATRIX_SLOTS] =
            {"TL", "TC", "TR", "ML", "MC", "MR", "BL", "BR"};
        static constexpr float DX[V3_MATRIX_SLOTS] =
            {80, 240, 400, 80, 240, 400, 80, 400};
        static constexpr float DY[V3_MATRIX_SLOTS] =
            {-1000, -1000, -1000, -840, -840, -840, -680, -680};

        goblin::mapproject::MapView view{};
        if (!goblin::mapproject::read_view(view))
            return;

        uint32_t expected = 0;
        if (!g_v3_matrix_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            return;

        uint32_t control_levels = 0;
        const uintptr_t control_root = v3_parent_root(control.parent, control_levels);
        spdlog::info("[v3matrix] WorldMapItem attach: parent=0x{:X} "
                     "count={} root=0x{:X}/{} fullMid=({:.1f},{:.1f})",
                     control.parent, control.count, control_root, control_levels,
                     view.fullMidX, view.fullMidZ);

        for (uint32_t slot = 0; slot < V3_MATRIX_SLOTS; ++slot)
        {
            const V3Candidate c = selected[slot];
            const uintptr_t child = g_v3_matrix_children[slot].load(std::memory_order_acquire);
            if (!v3_heap_ptr(c.wrapper) || !v3_heap_ptr(c.parent))
            {
                spdlog::info("[v3matrix] cell={} slot={} WorldMapItem candidate=NONE",
                             CELL[slot], slot);
                continue;
            }
            uint64_t before_count = 0, after_count = 0, child_parent_before = 0;
            uint64_t child_parent_after = 0;
            v3_read64(c.parent + 0xe0, before_count);
            v3_read64(child + 0x38, child_parent_before);
            uint32_t child_levels = 0, candidate_levels = 0;
            const uintptr_t child_root = v3_parent_root(child, child_levels);
            const uintptr_t candidate_root = v3_parent_root(c.parent, candidate_levels);
            const int src_icon = slot < static_cast<uint32_t>(goblin::generated::MAP_ICON_TAG_COUNT)
                                     ? goblin::generated::MAP_ICON_TAGS[slot].srcIconId : -1;
            if (candidate_root != control_root || child_root != control_root)
            {
                spdlog::warn("[v3matrix] cell={} slot={} SKIPPED root mismatch: candidate=0x{:X} "
                             "child=0x{:X} control=0x{:X}",
                             CELL[slot], slot, candidate_root, child_root, control_root);
                continue;
            }

            uint32_t refs_before_hold = 0, refs_held = 0;
            if (!v3_hold_ref(child, refs_before_hold, refs_held))
            {
                spdlog::warn("[v3matrix] cell={} slot={} SKIPPED: AddRef failed", CELL[slot], slot);
                continue;
            }

            const uintptr_t game_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const uintptr_t caller_rva = c.caller >= game_base ? c.caller - game_base : c.caller;
            spdlog::info("[v3matrix] PRE cell={} slot={} srcIconId={} child=0x{:X} oldParent=0x{:X} "
                         "target=0x{:X} count={} caller=exe+0x{:X} root=0x{:X}/{} refs {}->{}",
                         CELL[slot], slot, src_icon, child, child_parent_before, c.parent,
                         before_count, caller_rva, candidate_root, candidate_levels,
                         refs_before_hold, refs_held);
            spdlog::default_logger()->flush();

            const uint32_t attach_exception = v3_guarded_attach(c.wrapper, child);
            const bool attach_returned = attach_exception == 0;
            if (attach_exception)
                spdlog::error("[v3matrix] cell={} slot={} attach raised SEH 0x{:08X}",
                              CELL[slot], slot, attach_exception);

            v3_read64(c.parent + 0xe0, after_count);
            v3_read64(child + 0x38, child_parent_after);
            const float map_x = view.fullMidX + DX[slot];
            const float map_y = view.fullMidZ + DY[slot];
            const bool attached = attach_returned && child_parent_after == c.parent;
            const bool positioned = attached &&
                v3_position_child(child, map_x, map_y,
                                  g_v3_matrix_base_tx[slot], g_v3_matrix_base_ty[slot]);
            uint32_t refs_before_drop = 0, refs_after_drop = 0;
            const bool dropped = v3_drop_held_ref(child, refs_before_drop, refs_after_drop);
            spdlog::info("[v3matrix] POST cell={} slot={} count {}->{} parent=0x{:X} attached={} "
                         "positioned={} map=({:.1f},{:.1f}) refs {}->{} dropped={}",
                         CELL[slot], slot, before_count, after_count, child_parent_after,
                         attached, positioned, map_x, map_y,
                         refs_before_drop, refs_after_drop, dropped);
            if (!attach_returned)
                break;
        }
    }

    void v3_try_visual_move()
    {
        const uintptr_t wrapper = g_v3_target_wrapper.load(std::memory_order_acquire);
        const uintptr_t target_parent = g_v3_target_parent.load(std::memory_order_acquire);
        const uintptr_t child = g_v3_custom_child.load(std::memory_order_acquire);
        if (!v3_heap_ptr(wrapper) || !v3_heap_ptr(target_parent) || !v3_heap_ptr(child))
            return;

        uint64_t wrapper_parent = 0, target_count = 0, old_parent = 0, old_count = 0;
        if (!v3_read64(wrapper + 0x18, wrapper_parent) || wrapper_parent != target_parent ||
            !v3_read64(target_parent + 0xe0, target_count) || target_count != 104 ||
            !v3_read64(child + 0x38, old_parent) || !v3_heap_ptr(old_parent) ||
            old_parent == target_parent)
            return;
        v3_read64(static_cast<uintptr_t>(old_parent) + 0xe0, old_count);

        goblin::mapproject::MapView view{};
        if (!goblin::mapproject::read_view(view))
            return;

        uint32_t expected = 0;
        if (!g_v3_visual_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            return;

        V3VisualSnapshot s{};
        s.wrapper = wrapper;
        s.target_parent = target_parent;
        s.old_parent = static_cast<uintptr_t>(old_parent);
        s.child = child;
        s.target_before = target_count;
        s.old_before = old_count;

        // Use the engine's normal move/attach operation. Index 0 matches the
        // native map layer's observed insertion convention.
        o_v3_attach(reinterpret_cast<void *>(wrapper), reinterpret_cast<void *>(child), 0);

        v3_read64(target_parent + 0xe0, s.target_after);
        v3_read64(s.old_parent + 0xe0, s.old_after);
        v3_read64(child + 0x38, s.child_parent_after);

        // DisplayObject vtbl +0x10/+0x18 are GetMatrix/SetMatrix. Preserve the
        // sprite's scale/rotation and place its local translation at the live
        // map-space view centre, which projects to the middle of the viewport.
        using GetMatrixFn = const float *(void *);
        using SetMatrixFn = void(void *, const float *);
        uint64_t vt = 0, get_addr = 0, set_addr = 0;
        if (v3_read64(child, vt) && v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) &&
            v3_read64(static_cast<uintptr_t>(vt) + 0x18, set_addr) &&
            v3_heap_ptr(get_addr) && v3_heap_ptr(set_addr))
        {
            float matrix[8]{};
            __try
            {
                auto *get_matrix = reinterpret_cast<GetMatrixFn *>(get_addr);
                auto *set_matrix = reinterpret_cast<SetMatrixFn *>(set_addr);
                const float *current = get_matrix(reinterpret_cast<void *>(child));
                memcpy(matrix, current, sizeof(matrix));
                s.old_tx = matrix[3];
                s.old_ty = matrix[7];
                s.reference_zoom = view.zoom;
                s.base_m0 = matrix[0];
                s.base_m1 = matrix[1];
                s.base_m4 = matrix[4];
                s.base_m5 = matrix[5];
                // GFx stores translation in twips (1/20 pixel), while MapView
                // pan/snap coordinates and our projection math use pixels.
                constexpr float kTwipsPerPixel = 20.0f;
                // Fixed world-map centre, independent of the current viewport/pan/zoom.
                // The previous experiment used (pan+visibleMid)/zoom, which is the
                // point under the screen cursor and therefore moved between opens.
                s.map_mid_x = view.fullMidX;
                s.map_mid_y = view.fullMidZ;
                const float target_tx = view.fullMidX * kTwipsPerPixel;
                const float target_ty = view.fullMidZ * kTwipsPerPixel;

                // The generated matrix already contains both the intended native
                // icon scale and the centring pivot. The real marker parent supplies
                // the zoom compensation, so preserve the authored 2x2 basis and add
                // only the fixed map-space anchor to its existing translation.
                matrix[3] = target_tx + s.old_tx;
                matrix[7] = target_ty + s.old_ty;
                set_matrix(reinterpret_cast<void *>(child), matrix);
                s.new_tx = matrix[3];
                s.new_ty = matrix[7];
                s.positioned = true;

                // Deliberately do not arm the old background-layer counter-scale.
                // This candidate parent must prove that it supplies native marker
                // zoom/z-order on its own.
                g_v3_scale_ready.store(false, std::memory_order_release);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                s.positioned = false;
            }
        }
        g_v3_visual = s;
        g_v3_visual_published.store(true, std::memory_order_release);
    }

    void v3_note_add(void *vector_ptr, void *parent_ptr, void *index_arg,
                     void *child_ptr, uintptr_t ret, uint64_t before_count)
    {
        const uintptr_t vector = reinterpret_cast<uintptr_t>(vector_ptr);
        const uintptr_t parent = reinterpret_cast<uintptr_t>(parent_ptr);
        uint64_t after_count = 0;
        if (!v3_heap_ptr(vector) || !v3_read64(vector + 8, after_count) || after_count > 100000)
            return;

        g_v3_add_calls.fetch_add(1, std::memory_order_relaxed);
        if (after_count > before_count)
            g_v3_add_grew.fetch_add(1, std::memory_order_relaxed);
        else if (after_count == before_count)
            g_v3_add_same.fetch_add(1, std::memory_order_relaxed);

        const bool owner_match = v3_heap_ptr(parent) && vector == parent + 0xd8;
        if (owner_match)
            g_v3_add_owner_match.fetch_add(1, std::memory_order_relaxed);

        if (after_count <= g_v3_add_max_count.load(std::memory_order_relaxed) ||
            g_v3_add_lock.test_and_set(std::memory_order_acquire))
            return;
        if (after_count > g_v3_add_max_count.load(std::memory_order_relaxed))
        {
            V3AddSnapshot s{};
            s.vector = vector;
            s.parent = parent;
            s.child = reinterpret_cast<uintptr_t>(child_ptr);
            s.ret = ret;
            s.before_count = before_count;
            s.after_count = after_count;
            s.insert_index = reinterpret_cast<uintptr_t>(index_arg);
            s.owner_match = owner_match;
            v3_read64(s.child, s.child_vt);
            v3_read32(s.child + 0x2c, s.child_depth);
            g_v3_add = s;
            g_v3_add_max_count.store(after_count, std::memory_order_release);
        }
        g_v3_add_lock.clear(std::memory_order_release);
    }

    void v3_add_detour(void *vector, void *parent, void *index_arg, void *child)
    {
        // The map's native children are inserted before the first per-marker refresh call starts a
        // sampling window, so this feeds the stall profiler and is built only with it: it takes a
        // spinlock on one of the engine's hottest paths (every display-list add), which is exactly the
        // kind of cost that must not be present in a run whose timings we trust.
        const bool capture = goblin::variants::kStallProfiler && goblin::config::debugLogging;
        uint64_t before_count = 0;
        if (capture && v3_heap_ptr(reinterpret_cast<uintptr_t>(vector)))
            v3_read64(reinterpret_cast<uintptr_t>(vector) + 8, before_count);
        const uintptr_t ret = capture ? reinterpret_cast<uintptr_t>(_ReturnAddress()) : 0;
        o_v3_add(vector, parent, index_arg, child);
        if (capture)
            v3_note_add(vector, parent, index_arg, child, ret, before_count);
    }

    void v3_place_detour(void *vector_ptr, void *parent_ptr, void *placement_ptr,
                         void *child_ptr, uint64_t replace)
    {
        v3_note_thread("v3_place_detour (engine AddDisplayObject)");
        // Does the ENGINE add children to the very container we remove from by index, and from which
        // thread? That decides whether the scan-then-remove-by-index pass can be handed an index that
        // has since moved (which would free an engine-owned child and leave a hole where a map tile
        // was). Rate-limited to a handful of lines; reports the child count so we can also see whether
        // this container is the one that carries the map's own clips alongside our markers.
        if (parent_ptr && reinterpret_cast<uintptr_t>(parent_ptr) == g_v3_native.parent)
        {
            static std::atomic<int> shared_logs{0};
            const uint32_t tid = GetCurrentThreadId();
            const uint32_t owner = g_v3_owner_tid.load(std::memory_order_relaxed);
            if (shared_logs.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                uint64_t count = 0;
                v3_read64(reinterpret_cast<uintptr_t>(parent_ptr) + 0xe0, count);
                spdlog::info("[v3native] engine adds into OUR marker parent 0x{:X}: tid {} (manager "
                             "owner tid {}), child count now {}{}",
                             reinterpret_cast<uintptr_t>(parent_ptr), tid, owner,
                             count & 0xFFFFFFFF,
                             (owner && tid != owner) ? " - CROSS-THREAD, index surgery is unsafe" : "");
            }
        }
        uint32_t char_id = 0;
        uint32_t depth = 0;
        const uintptr_t placement = reinterpret_cast<uintptr_t>(placement_ptr);
        // The eight-cell discovery grid is retired. Keep this historical hook
        // pass-through while the surrounding RE diagnostics are still present,
        // but never capture/stage production native-marker factory objects.
        const bool custom = false;
        uint64_t before_count = 0;
        if (custom)
            v3_read64(reinterpret_cast<uintptr_t>(vector_ptr) + 8, before_count);
        const uintptr_t ret = custom ? reinterpret_cast<uintptr_t>(_ReturnAddress()) : 0;

        // PlaceObject::Execute only queues a 0x78 timeline record. The real
        // DisplayObject arrives here later through Sprite::AddDisplayObject,
        // where placement+0x4c/+0x50 are the decoded depth/character id. Match
        // it against the outstanding factory batch by its unique depth.
        V3FactorySlot *slot = nullptr;
        if (g_v3_factory_active != 0 &&
            v3_read32(placement + 0x4c, depth) &&
            v3_read32(placement + 0x50, char_id))
        {
            // Match by depth+charId only: materialization is normally
            // SYNCHRONOUS (fires inside create_native_icon_instance's Execute,
            // before the issuer stores the returned node), so s.node may still
            // be 0 at capture time. Default slots (depth=UINT32_MAX, charId=0)
            // can never match a real placement.
            for (auto &s : g_v3_factory_slots)
                if (!s.child && s.depth == depth && s.char_id == char_id)
                {
                    slot = &s;
                    break;
                }
        }
        o_v3_place(vector_ptr, parent_ptr, placement_ptr, child_ptr, replace);
        if (slot)
        {
            const uintptr_t child = reinterpret_cast<uintptr_t>(child_ptr);
            uint32_t child_levels = 0, target_levels = 0;
            const uintptr_t child_root = v3_parent_root(child, child_levels);
            const uintptr_t target_parent = g_v3_target_parent.load(std::memory_order_acquire);
            const uintptr_t target_root = v3_parent_root(target_parent, target_levels);
            uint32_t refs_before = 0, refs_held = 0;
            float base_tx = 0.0f, base_ty = 0.0f;
            float basis[4] = {};
            const bool same_movie = v3_heap_ptr(child_root) && child_root == target_root;
            const bool held = same_movie && v3_hold_ref(child, refs_before, refs_held);
            const bool staged = held && v3_stage_source_child(child, base_tx, base_ty, basis);
            if (staged)
            {
                slot->child = child;
                slot->root = child_root;
                slot->base_tx = base_tx;
                slot->base_ty = base_ty;
                memcpy(slot->basis, basis, sizeof(basis));
                slot->held = true;
            }
            else
            {
                if (held)
                {
                    uint32_t before_drop = 0, after_drop = 0;
                    v3_drop_held_ref(child, before_drop, after_drop);
                }
                ++g_v3_native.failed;
                // The WALK DEPTH of each root matters, and it was missing from a player report
                // where every child was rejected on two different roots (Linux, DLC map). The
                // walk stops at 16 levels, so a tree deeper than that returns a mid-chain node
                // instead of the movie root, and "same movie" then compares two unrelated nodes.
                // A pair of levels reading 16/16 is that cap; anything less is a genuine mismatch.
                spdlog::warn("[v3native] deferred child rejected: child=0x{:X} "
                             "charId={} depth={} roots 0x{:X}(lv {})/0x{:X}(lv {}) held={} "
                             "staged={}",
                             child, char_id, depth, child_root, child_levels, target_root,
                             target_levels, held, staged);
                *slot = V3FactorySlot{};
                if (g_v3_factory_active)
                    --g_v3_factory_active;
            }
            return;
        }
        if (!custom || g_v3_custom_lock.test_and_set(std::memory_order_acquire))
            return;

        V3CustomSnapshot s{};
        s.vector = reinterpret_cast<uintptr_t>(vector_ptr);
        s.parent = reinterpret_cast<uintptr_t>(parent_ptr);
        s.placement = placement;
        s.child = reinterpret_cast<uintptr_t>(child_ptr);
        s.ret = ret;
        s.before_count = before_count;
        s.char_id = char_id;
        s.depth = depth;
        v3_read64(s.vector + 8, s.after_count);
        v3_read32(s.placement + 0x62, s.placement_flags);
        v3_read64(s.child, s.child_vt);
        v3_read64(s.child + 0x28, s.child_28);
        v3_read64(s.child + 0x30, s.child_30);
        v3_read64(s.child + 0x38, s.child_parent);
        v3_read32(s.child + 0x6a, s.child_flags);
        g_v3_custom = s;
        g_v3_custom_hits.fetch_add(1, std::memory_order_relaxed);
        g_v3_custom_lock.clear(std::memory_order_release);
        const uint32_t matrix_slot = depth - 24;
        float base_tx = 0.0f, base_ty = 0.0f;
        const bool staged = v3_stage_source_child(s.child, base_tx, base_ty);
        if (!staged)
        {
            spdlog::warn("[v3matrix] slot={} source staging failed; child not published",
                         matrix_slot);
            return;
        }
        g_v3_matrix_base_tx[matrix_slot] = base_tx;
        g_v3_matrix_base_ty[matrix_slot] = base_ty;
        g_v3_matrix_children[matrix_slot].store(s.child, std::memory_order_release);
        uint64_t no_start = 0;
        g_v3_matrix_started_ms.compare_exchange_strong(no_start, GetTickCount64(),
                                                       std::memory_order_acq_rel);
    }

    void v3_attach_detour(void *wrapper_ptr, void *child_ptr, uint32_t index)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        o_v3_attach(wrapper_ptr, child_ptr, index);
        // This detour serves TWO purposes and they must not share a gate. v3_note_movie_attach is the
        // ONLY writer of the marker anchor (g_v3_target_wrapper/parent/layer/count, and the same-parent
        // clear of g_v3_map_closed), which the whole native-marker path reads - seed, tick and viewport
        // reconcile all bail when the anchor is 0. It used to sit behind debug_logging, which was
        // survivable only while native markers were themselves dev-gated: with the mechanism now
        // compiled in as the shipping default, that gate meant a normal build anchored nothing and drew
        // NO markers at all. The other two calls really are diagnostics and stay behind the log key.
        if (!goblin::variants::kNativeMarkers && !goblin::config::debugLogging)
            return;

        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(wrapper_ptr);
        uint64_t parent = 0, count = 0;
        if (!v3_heap_ptr(wrapper) || !v3_read64(wrapper + 0x18, parent) ||
            !v3_heap_ptr(parent) || !v3_read64(static_cast<uintptr_t>(parent) + 0xe0, count))
            return;
        if (goblin::variants::kNativeMarkers)
            v3_note_movie_attach(wrapper, static_cast<uintptr_t>(parent), count);
        if (goblin::variants::kStallProfiler && goblin::config::debugLogging)
        {
            // Thousands of calls per map open - profiler-only, not general logging.
            v3_note_build_attach(wrapper, static_cast<uintptr_t>(parent), count);
            v3_record_candidate(wrapper, static_cast<uintptr_t>(parent), caller, count);
        }
    }

    void v3_try_link(V3ContainerSnapshot &s, uintptr_t root, uintptr_t source_off,
                     uintptr_t candidate, bool indirect)
    {
        if (s.link_count >= V3_LINK_CAP || !v3_heap_ptr(candidate))
            return;
        for (uint32_t i = 0; i < s.link_count; ++i)
            if (s.link[i].candidate == candidate)
                return;

        uint64_t list = 0, count = 0, first = 0, second = 0;
        uint64_t ctx_key = 0, node_key = 0;
        uint32_t depth = 0;
        if (!v3_read64(candidate + 0x28, list) ||
            !v3_read64(candidate + 0x30, count) ||
            !v3_heap_ptr(list) || count == 0 || count >= 20000 ||
            !v3_read64(static_cast<uintptr_t>(list), first) || !v3_heap_ptr(first))
            return;
        // A PO Execute list is a dense pointer array (stride 8). The +d8/+e0
        // container we started from has stride 0x10 and normally fails here.
        if (count > 1 &&
            (!v3_read64(static_cast<uintptr_t>(list) + 8, second) || !v3_heap_ptr(second)))
            return;
        if (!v3_read64(candidate, ctx_key) || !v3_read64(static_cast<uintptr_t>(first), node_key) ||
            ctx_key == 0 || ctx_key != node_key ||
            !v3_read32(static_cast<uintptr_t>(first) + 0x14, depth) || depth > 0xffff)
            return;

        V3LinkSnapshot &out = s.link[s.link_count++];
        out.root = root;
        out.source_off = source_off;
        out.candidate = candidate;
        out.list = static_cast<uintptr_t>(list);
        out.first_node = static_cast<uintptr_t>(first);
        out.count = count;
        out.shared_key = ctx_key;
        out.first_depth = depth;
        out.indirect = indirect;
    }

    void v3_scan_links(V3ContainerSnapshot &s, uintptr_t root)
    {
        if (!v3_heap_ptr(root))
            return;
        for (uintptr_t off = 0; off <= 0x300 && s.link_count < V3_LINK_CAP; off += 8)
        {
            // Some Scaleform helper/context structures are embedded in the
            // display object; others are reached through one object field.
            v3_try_link(s, root, off, root + off, false);
            uint64_t pointed = 0;
            if (v3_read64(root + off, pointed))
                v3_try_link(s, root, off, static_cast<uintptr_t>(pointed), true);
        }
    }

    void v3_note_container(void *object, uintptr_t ret)
    {
        uintptr_t parent = reinterpret_cast<uintptr_t>(object);
        if (parent < 0x10000 || parent >= 0x7fffffffffffULL)
            return;

        uint64_t entries64 = 0, count = 0;
        if (!v3_read64(parent + 0xd8, entries64) ||
            !v3_read64(parent + 0xe0, count) ||
            entries64 < 0x10000 || entries64 >= 0x7fffffffffffULL ||
            count == 0 || count > 100000)
            return;
        if (count <= g_v3_published_count.load(std::memory_order_relaxed))
            return;
        if (g_v3_snapshot_lock.test_and_set(std::memory_order_acquire))
            return;

        if (count > g_v3_published_count.load(std::memory_order_relaxed))
        {
            V3ContainerSnapshot s{};
            s.parent = parent;
            s.entries = static_cast<uintptr_t>(entries64);
            s.ret = ret;
            s.count = count;
            v3_read64(parent, s.parent_vt);
            v3_read64(parent + 0x28, s.parent_28);
            v3_read64(parent + 0x30, s.parent_30);
            v3_read64(parent + 0xb8, s.parent_b8);
            for (int q = 0; q < 3; ++q)
                v3_read64(parent + 0x130 + static_cast<uintptr_t>(q) * 8, s.parent_name[q]);

            const uint64_t indexes[3] = {0, count / 2, count - 1};
            for (int k = 0; k < 3; ++k)
            {
                uint64_t child64 = 0;
                v3_read64(s.entries + indexes[k] * 0x10, child64);
                s.child[k] = static_cast<uintptr_t>(child64);
                uint64_t aux64 = 0;
                v3_read64(s.entries + indexes[k] * 0x10 + 8, aux64);
                s.child_aux[k] = static_cast<uintptr_t>(aux64);
                if (s.child[k] < 0x10000 || s.child[k] >= 0x7fffffffffffULL)
                    continue;
                v3_read64(s.child[k], s.child_vt[k]);
                v3_read64(s.child[k] + 0x28, s.child_28[k]);
                v3_read64(s.child[k] + 0x30, s.child_30[k]);
                for (int q = 0; q < 3; ++q)
                    v3_read64(s.child[k] + 0x130 + static_cast<uintptr_t>(q) * 8,
                              s.child_name[k][q]);
            }
            if (count >= 1000)
            {
                v3_scan_links(s, s.parent);
                for (int k = 0; k < 3; ++k)
                {
                    v3_scan_links(s, s.child[k]);
                    v3_scan_links(s, s.child_aux[k]);
                }
            }
            g_v3_container = s;
            g_v3_published_count.store(count, std::memory_order_release);
        }
        g_v3_snapshot_lock.clear(std::memory_order_release);
    }

    // ── typed-find pointer-scan accelerator ("sequential predictor") ─────────
    // At the widget-b call site (ret = widget-b entry + 0x178) the engine does a
    // pointer-find over the map widget's entry array: an O(N) scan per pin makes
    // the register burst at open and the unregister burst at close O(N^2) -
    // ~280 ms each at ~9k markers (measured 2026-07-15). Both bursts visit pins
    // in container order, so the NEXT query's entry is almost always at the last
    // hit index + 1. We pre-seed the container's own result-cache slot
    // (container[4], which the engine REVALIDATES on every call by locking the
    // entry and comparing the object pointer - see FUN_14113feb0) with that
    // guess: a correct guess returns through the engine's own validated fast
    // path, a wrong one falls back to the normal scan. Either way only engine
    // code decides the result - the predictor cannot change semantics.
    uintptr_t g_wb_find_ret = 0;  // call-site gate; 0 = accelerator off
    uintptr_t g_pred_base = 0;    // container array base we are synced to
    size_t g_pred_idx = SIZE_MAX; // index of the last confirmed hit
    uintptr_t g_pred_guess = 0;   // entry we seeded this call
    size_t g_pred_guess_idx = 0;
    std::atomic<uint64_t> g_pred_hit{0}, g_pred_miss{0};
    std::atomic<uint64_t> g_pred_gate_seen{0}; // ret matched, before the other gate terms
                                               // (diagnoses the round-4 coverage anomaly)

    // ── close-wait job spy ───────────────────────────────────────────────────
    // FUN_140e811c0 = the generic "async job done?" poll (waits on job+0x10); the
    // map UI thread sits in it ~140ms after map close. Recording (caller ret, job
    // vtable) pairs during capture windows identifies WHICH job the close waits
    // on - its vtable RVA is the key to decompiling the job body.
    Fn4 *o_jobpoll = nullptr;
    struct JobSeen
    {
        std::atomic<uintptr_t> ret{0};
        std::atomic<uintptr_t> vt{0};
        std::atomic<uint32_t> cnt{0};
        std::atomic<uint64_t> ticks{0};    // total time INSIDE the poll (incl. blocking)
        std::atomic<uint64_t> max_ticks{0}; // worst single poll - names the freeze site
    };
    JobSeen g_jobs[24];

    void record_job(uintptr_t ret, uintptr_t vt, uint64_t ticks)
    {
        size_t h = ((ret >> 4) ^ (vt >> 4)) % 24;
        for (size_t i = 0; i < 24; ++i)
        {
            JobSeen &s = g_jobs[(h + i) % 24];
            uintptr_t cur = s.ret.load(std::memory_order_relaxed);
            if (cur == 0)
            {
                uintptr_t expected = 0;
                if (!s.ret.compare_exchange_strong(expected, ret, std::memory_order_relaxed))
                {
                    cur = expected;
                }
                else
                {
                    s.vt.store(vt, std::memory_order_relaxed);
                    cur = ret;
                }
            }
            if (cur == ret && s.vt.load(std::memory_order_relaxed) == vt)
            {
                s.cnt.fetch_add(1, std::memory_order_relaxed);
                s.ticks.fetch_add(ticks, std::memory_order_relaxed);
                uint64_t prev = s.max_ticks.load(std::memory_order_relaxed);
                while (ticks > prev &&
                       !s.max_ticks.compare_exchange_weak(prev, ticks, std::memory_order_relaxed))
                {
                }
                return;
            }
        }
    }

    void *jobpoll_detour(void *a, void *b, void *c, void *d)
    {
        if (!g_count.load(std::memory_order_relaxed) || !a)
            return o_jobpoll(a, b, c, d);
        uintptr_t obj = *reinterpret_cast<uintptr_t *>(a);
        uintptr_t vt = obj ? *reinterpret_cast<uintptr_t *>(obj) : 0;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void *r = o_jobpoll(a, b, c, d);
        QueryPerformanceCounter(&t1);
        record_job(reinterpret_cast<uintptr_t>(_ReturnAddress()), vt,
                   static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
        return r;
    }

    void pred_seed(uintptr_t *container)
    {
        g_pred_guess = 0;
        uintptr_t base = container[0];
        size_t cnt = container[1];
        if (!base || base != g_pred_base)
        {
            g_pred_base = base;
            g_pred_idx = SIZE_MAX; // new container: resync on the first result
            return;
        }
        size_t guess = g_pred_idx + 1; // SIZE_MAX + 1 wraps to 0 = first slot
        if (guess >= cnt) return;
        uintptr_t e = *reinterpret_cast<uintptr_t *>(base + guess * 0x10);
        // Same raw guards the engine's own scan applies before locking an entry.
        if (e && (*reinterpret_cast<uint8_t *>(e + 0x6b) & 1))
        {
            container[4] = e; // engine revalidates before trusting it
            g_pred_guess = e;
            g_pred_guess_idx = guess;
        }
    }

    void pred_update(uintptr_t *container, uintptr_t result)
    {
        if (!result)
        {
            g_pred_idx = SIZE_MAX;
            return;
        }
        if (g_pred_guess && result == g_pred_guess)
        {
            g_pred_idx = g_pred_guess_idx;
            g_pred_hit.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Miss: locate the result so the next guess is its neighbor (one O(N)
        // resync per miss keeps the burst ~O(N) overall).
        uintptr_t base = container[0];
        size_t cnt = container[1];
        g_pred_idx = SIZE_MAX;
        if (base == g_pred_base)
            for (size_t i = 0; i < cnt; ++i)
                if (*reinterpret_cast<uintptr_t *>(base + i * 0x10) == result)
                {
                    g_pred_idx = i;
                    break;
                }
        g_pred_miss.fetch_add(1, std::memory_order_relaxed);
    }

    void record_ret(int fn, uintptr_t ret)
    {
        size_t h = (ret >> 4) % RET_SLOTS;
        for (size_t i = 0; i < RET_SLOTS; ++i)
        {
            RetSlot &s = g_ret[fn][(h + i) % RET_SLOTS];
            uintptr_t cur = s.addr.load(std::memory_order_relaxed);
            if (cur == ret)
            {
                s.cnt.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (cur == 0)
            {
                uintptr_t expected = 0;
                if (s.addr.compare_exchange_strong(expected, ret, std::memory_order_relaxed))
                {
                    s.cnt.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (expected == ret)
                {
                    s.cnt.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        }
        // table full: drop (histogram is best-effort)
    }

    template <int I> void *reg_detour(void *a, void *b, void *c, void *d)
    {
        bool accel = false;
        if constexpr (I == 3) // typed-find: sequential predictor at the gated site
        {
            if (g_wb_find_ret &&
                reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_wb_find_ret)
            {
                g_pred_gate_seen.fetch_add(1, std::memory_order_relaxed);
                if (goblin::variants::kFastMapOpen && a)
                {
                    accel = true;
                    pred_seed(reinterpret_cast<uintptr_t *>(a));
                }
            }
        }
        if (!g_count.load(std::memory_order_relaxed))
        {
            void *r = o_reg[I](a, b, c, d);
            if constexpr (I == 3)
                if (accel)
                    pred_update(reinterpret_cast<uintptr_t *>(a),
                                reinterpret_cast<uintptr_t>(r));
            return r;
        }
        record_ret(I, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        if constexpr (I == 4)
            v3_note_container(a, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void *r = o_reg[I](a, b, c, d);
        QueryPerformanceCounter(&t1);
        if constexpr (I == 3)
            if (accel)
                pred_update(reinterpret_cast<uintptr_t *>(a), reinterpret_cast<uintptr_t>(r));
        uint64_t dt = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        g_reg_calls[I].fetch_add(1, std::memory_order_relaxed);
        g_reg_ticks[I].fetch_add(dt, std::memory_order_relaxed);
        uint64_t prev = g_reg_max[I].load(std::memory_order_relaxed);
        while (dt > prev &&
               !g_reg_max[I].compare_exchange_weak(prev, dt, std::memory_order_relaxed))
        {
        }
        return r;
    }

    void counters_reset()
    {
        g_v3_published_count.store(0, std::memory_order_relaxed);
        g_v3_container = {};
        for (int i = 0; i < N_REG; ++i)
        {
            g_reg_calls[i].store(0, std::memory_order_relaxed);
            g_reg_ticks[i].store(0, std::memory_order_relaxed);
            g_reg_max[i].store(0, std::memory_order_relaxed);
            for (auto &s : g_ret[i])
            {
                s.addr.store(0, std::memory_order_relaxed);
                s.cnt.store(0, std::memory_order_relaxed);
            }
        }
    }

    void counters_log(const std::string &tag, uintptr_t exe_base)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        for (int i = 0; i < N_REG; ++i)
        {
            uint64_t calls = g_reg_calls[i].load(std::memory_order_relaxed);
            if (!calls) continue;
            double ms = double(g_reg_ticks[i].load(std::memory_order_relaxed)) * 1000.0 /
                        double(f.QuadPart);
            double max_ms = double(g_reg_max[i].load(std::memory_order_relaxed)) * 1000.0 /
                            double(f.QuadPart);
            std::vector<std::pair<uint32_t, uintptr_t>> tops;
            for (auto &s : g_ret[i])
                if (s.addr.load(std::memory_order_relaxed))
                    tops.push_back({s.cnt.load(std::memory_order_relaxed),
                                    s.addr.load(std::memory_order_relaxed)});
            std::sort(tops.rbegin(), tops.rend());
            std::string line;
            for (size_t k = 0; k < tops.size() && k < 6; ++k)
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "exe+0x%llX x%u  ",
                         (unsigned long long)(tops[k].second - exe_base), tops[k].first);
                line += buf;
            }
            spdlog::info("[stallprobe] {}: {} = {} calls / {:.1f} ms (max {:.1f}); ret: {}",
                         tag, REG_NAME[i], calls, ms, max_ms, line);
        }

        uint64_t v3_count = g_v3_published_count.load(std::memory_order_acquire);
        if (v3_count)
        {
            const V3ContainerSnapshot &s = g_v3_container;
            spdlog::info("[v3tree] {}: busiest parent=0x{:X} entries=0x{:X} count={} ret=exe+0x{:X}",
                         tag, s.parent, s.entries, s.count,
                         s.ret >= exe_base ? s.ret - exe_base : s.ret);
            spdlog::info("[v3tree] parent vt=0x{:X} +28=0x{:X} +30=0x{:X} +b8=0x{:X} "
                         "name130={:016X} {:016X} {:016X}",
                         s.parent_vt, s.parent_28, s.parent_30, s.parent_b8,
                         s.parent_name[0], s.parent_name[1], s.parent_name[2]);
            static const char *const which[3] = {"first", "middle", "last"};
            for (int k = 0; k < 3; ++k)
                spdlog::info("[v3tree] {} child=0x{:X} aux=0x{:X} vt=0x{:X} +28=0x{:X} +30=0x{:X} "
                             "name130={:016X} {:016X} {:016X}",
                             which[k], s.child[k], s.child_aux[k], s.child_vt[k],
                             s.child_28[k], s.child_30[k],
                             s.child_name[k][0], s.child_name[k][1], s.child_name[k][2]);
            spdlog::info("[v3link] {}: {} PO-context candidate(s) under count={} container",
                         tag, s.link_count, s.count);
            for (uint32_t i = 0; i < s.link_count; ++i)
            {
                const V3LinkSnapshot &l = s.link[i];
                spdlog::info("[v3link] root=0x{:X} {}+0x{:X} -> ctx=0x{:X} "
                             "list=0x{:X} count={} first=0x{:X} depth={} key=0x{:X}",
                             l.root, l.indirect ? "ptr" : "embedded", l.source_off,
                             l.candidate, l.list, l.count, l.first_node,
                             l.first_depth, l.shared_key);
            }
        }

        const uint64_t add_calls = g_v3_add_calls.exchange(0, std::memory_order_relaxed);
        if (add_calls)
        {
            while (g_v3_add_lock.test_and_set(std::memory_order_acquire))
                _mm_pause();
            const V3AddSnapshot s = g_v3_add;
            g_v3_add = {};
            g_v3_add_max_count.store(0, std::memory_order_relaxed);
            g_v3_add_lock.clear(std::memory_order_release);
            spdlog::info("[v3add] {} since previous report: {} calls, grew={} same={} owner-match={}; "
                         "max {}->{} vector=0x{:X} parent=0x{:X} match={} ret=exe+0x{:X}",
                         tag, add_calls,
                         g_v3_add_grew.exchange(0, std::memory_order_relaxed),
                         g_v3_add_same.exchange(0, std::memory_order_relaxed),
                         g_v3_add_owner_match.exchange(0, std::memory_order_relaxed),
                         s.before_count, s.after_count, s.vector, s.parent, s.owner_match,
                         s.ret >= exe_base ? s.ret - exe_base : s.ret);
            spdlog::info("[v3add] insert-index={}; child=0x{:X} vt=0x{:X} child-depth={}",
                         s.insert_index,
                         s.child, s.child_vt, s.child_depth);
        }

        const uint64_t custom_hits = g_v3_custom_hits.exchange(0, std::memory_order_relaxed);
        if (custom_hits)
        {
            while (g_v3_custom_lock.test_and_set(std::memory_order_acquire))
                _mm_pause();
            const V3CustomSnapshot s = g_v3_custom;
            g_v3_custom = {};
            g_v3_custom_lock.clear(std::memory_order_release);
            spdlog::info("[v3custom] {}: {} hit(s), charId={} depth={} count {}->{} "
                         "vector=0x{:X} parent=0x{:X} ret=exe+0x{:X}",
                         tag, custom_hits, s.char_id, s.depth, s.before_count, s.after_count,
                         s.vector, s.parent, s.ret >= exe_base ? s.ret - exe_base : s.ret);
            spdlog::info("[v3custom] placement=0x{:X} flags=0x{:X}; child=0x{:X} vt=0x{:X} "
                         "+28=0x{:X} +30=0x{:X} child-parent=0x{:X} child-flags=0x{:X}",
                         s.placement, s.placement_flags, s.child, s.child_vt,
                         s.child_28, s.child_30, s.child_parent, s.child_flags);
        }

        if (g_v3_visual_published.exchange(false, std::memory_order_acq_rel))
        {
            const V3VisualSnapshot &s = g_v3_visual;
            spdlog::info("[v3visual] {}: engine attach wrapper=0x{:X} child=0x{:X} "
                         "old-parent=0x{:X} target-parent=0x{:X}",
                         tag, s.wrapper, s.child, s.old_parent, s.target_parent);
            spdlog::info("[v3visual] target {}->{} old {}->{} child-parent-after=0x{:X} success={}",
                         s.target_before, s.target_after, s.old_before, s.old_after,
                         s.child_parent_after,
                         s.target_after == s.target_before + 1 &&
                         s.old_after + 1 == s.old_before &&
                         s.child_parent_after == s.target_parent);
            spdlog::info("[v3visual] matrix translation ({:.3f}, {:.3f})->({:.3f}, {:.3f}) "
                         "positioned={}",
                         s.old_tx, s.old_ty, s.new_tx, s.new_ty, s.positioned);
            spdlog::info("[v3visual] fixed full-map midpoint=({:.3f}, {:.3f})",
                         s.map_mid_x, s.map_mid_y);
            spdlog::info("[v3visual] scale matrix [{:.5f} {:.5f}; {:.5f} {:.5f}] "
                         "reference-zoom={:.5f} native-parent-scale=test",
                         s.base_m0, s.base_m1, s.base_m4, s.base_m5, s.reference_zoom);
        }
    }

    struct ModRange
    {
        uintptr_t base = 0, end = 0;
    };

    ModRange module_range(const wchar_t *name)
    {
        HMODULE h = GetModuleHandleW(name);
        if (!h) return {};
        auto base = reinterpret_cast<uintptr_t>(h);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        return {base, base + nt->OptionalHeader.SizeOfImage};
    }

    // First executable section (.text) of a module. Frames are only accepted from
    // here: whole-image filtering let .data globals (e.g. exe+0x3D8xxxx singleton
    // pointers sitting on the stack) pollute the caller histograms in round 1.
    ModRange text_range(const wchar_t *name)
    {
        HMODULE h = GetModuleHandleW(name);
        if (!h) return {};
        auto base = reinterpret_cast<uintptr_t>(h);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        auto *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                return {base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        return {base, base + nt->OptionalHeader.SizeOfImage};
    }

    struct Sample
    {
        uintptr_t rip = 0;
        uintptr_t exe_frames[4] = {};
        int n_exe = 0;
    };

    void log_hist(const std::string &tag, const char *what,
                  const std::map<uintptr_t, int> &hist, uintptr_t exe_base, int total)
    {
        if (hist.empty() || !total) return;
        // top 10 by count
        std::vector<std::pair<uintptr_t, int>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        std::string line;
        int n = 0;
        for (auto &[addr, cnt] : v)
        {
            if (n++ >= 10) break;
            char buf[64];
            snprintf(buf, sizeof(buf), "exe+0x%llX x%d  ",
                     (unsigned long long)(addr - exe_base), cnt);
            line += buf;
        }
        spdlog::info("[stallprobe] {}: {} ({} samples): {}", tag, what, total, line);
    }

    void run_capture(std::string tag, DWORD tid, unsigned duration_ms)
    {
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION,
                               FALSE, tid);
        if (!th)
        {
            g_running.store(false);
            return;
        }
        ModRange exe = module_range(nullptr);
        ModRange ntdll = module_range(L"ntdll.dll");
        ModRange xtext = text_range(nullptr); // frames accepted from exe .text ONLY

        // All-thread sweep (every ~20ms): the target (map UI) thread turned out to
        // WAIT on a DLConditionSignal during the post-close stall while a worker
        // thread does the actual work - the sweep finds that worker. One thread is
        // paused at a time and only its context/stack is read while paused.
        struct SweepSample
        {
            DWORD tid;
            uintptr_t rip, frame0;
            bool active; // rip moved since this thread's previous sweep pass
        };
        std::vector<DWORD> sw_tids;
        std::vector<HANDLE> sw_handles;
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
                if (Thread32First(snap, &te)) do
                    {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self &&
                            sw_tids.size() < 256)
                        {
                            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                      THREAD_QUERY_INFORMATION,
                                                  FALSE, te.th32ThreadID);
                            if (h)
                            {
                                sw_tids.push_back(te.th32ThreadID);
                                sw_handles.push_back(h);
                            }
                        }
                    } while (Thread32Next(snap, &te));
                CloseHandle(snap);
            }
        }
        std::vector<SweepSample> sweeps;
        sweeps.reserve((duration_ms / 20 + 2) * (sw_tids.empty() ? 1 : sw_tids.size()));
        std::vector<uintptr_t> sw_last(sw_tids.size(), 0); // parked-thread filter

        counters_reset();
        g_count.store(true, std::memory_order_relaxed);

        std::vector<Sample> samples;
        samples.reserve(duration_ms + 64);

        // Scan a paused thread's stack for return addresses into the exe .text.
        auto scan_first_text_frame = [&](uintptr_t sp, uintptr_t *out, int max_out) -> int {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!sp || !VirtualQuery(reinterpret_cast<void *>(sp), &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                return 0;
            uintptr_t lim = std::min<uintptr_t>(
                reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, sp + 48 * 1024);
            int n = 0;
            for (uintptr_t p = sp; p + 8 <= lim && n < max_out; p += 8)
            {
                uintptr_t v = *reinterpret_cast<uintptr_t *>(p);
                if (v >= xtext.base && v < xtext.end) out[n++] = v;
            }
            return n;
        };

        ULONGLONG t0 = GetTickCount64();
        alignas(16) CONTEXT ctx;
        unsigned iter = 0;
        unsigned stall_run = 0; // consecutive samples with the target parked in ntdll
        while (GetTickCount64() - t0 < duration_ms)
        {
            if (SuspendThread(th) == (DWORD)-1) break;
            ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            Sample s{};
            bool ok = GetThreadContext(th, &ctx) != 0;
            if (ok)
            {
                s.rip = static_cast<uintptr_t>(ctx.Rip);
                s.n_exe = scan_first_text_frame(static_cast<uintptr_t>(ctx.Rsp), s.exe_frames, 4);
            }
            ResumeThread(th);
            if (ok) samples.push_back(s);

            // Adaptive burst: while the target sits in ntdll for >20ms straight (a
            // long wait = the stall we hunt), sweep every pass so the thread doing
            // the actual work during the stall is captured densely.
            bool stalled = ok && s.rip >= ntdll.base && s.rip < ntdll.end;
            stall_run = stalled ? stall_run + 1 : 0;

            if ((iter++ % 20) == 0 || (stall_run > 20 && sweeps.size() < 400000))
            {
                for (size_t k = 0; k < sw_handles.size(); ++k)
                {
                    if (SuspendThread(sw_handles[k]) == (DWORD)-1) continue;
                    ctx = {};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    SweepSample ss{sw_tids[k], 0, 0, false};
                    if (GetThreadContext(sw_handles[k], &ctx))
                    {
                        ss.rip = static_cast<uintptr_t>(ctx.Rip);
                        // A parked thread (blocked in a wait) reports the same rip
                        // every pass; only a MOVING rip means the thread works.
                        ss.active = (ss.rip != sw_last[k]);
                        sw_last[k] = ss.rip;
                        // Only active ntdll-time threads get the (pricier) stack scan.
                        if (ss.active && ss.rip >= ntdll.base && ss.rip < ntdll.end)
                        {
                            uintptr_t f = 0;
                            if (scan_first_text_frame(static_cast<uintptr_t>(ctx.Rsp), &f, 1))
                                ss.frame0 = f;
                        }
                    }
                    ResumeThread(sw_handles[k]);
                    if (ss.rip) sweeps.push_back(ss);
                }
            }
            Sleep(1);
        }
        CloseHandle(th);
        for (HANDLE h : sw_handles) CloseHandle(h);
        g_count.store(false, std::memory_order_relaxed);

        // Aggregate. rip location tells WHERE time went (exe vs ntdll = heap ops);
        // for non-exe rips the first stack frame into the exe is the caller we
        // need for a targeted patch. "other" rips are attributed by MODULE NAME
        // (GPU driver vs other mods vs D3D runtime decides the close-freeze story).
        auto module_of = [](uintptr_t rip) -> std::string {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void *>(rip), &mbi, sizeof(mbi)) ||
                !mbi.AllocationBase)
                return "<jit/unmapped>";
            char path[MAX_PATH] = {};
            if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), path,
                                    MAX_PATH))
                return "<jit/unmapped>";
            const char *base = strrchr(path, '\\');
            return base ? base + 1 : path;
        };
        int in_exe = 0, in_ntdll = 0, other = 0;
        std::map<uintptr_t, int> rip_exe, caller0, caller1;
        std::map<std::string, int> other_mods;
        for (const auto &s : samples)
        {
            if (s.rip >= exe.base && s.rip < exe.end)
            {
                in_exe++;
                rip_exe[s.rip]++;
            }
            else
            {
                if (s.rip >= ntdll.base && s.rip < ntdll.end)
                {
                    in_ntdll++;
                }
                else
                {
                    other++;
                    other_mods[module_of(s.rip)]++;
                }
                if (s.n_exe > 0) caller0[s.exe_frames[0]]++;
                if (s.n_exe > 1) caller1[s.exe_frames[1]]++;
            }
        }
        int total = static_cast<int>(samples.size());
        spdlog::info("[stallprobe] {}: {} samples over {} ms: exe {} / ntdll {} / other {}",
                     tag, total, duration_ms, in_exe, in_ntdll, other);
        if (!other_mods.empty())
        {
            std::vector<std::pair<int, std::string>> om;
            for (auto &[n, c] : other_mods) om.push_back({c, n});
            std::sort(om.rbegin(), om.rend());
            std::string line;
            for (size_t j = 0; j < om.size() && j < 6; ++j)
                line += om[j].second + " x" + std::to_string(om[j].first) + "; ";
            spdlog::info("[stallprobe] {}: other-module rip: {}", tag, line);
        }
        log_hist(tag, "exe rip hot spots", rip_exe, exe.base, in_exe);
        log_hist(tag, "ntdll-time exe callers (frame0)", caller0, exe.base, in_ntdll + other);
        log_hist(tag, "ntdll-time exe callers (frame1)", caller1, exe.base, in_ntdll + other);

        // Sweep result: which threads spent the window inside ntdll (heap/waits),
        // ranked; each with its top exe-.text callers. This is what identifies the
        // worker that performs the deferred post-close release work.
        {
            // Rank threads by ACTIVE samples only (moving rip): parked worker-pool
            // threads sit in ntdll waits with a frozen rip and are not work.
            std::map<DWORD, int> tid_act_ntdll, tid_act_exe, tid_act_other, tid_total;
            std::map<DWORD, std::map<uintptr_t, int>> tid_frames, tid_ntoff;
            std::map<std::string, int> sweep_other_mods;
            for (const auto &ss : sweeps)
            {
                tid_total[ss.tid]++;
                if (!ss.active) continue;
                if (ss.rip >= ntdll.base && ss.rip < ntdll.end)
                {
                    tid_act_ntdll[ss.tid]++;
                    tid_ntoff[ss.tid][ss.rip - ntdll.base]++;
                    if (ss.frame0) tid_frames[ss.tid][ss.frame0]++;
                }
                else if (ss.rip >= exe.base && ss.rip < exe.end)
                {
                    tid_act_exe[ss.tid]++;
                }
                else
                {
                    tid_act_other[ss.tid]++;
                    sweep_other_mods[module_of(ss.rip)]++;
                }
            }
            auto top_of = [](std::map<uintptr_t, int> &m, const char *pfx, uintptr_t rebase) {
                std::vector<std::pair<int, uintptr_t>> v;
                for (auto &[a, c] : m) v.push_back({c, a});
                std::sort(v.rbegin(), v.rend());
                std::string s;
                for (size_t j = 0; j < v.size() && j < 3; ++j)
                {
                    char b[48];
                    snprintf(b, sizeof(b), "%s+0x%llX x%d ", pfx,
                             (unsigned long long)(v[j].second - rebase), v[j].first);
                    s += b;
                }
                return s;
            };
            std::map<DWORD, int> tid_act_all;
            for (auto &[t, c] : tid_act_ntdll) tid_act_all[t] += c;
            for (auto &[t, c] : tid_act_exe) tid_act_all[t] += c;
            for (auto &[t, c] : tid_act_other) tid_act_all[t] += c;
            std::vector<std::pair<int, DWORD>> rank;
            for (auto &[t, c] : tid_act_all) rank.push_back({c, t});
            std::sort(rank.rbegin(), rank.rend());
            std::string line;
            int shown = 0;
            for (auto &[cnt, t] : rank)
            {
                if (shown++ >= 6) break;
                char buf[240];
                snprintf(buf, sizeof(buf),
                         "tid %lu act %d (nt %d exe %d oth %d) of %d [%s| %s]; ",
                         (unsigned long)t, cnt, tid_act_ntdll[t], tid_act_exe[t],
                         tid_act_other[t], tid_total[t],
                         top_of(tid_ntoff[t], "nt", 0).c_str(),
                         top_of(tid_frames[t], "exe", exe.base).c_str());
                line += buf;
            }
            spdlog::info("[stallprobe] {}: sweep {} threads (active only): {}", tag,
                         sw_tids.size(), line.empty() ? "no active threads" : line);
            if (!sweep_other_mods.empty())
            {
                std::vector<std::pair<int, std::string>> om;
                for (auto &[n, c] : sweep_other_mods) om.push_back({c, n});
                std::sort(om.rbegin(), om.rend());
                std::string ml;
                for (size_t j = 0; j < om.size() && j < 6; ++j)
                    ml += om[j].second + " x" + std::to_string(om[j].first) + "; ";
                spdlog::info("[stallprobe] {}: sweep other-module rip: {}", tag, ml);
            }
        }

        counters_log(tag, exe.base);
        // Sequential-predictor effectiveness (accumulated since the last window).
        {
            uint64_t ph = g_pred_hit.exchange(0, std::memory_order_relaxed);
            uint64_t pm = g_pred_miss.exchange(0, std::memory_order_relaxed);
            uint64_t gs = g_pred_gate_seen.exchange(0, std::memory_order_relaxed);
            if (ph + pm + gs)
                spdlog::info("[stallprobe] {}: find-predictor {} hits / {} misses / {} gate-seen",
                             tag, ph, pm, gs);
        }
        // Job-poll spy: which async jobs were waited on during this window and for
        // how long. The site with the big max is the one the close-freeze blocks in.
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            const double to_ms = 1000.0 / double(f.QuadPart);
            std::string line;
            for (auto &s : g_jobs)
            {
                uintptr_t r = s.ret.exchange(0, std::memory_order_relaxed);
                uintptr_t v = s.vt.exchange(0, std::memory_order_relaxed);
                uint32_t c = s.cnt.exchange(0, std::memory_order_relaxed);
                uint64_t tk = s.ticks.exchange(0, std::memory_order_relaxed);
                uint64_t mx = s.max_ticks.exchange(0, std::memory_order_relaxed);
                if (!r || !c) continue;
                // Only report sites that actually spent time (>0.5ms total) or ran
                // often - keeps the line readable.
                double total_ms = double(tk) * to_ms, max_ms = double(mx) * to_ms;
                if (total_ms < 0.5 && c < 50) continue;
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "ret exe+0x%llX vt exe+0x%llX x%u %.1fms (max %.1f); ",
                         (unsigned long long)(r - exe.base),
                         (unsigned long long)(v ? v - exe.base : 0), c, total_ms, max_ms);
                line += buf;
            }
            if (!line.empty())
                spdlog::info("[stallprobe] {}: job-poll: {}", tag, line);
        }
        g_running.store(false);
    }
} // namespace

// Rows of our native settings page (see goblin_inject.hpp). ini_key doubles as
// the i18n entry_labels key; goblin_messages injects the localized label into
// GR_MenuText at startup, build_our_rows binds the checkbox to the bool.
// Proof set for now - grows as the overlay settings migrate over.
const goblin::NativeMenuRowDef *goblin::native_menu_rows(size_t *count)
{
    // The "general" (non-category) settings, 16 rows = exactly the page list cap
    // (BasicViewItemList<EditProperty,16>). The ~45 show_* category toggles need
    // their own tabs/pages (multi-page design) - next phase. ERR-only entries are
    // harmless no-ops on other profiles (their config vars exist everywhere).
    // Live-loot defaults differ per profile (vanilla=true, others=false) - use the
    // compiled-in initial value of the vars themselves as the schema default.
    static goblin::NativeMenuRowDef rows[] = {
        {"require_map_fragments", &goblin::config::requireMapFragments, true},
        {"show_world_maps_ignore_fragments", &goblin::config::worldMapsIgnoreFragments, true},
        {"hide_killed_bosses", &goblin::config::hideKilledBosses, false},
        {"hide_dungeon_icons_on_clear", &goblin::config::hideDungeonIconsOnClear, false},
        {"patch_overworld_boss_icons", &goblin::config::patchOverworldBossIcons, true},
        {"patch_dungeon_boss_icons", &goblin::config::patchDungeonBossIcons, true},
        {"patch_camp_icons", &goblin::config::patchCampIcons, true},
        {"patch_merchant_icons", &goblin::config::patchMerchantIcons, true},
#ifdef MFG_PROFILE_VANILLA // matches the schema's MFG_LL_DEF (vanilla bake only)
        {"live_loot_flags", &goblin::config::liveLootFlags, true},
        {"live_loot_labels", &goblin::config::liveLootLabels, true},
        {"live_loot_icons", &goblin::config::liveLootIcons, true},
#else
        {"live_loot_flags", &goblin::config::liveLootFlags, false},
        {"live_loot_labels", &goblin::config::liveLootLabels, false},
        {"live_loot_icons", &goblin::config::liveLootIcons, false},
#endif
        {"anonymous_loot", &goblin::config::anonymousLoot, false},
        {"show_bosses", &goblin::config::showBosses, true},
    };
    *count = sizeof(rows) / sizeof(rows[0]);
    return rows;
}

void goblin::stall_probe::v3_pin_build_begin(void *owner, void *ctx)
{
    if (!goblin::config::debugLogging)
        return;
    auto &corr = g_v3_build_corr;
    if (corr.depth == 0)
    {
        corr = {};
        corr.owner = owner;
        corr.ctx = ctx;
    }
    ++corr.depth;
}

void goblin::stall_probe::v3_pin_build_end()
{
    if (!goblin::config::debugLogging)
        return;
    auto &corr = g_v3_build_corr;
    if (corr.depth == 0 || --corr.depth != 0)
        return;

    std::sort(corr.parents, corr.parents + corr.parent_count,
              [](const V3BuildParentStat &a, const V3BuildParentStat &b)
              {
                  if (a.calls != b.calls) return a.calls > b.calls;
                  return a.max_count > b.max_count;
              });
    spdlog::info("[v3corr] native buildMarkers owner=0x{:X} ctx=0x{:X}: "
                 "{} attachMovie call(s), {} parent(s)",
                 reinterpret_cast<uintptr_t>(corr.owner),
                 reinterpret_cast<uintptr_t>(corr.ctx),
                 corr.total_calls, corr.parent_count);
    const uint32_t shown = std::min<uint32_t>(corr.parent_count, 8);
    for (uint32_t i = 0; i < shown; ++i)
    {
        const auto &s = corr.parents[i];
        spdlog::info("[v3corr] rank={} parent=0x{:X} wrapper=0x{:X} calls={} "
                     "countRange={}..{}",
                     i, s.parent, s.wrapper, s.calls,
                     s.min_count == UINT64_MAX ? 0 : s.min_count, s.max_count);
    }
    corr = {};
}

void goblin::stall_probe::on_map_frame()
{
    // Drive the category-by-category lightweight native-marker rollout.
    v3_native_tick();

    // Lever B: reconcile which markers are attached to the visible map window.
    v3_viewport_reconcile();

    if (!g_v3_scale_ready.load(std::memory_order_acquire))
        return;

    const V3ScaleState state = g_v3_scale;
    goblin::mapproject::MapView view{};
    if (!(state.reference_zoom > 0.01f) || !goblin::mapproject::read_view(view))
        return;

    uint64_t parent = 0, vt = 0, get_addr = 0, set_addr = 0;
    if (!v3_read64(state.child + 0x38, parent) || parent != state.target_parent ||
        !v3_read64(state.child, vt) ||
        !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
        !v3_read64(static_cast<uintptr_t>(vt) + 0x18, set_addr) ||
        !v3_heap_ptr(get_addr) || !v3_heap_ptr(set_addr))
        return;

    // The background container's apparent scale follows MapView.zoom. Applying
    // reference/current to the child's authored 2x2 basis cancels that zoom while
    // leaving its twip translation (world position) untouched.
    float factor = state.reference_zoom / view.zoom;
    factor = std::clamp(factor, 0.05f, 20.0f);
    using GetMatrixFn = const float *(void *);
    using SetMatrixFn = void(void *, const float *);
    float matrix[8]{};
    __try
    {
        auto *get_matrix = reinterpret_cast<GetMatrixFn *>(get_addr);
        auto *set_matrix = reinterpret_cast<SetMatrixFn *>(set_addr);
        const float *current = get_matrix(reinterpret_cast<void *>(state.child));
        memcpy(matrix, current, sizeof(matrix));
        matrix[0] = state.base_m0 * factor;
        matrix[1] = state.base_m1 * factor;
        matrix[4] = state.base_m4 * factor;
        matrix[5] = state.base_m5 * factor;
        matrix[3] = state.target_tx + state.pivot_tx * factor;
        matrix[7] = state.target_ty + state.pivot_ty * factor;
        set_matrix(reinterpret_cast<void *>(state.child), matrix);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Diagnostic experiment only: leave the last good transform in place.
    }
}


void goblin::stall_probe::v3_native_factory_pulse(void *ctx, unsigned frame)
{
    try
    {
        v3_note_thread("v3_native_factory_pulse (RM2 burst)");
        v3_check_owner("v3_native_factory_pulse");
        if (!v3_native_seed_from_live_callback()) return;
        v3_native_factory_consume(ctx, static_cast<uint32_t>(frame));
    }
    catch (const std::exception &e)
    {
        g_v3_native.frame_budget = 0;
        g_v3_native.in_factory = false;
        spdlog::error("[v3native] factory exception: {}", e.what());
    }
}

void goblin::stall_probe::on_map_close()
{
    // Deliberately KEEP g_v3_native and the g_v3_target_* anchors: a quick
    // reopen (before the engine's deferred teardown) reuses the same movie and
    // never emits a new attachMovie burst, so a reset here would orphan our
    // still-alive children with stale visibility. After a REAL teardown the
    // next build burst re-targets a fresh parent (parent-changed arm, gated on
    // this flag) and the map-frame tick resets and reseeds the manager; until
    // then every touch is liveness-validated and SEH-guarded.
    g_v3_close_ms.store(GetTickCount64(), std::memory_order_release);
    g_v3_map_closed.store(true, std::memory_order_release);
    g_solidfill_spike_done.store(0, std::memory_order_relaxed); // re-fire the spike on the next real build
    g_v3_scale_ready.store(false, std::memory_order_release);
    g_v3_matrix_state.store(0, std::memory_order_release);
    g_v3_matrix_started_ms.store(0, std::memory_order_release);
    for (auto &child : g_v3_matrix_children)
        child.store(0, std::memory_order_release);
    memset(g_v3_matrix_base_tx, 0, sizeof(g_v3_matrix_base_tx));
    memset(g_v3_matrix_base_ty, 0, sizeof(g_v3_matrix_base_ty));
    while (g_v3_candidate_lock.test_and_set(std::memory_order_acquire))
        YieldProcessor();
    g_v3_candidate_count = 0;
    memset(g_v3_candidates, 0, sizeof(g_v3_candidates));
    g_v3_candidate_lock.clear(std::memory_order_release);
    g_v3_custom_child.store(0, std::memory_order_release);
    g_v3_visual_state.store(0, std::memory_order_release);
}

uint32_t goblin::stall_probe::v3_detach_all_children()
{
    if (!goblin::variants::kSelfDetach || !g_v3_remove_at)
        return 0;
    const uintptr_t wrapper = g_v3_native.wrapper;
    const uintptr_t parent = g_v3_native.parent;
    if (!v3_heap_ptr(wrapper) || !v3_heap_ptr(parent) || g_v3_native.objects.empty())
        return 0;

    // Snapshot our live child pointers, sorted+unique for the leaf's binary search.
    // Runs on the map UI thread from the WMD dtor detour, single-shot per close.
    static std::vector<uintptr_t> ours;
    ours.clear();
    ours.reserve(g_v3_native.objects.size());
    for (const auto &o : g_v3_native.objects)
        if (v3_heap_ptr(o.child))
            ours.push_back(o.child);
    if (ours.empty())
        return 0;
    std::sort(ours.begin(), ours.end());
    ours.erase(std::unique(ours.begin(), ours.end()), ours.end());

    static std::vector<uint32_t> idxbuf;
    idxbuf.assign(ours.size(), 0);

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    uint32_t faulted = 0;
    const uint32_t found = v3_detach_scan(parent, ours.data(),
                                          static_cast<uint32_t>(ours.size()),
                                          idxbuf.data(),
                                          static_cast<uint32_t>(idxbuf.size()), &faulted);
    if (faulted)
        spdlog::warn("[v3native] detach-all: child-vector scan faulted, removing nothing "
                     "(indices from a vector that moved would free engine-owned children).");
    uint32_t skipped = 0;
    const uint32_t removed = v3_detach_remove(wrapper, parent, idxbuf.data(), found, ours.data(),
                                              static_cast<uint32_t>(ours.size()), &skipped);
    if (skipped)
        spdlog::warn("[v3native] detach-all: {} of {} indices no longer pointed at our children and "
                     "were SKIPPED (vector moved between scan and removal).", skipped, found);
    QueryPerformanceCounter(&t1);
    const uint64_t us = freq.QuadPart
        ? static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart)
        : 0;
    g_v3_last_detached.store(removed, std::memory_order_relaxed);
    g_v3_last_detach_us.store(us, std::memory_order_relaxed);
    spdlog::info("[v3native] self-detach: matched={} removed={} tracked={} in {} us",
                 found, removed, g_v3_native.objects.size(), us);

    // The children were bulk-DETACHED (their TreeCacheNodes are gone, which is what pays for the
    // close freeze), but with the viewport window on they carry our retained reference, so the
    // child OBJECTS stay alive while detached. Keep them tracked, marked detached, so a QUICK
    // reopen - same movie and parent reused, no rebuild burst - re-attaches the in-view ones
    // through v3_viewport_reconcile instead of showing an empty map. A REAL teardown changes the
    // parent, and the reset arm in v3_native_tick drops the tracking there.
    // Without that retained reference the detach really did free them, so clearing is correct.
    if (goblin::variants::kViewportWindow)
    {
        for (auto &o : g_v3_native.objects)
            o.attached = false;
    }
    else
    {
        g_v3_native.objects.clear();
        g_v3_native.by_row.clear();
        g_v3_native.queued.clear();
    }
    return removed;
}

void goblin::stall_probe::setup()
{
    // AOB-resolved entries (v1.16 file VAs in the comment above). Each pattern was
    // verified unique in the exe; a miss disables just that counter (feature is
    // diagnostics-only, the game runs unchanged).
    struct Target
    {
        int idx;
        const char *aob;
    };
    const Target targets[] = {
        {0, "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC "
            "A0 00 00 00 48 8B"},
        {1, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D "
            "6C 24 D1 48 81 EC 90 00 00 00 48 8B 41 08"},
        {2, "48 89 5C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC"},
        {3, "40 53 41 55 41 57 48 83 EC 30 33 DB 4C 8B F9 89 5C 24 58 4C 8B EA 48 8B"},
        {4, "48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 20 8B A9 B8 00 00 00 4C 8B F1 "
            "48 8B B1 E0 00 00 00 C1 ED 03 40 80 E5 01 48"},
        {5, "48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 51 50 48 85 D2 0F"},
        {6, "40 53 56 57 48 83 EC 20 48 8B F9 83 CA FF 48 81 C1 88 00 00 00"},
        {7, "48 89 5C 24 20 57 41 56 41 57 48 83 EC 20 48 8B 19 4D 8B F8 4C 8B F2 48"},
        {8, "48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 80 00 00 00 48 8B"},
        {9, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 48 8B F2 "
            "E8 01 C4 FE FF 83"},
    };
    static Fn4 *detours[N_REG] = {reg_detour<0>, reg_detour<1>, reg_detour<2>,
                                  reg_detour<3>, reg_detour<4>, reg_detour<5>,
                                  reg_detour<6>, reg_detour<7>, reg_detour<8>,
                                  reg_detour<9>};
    int armed = 0;
    for (const auto &t : targets)
    {
        try
        {
            auto *fn = modutils::hook<Fn4>({.aob = t.aob}, *detours[t.idx], o_reg[t.idx]);
            // widget-b: its typed-find call site (entry+0x173, E8 rel32) returns to
            // entry+0x178 - the gate for the sequential predictor above.
            if (t.idx == 1)
                g_wb_find_ret = reinterpret_cast<uintptr_t>(fn) + 0x178;
            armed++;
        }
        catch (const std::exception &e)
        {
            spdlog::warn("[stallprobe] counter '{}' unavailable: {}", REG_NAME[t.idx], e.what());
        }
    }
    spdlog::info("[stallprobe] {} of {} cost counters armed", armed, N_REG);

    // Scaleform DisplayObjContainer native insert core
    // (v1.16 FUN_14113e970). Pass-through at all times; debug sessions retain
    // counters and the highest-count call until the next capture report.
    try
    {
        modutils::hook<V3AddFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 "
                    "48 C7 44 24 28 FF FF FF FF 49 8B D9 48 89 5C 24 20 49 8B F0 48 8B EA 48 8B F9"},
            v3_add_detour, o_v3_add);
        spdlog::info("[stallprobe] v3 native insert-core spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 native insert-core spy unavailable: {}", e.what());
    }

    // PlaceObject insertion path (v1.16 FUN_14113e7e0). The detour filters to
    // MAP_ICON_CHARID_BASE, so ordinary display-tree construction is untouched
    // apart from the pass-through hook itself.
    try
    {
        modutils::hook<V3PlaceFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
                    "41 54 41 56 41 57 48 83 EC 20 45 8B 70 4C 4C 8B FA 4C 8B 61 08 "
                    "41 8B D6 49 8B D9 49 8B E8 48 8B F9 E8"},
            v3_place_detour, o_v3_place);
        spdlog::info("[stallprobe] v3 custom PlaceObject spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 custom PlaceObject spy unavailable: {}", e.what());
    }

    // Record-materialization driver (v2.6.2.0 FUN_1411bf1b0): the engine-side
    // walker that turns pending timeline records into real children. Called by
    // the factory right after queueing a batch; a miss disables native marker
    // creation entirely (markers then fall back to the finite build-burst
    // trickle, ~1900/session).
    try
    {
        g_v3_mat_driver = reinterpret_cast<V3MatDriverFn *>(modutils::scan<void>(
            {.aob = "4C 8B DC 55 56 41 55 49 8D 6B D8 48 81 EC 10 01 00 00 "
                    "48 8D 41 48 4C 8B EA"}));
        spdlog::info("[stallprobe] v3 record driver resolved @ 0x{:X}",
                     reinterpret_cast<uintptr_t>(g_v3_mat_driver));
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 record driver AOB miss (native marker "
                     "creation limited): {}",
                     e.what());
    }

    // Lever C self-detach: the engine's remove-from-container primitive
    // FUN_1410c87c0 (the removal half of the reparent path FUN_1410c8440). A miss
    // just disables self-detach (close falls back to the engine's full teardown).
    try
    {
        g_v3_remove_at = reinterpret_cast<V3RemoveAtFn *>(modutils::scan<void>(
            {.aob = "40 57 48 83 EC 20 48 8B 41 18 48 8B F9 3B 90 E0 00 00 00 72 08 "
                    "33 C0 48 83 C4 20 5F C3"}));
        spdlog::info("[stallprobe] v3 self-detach primitive resolved: removeAt=0x{:X}",
                     reinterpret_cast<uintptr_t>(g_v3_remove_at));
    }
    catch (const std::exception &e)
    {
        g_v3_remove_at = nullptr;
        spdlog::warn("[stallprobe] v3 self-detach primitive AOB miss (lever C off): {}",
                     e.what());
    }

    // Solid-fill spike DrawingContext primitives (dev-only, gated on debug_logging).
    // A miss just disables the spike; nothing in the shipping path depends on these.
    // endFill's prologue carries a relative CALL - mask its displacement so the AOB
    // survives minor codegen shifts. (beginFill = the RE'd solid-color entry cc20.)
    try
    {
        g_dc_begin = reinterpret_cast<DcBeginFn *>(modutils::scan<void>(
            {.aob = "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 0F B6 FA 48 8B 49 38 48 85 C9 0F 84"}));
        g_dc_beginfill = reinterpret_cast<DcBeginFillFn *>(modutils::scan<void>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 8B DA "
                    "F6 81 D0 00 00 00 10 48"}));
        g_dc_moveto = reinterpret_cast<DcMoveToFn *>(modutils::scan<void>(
            {.aob = "40 53 48 81 EC 80 00 00 00 33 C0 0F 29 74 24 70 0F 57 C0 48 89 44 24 3C "
                    "F3 0F 7F 44 24 24"}));
        g_dc_lineto = reinterpret_cast<DcLineToFn *>(modutils::scan<void>(
            {.aob = "40 53 48 83 EC 40 F6 81 D0 00 00 00 08 48 8B D9 0F 29 74 24 30 0F 28 F2 "
                    "0F 29 7C 24 20 0F 28 F9"}));
        g_dc_endfill = reinterpret_cast<DcEndFillFn *>(modutils::scan<void>(
            {.aob = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 C0 C7 83 CC 00 00 00 00 00 "
                    "80 00 48 8B CB 48 89 43"}));
        g_dc_shapereset = reinterpret_cast<DcShapeResetFn *>(modutils::scan<void>(
            {.aob = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 28 48 8B D9 45 33 C0 "
                    "BA 80 00 00 00 48 8B"}));
        if (!g_dc_begin || !g_dc_beginfill || !g_dc_moveto || !g_dc_lineto ||
            !g_dc_endfill || !g_dc_shapereset)
        {
            g_dc_beginfill = nullptr; // arm gate: all-or-nothing
            spdlog::warn("[solidfill] one or more DrawingContext primitives unresolved; spike off");
        }
        else
        {
            spdlog::info("[solidfill] DrawingContext primitives resolved: begin=0x{:X} "
                         "beginFill=0x{:X} endFill=0x{:X}",
                         reinterpret_cast<uintptr_t>(g_dc_begin),
                         reinterpret_cast<uintptr_t>(g_dc_beginfill),
                         reinterpret_cast<uintptr_t>(g_dc_endfill));
        }
    }
    catch (const std::exception &e)
    {
        g_dc_beginfill = nullptr;
        spdlog::warn("[solidfill] DrawingContext primitive AOB miss (spike off): {}", e.what());
    }

    // The list's row-path helper: its argument pair is the slot about to be drawn, which is how
    // row icons find their clip. A miss only costs icons.
    try
    {
        modutils::hook<RowPathFn>(
            {.address = reinterpret_cast<void *>(
                 reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x736FC0)},
            row_path_detour, o_row_path);
        spdlog::info("[menuicons] row-slot hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[menuicons] row-slot hook unavailable: {}", e.what());
    }

    // Input-action predicate hook: names the action ESC produces. Inert unless armed (see
    // action_test_detour); a miss only costs that diagnostic.
    // Diagnostic only, and it sits in a path called several times per frame - so it is installed
    // ONLY when debug logging is on. Its finding is already recorded in the docs; it stays because the
    // same instrument answers "which action is this?" for any future menu work.
    try
    {
        if (!goblin::config::debugLogging)
            throw std::runtime_error("debug logging off - action probe not installed");
        modutils::hook<ActionTestFn>(
            {.address = reinterpret_cast<void *>(
                 reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x758500)},
            action_test_detour, o_action_test);
        spdlog::info("[action] input-action predicate hook armed (inert until ESC)");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[action] input-action hook unavailable: {}", e.what());
    }

    // Load-time movie interception (see goblin_own_movie.hpp). Independent of everything
    // else: if it fails to arm, the screen simply loads the stock movie.
    goblin::own_movie::install();

    // Menu-movie graphics probe hook: CSMenuMan::updateTask (v2.6.x 0x766980). The
    // detour reads the active menu and (dev-only) tests solid-fill rendering on menu
    // sprites. A miss just disables the probe. This is also the UI-thread beachhead
    // the future native announce/dialogs will reuse.
    try
    {
        modutils::hook<MenuUpdateFn>(
            {.aob = "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 "
                    "48 81 EC 98 00 00 00 48 C7 45 A7 FE FF FF FF 0F 29 70 A8 "
                    "0F 29 78 98 44 0F 29 40 88 44 0F 29 4C 24 50 44 0F 29 54 24 40 "
                    "48 8B FA 48 8B D9 0F 57 FF F3 0F 10 05 ?? ?? ?? ?? 0F 2E C7 "
                    "7A 16 75 14 B9 29 0A 00 00"},
            menu_update_detour, o_menu_update);
        spdlog::info("[menuprobe] CSMenuMan::updateTask hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[menuprobe] updateTask hook unavailable: {}", e.what());
    }

    // Native settings-menu tab injection Proto 0: hook OptionSettingTopDialog ctor
    // (0x966120) to observe the live tab list (read-only, dev-only). A miss just
    // disables the observer.
    try
    {
        modutils::hook<OptTopCtorFn>(
            {.aob = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C8 F5 FF FF "
                    "48 81 EC 38 0B 00 00 48 C7 45 A0 FE FF FF FF"},
            opttop_ctor_detour, o_opttop_ctor);
        spdlog::info("[optmenu] OptionSettingTopDialog ctor hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[optmenu] ctor hook unavailable: {}", e.what());
    }

    // OptionSettingTopDialog dtor hook (0x966980): clears our anti-restack tracker on
    // close. A miss just means F11 could re-stack (the tracker never clears).
    try
    {
        modutils::hook<OptTopDtorFn>(
            {.aob = "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 "
                    "48 89 74 24 58 48 8B F1 48 8D 05 ?? ?? ?? ?? 48 89 01 80 B9 98 18 00 00 00 "
                    "74 73 E8 ?? ?? ?? ?? 90 48 8B F8 48 8D 8E 68 17 00 00"},
            opttop_dtor_detour, o_opttop_dtor);
        spdlog::info("[optmenu] OptionSettingTopDialog dtor hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[optmenu] dtor hook unavailable: {}", e.what());
    }

    // Tab-inject primitives: build a settings-tab category / destruct temp, and HOOK
    // the tab-append so we inject our tab during the ctor. A miss just disables inject.
    try
    {
        p_build_cat = reinterpret_cast<BuildCatFn *>(modutils::scan<void>(
            {.aob = "4C 8B DC 49 89 4B 08 57 48 81 EC B0 00 00 00 48 C7 44 24 28 FE FF FF FF "
                    "49 89 5B 18 48 8B F9 C7 44 24 20 00 00 00 00 49 8D 43 C0 49 89 43 10 "
                    "BA B0 AD 01 00 49 8D 4B C0 E8 ?? ?? ?? ?? 48 8B D8 8B 15 ?? ?? ?? ?? "
                    "83 C2 14 89 54 24 30 0F 57 C0"})); // mov edx,110000 + icon calc = unique
        p_dtor_cat = reinterpret_cast<DtorCatFn *>(modutils::scan<void>(
            {.aob = "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 "
                    "48 8B F9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 41 08 48 89 44 24 48 "
                    "48 8D 59 10 48 89 5C 24 48 48 83 7B 20 08 72 0E"}));
        modutils::hook<AppendTabFn>(
            {.aob = "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B FA "
                    "48 8B D9 48 8B 81 58 05 00 00 48 FF C0 48 83 F8 0A"},
            append_tab_detour, o_append_tab);
        if (p_build_cat && p_dtor_cat && o_append_tab)
            spdlog::info("[optmenu] tab-inject primitives resolved + append hook armed");
        else
        {
            p_build_cat = nullptr;
            spdlog::warn("[optmenu] tab-inject primitives incomplete; inject off");
        }
    }
    catch (const std::exception &e)
    {
        p_build_cat = nullptr;
        spdlog::warn("[optmenu] tab-append primitive AOB miss: {}", e.what());
    }

    // Our-page hooks: the show-page dispatch (FUN_14093b760 - MUST be intercepted for
    // our out-of-range category id, its 10-slot page cache is indexed unchecked) and
    // the borrowed page's populate (FUN_140957ef0 - swapped to our rows when armed).
    // Both are cold options-UI functions (hooking is safe). A miss just means our tab
    // opens no page.
    try
    {
        modutils::hook<ShowPageFn>(
            {.aob = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 99 B8 00 00 00 "
                    "48 8B F1 48 63 C2 48 8B 44 C1 68 48 85 C0 75"},
            show_page_detour, o_show_page);
        modutils::hook<PopulatePageFn>(
            {.aob = "40 55 56 57 48 8D AC 24 50 FE FF FF 48 81 EC B0 02 00 00 "
                    "48 C7 44 24 70 FE FF FF FF 48 89 9C 24 E0 02 00 00 48 8B 05 ?? ?? ?? ?? "
                    "48 33 C4 48 89 85 A0 01 00 00 48 8B FA 48 8B D9 48 8D 4D F0"},
            populate_page_detour, o_populate_page);
        // (The 02_042 graphics-screen populate hook went with the F6 host on 2026-07-28 - one less patch
        //  in the game. See docs/research_f6_graphics_host_retired.md.)
        if (o_show_page && o_populate_page)
            spdlog::info("[optmenu] our-page hooks armed (show-page + populate)");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[optmenu] our-page hook AOB miss: {}", e.what());
    }

    // Keybinding-form row hooks (dev prototype, keysetting_*_re.txt): the row-vector
    // builder FUN_140868590 and the row VALUE provider FUN_140867de0. Both are cold
    // options-UI functions and both pass straight through unless our form is armed, so
    // the game's own keybinding screen is untouched. Hooked by address: their prologues
    // are short/shared, so an AOB would not be unique.
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        modutils::hook(reinterpret_cast<void *>(base + 0x868590),
                       reinterpret_cast<void *>(&build_items_detour),
                       reinterpret_cast<void **>(&o_build_items));
        // Row DRAW (the item's vt+0x8): we paint name/value ourselves, so our rows are
        // not limited to FMG strings.
        modutils::hook(reinterpret_cast<void *>(base + 0x8674e0),
                       reinterpret_cast<void *>(&row_render_detour),
                       reinterpret_cast<void **>(&o_row_render));
        modutils::hook(reinterpret_cast<void *>(base + 0x9411a0),
                       reinterpret_cast<void *>(&form_decide_detour),
                       reinterpret_cast<void **>(&o_form_decide));
        // The dialog's per-frame update, used only as a liveness heartbeat.
        modutils::hook(reinterpret_cast<void *>(base + 0x93F540),
                       reinterpret_cast<void *>(&form_update_detour),
                       reinterpret_cast<void **>(&o_form_update));
        if (o_build_items && o_row_render && o_form_decide)
            spdlog::info("[form] keybinding-form row hooks armed (build + draw + decide)");
        else
            spdlog::warn("[form] keybinding-form row hooks incomplete; row swap off");
    }

    // Standalone settings-open (F11) primitives: build-job / ref-move / push-job. A miss
    // just disables F11 open. push-job/ref-move reuse the confirm-dialog machinery.
    try
    {
        p_build_job = reinterpret_cast<BuildJobFn *>(modutils::scan<void>(
            {.aob = "48 8B C4 55 57 41 56 48 8D 68 A1 48 81 EC C0 00 00 00 48 C7 45 D7 FE FF FF FF "
                    "48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 41 0F B6 F8 "
                    "48 8B F2"}));
        p_refmove = reinterpret_cast<RefMoveFn *>(modutils::scan<void>(
            {.aob = "48 89 54 24 10 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B DA "
                    "C7 44 24 20 00 00 00 00 48 8B 09 48 89 0A 48 85 C9 74"}));
        p_push_job = reinterpret_cast<PushJobFn *>(modutils::scan<void>(
            {.aob = "4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC A0 00 00 00 "
                    "48 C7 44 24 40 FE FF FF FF 48 89 9C 24 D0 00 00"}));
        // CSFeManImp singleton slot: the store at the tail of its init. Unique in the exe and it
        // resolves to the same 0x3D6B880 the live process uses; a miss only disables the HUD-mode
        // restore.
        g_feman_slot = reinterpret_cast<void **>(modutils::scan<void *>(
            {.aob = "48 89 05 ?? ?? ?? ?? 48 8B 8B 80 00 00 00 48 85 C9 74 05 E8",
             .relative_offsets = {{3, 7}}}));
        p_worldchrman_slot = reinterpret_cast<void **>(modutils::scan<void *>(
            {.aob = "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 98 08 E5 01 00",
             .relative_offsets = {{3, 7}}}));
        if (p_build_job && p_refmove && p_push_job)
            spdlog::info("[optmenu] settings-open (F11) primitives resolved (worldChrMan={})",
                         p_worldchrman_slot != nullptr);
        else
        {
            p_build_job = nullptr;
            spdlog::warn("[optmenu] settings-open primitives incomplete; F11 off");
        }
    }
    catch (const std::exception &e)
    {
        p_build_job = nullptr;
        spdlog::warn("[optmenu] settings-open primitive AOB miss: {}", e.what());
    }

    // Task #9 capture: hook the SAME pushJob AFTER the raw scan above (the hook
    // rewrites the prologue, so scanning must come first). Pass-through logger; a
    // miss just disables the capture.
    try
    {
        modutils::hook<PushJobFn>(
            {.aob = "4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC A0 00 00 00 "
                    "48 C7 44 24 40 FE FF FF FF 48 89 9C 24 D0 00 00"},
            pushjob_detour, o_push_job);
        spdlog::info("[pushjob] push-job capture hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[pushjob] capture hook unavailable: {}", e.what());
    }

    // Task #9 capture 2: the generic settings movie-job builder FUN_140808630.
    // Pass-through logger; a miss just disables the capture.
    try
    {
        modutils::hook<BuilderFn>(
            {.aob = "40 55 56 57 41 56 41 57 48 8D 6C 24 C0 48 81 EC 40 01 00 00 "
                    "48 C7 44 24 50 FE FF FF FF 48 89 9C 24 88 01 00 00 48 8B 05 ?? ?? ?? ?? "
                    "48 33 C4 48 89 45 30 4D 8B F0 48 8B F2 48 8B F9"},
            builder_detour, o_builder);
        spdlog::info("[subopen] movie-job builder capture hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[subopen] builder capture hook unavailable: {}", e.what());
    }

    // Task #5 Proto 0: WorldMapMemoSelectDialog control (Route B). Both hooks are
    // pass-through; all mutation is dev-gated on debug_logging inside the detours.
    // A miss just disables the proto. The view-couple is a template with byte-
    // identical instantiations, so it is resolved through its unique call site
    // inside the memo ctor (the E8 disp at the pattern tail).
    try
    {
        p_memo_couple = reinterpret_cast<MemoCoupleFn *>(modutils::scan<void>(
            {.aob = "54 24 30 E8 ?? ?? ?? ?? 45 33 C9 44 8B C3 49 8B D6 48 8D 8E 78 0A 00 00 "
                    "E8 ?? ?? ?? ??",
             .relative_offsets = {{25, 29}}}));
        p_game_alloc = reinterpret_cast<GameAllocFn *>(modutils::scan<void>(
            {.aob = "49 8B 00 4D 8B C8 4C 8B C2 48 8B D1 49 8B C9 48 FF 60 50 90 F3 41 0F 58 C7 "
                    "FF C7 48 83 C3 08 F3 48 85 C9 74 41"}));
        // Resolve the allocator singleton global (mov r8,[rip+d] in the settings build-job
        // factory: `4C 8B 05 <d32> 4C 89 40 18 8D 53 08 B9 A0 18 00 00`); relative_offsets
        // turns the rip-disp into the global's address (= &DAT_143d87350).
        p_alloc_global = reinterpret_cast<void **>(modutils::scan<void>(
            {.aob = "4C 8B 05 ?? ?? ?? ?? 4C 89 40 18 8D 53 08 B9 A0 18 00 00",
             .relative_offsets = {{3, 7}}}));
        spdlog::info("[memoproto] memo-dialog hooks armed (couple=0x{:X} alloc=0x{:X} "
                     "allocGlobal=0x{:X})",
                     reinterpret_cast<uintptr_t>(p_memo_couple),
                     reinterpret_cast<uintptr_t>(p_game_alloc),
                     reinterpret_cast<uintptr_t>(p_alloc_global));
    }
    catch (const std::exception &e)
    {
        p_memo_couple = nullptr;
        spdlog::warn("[memoproto] memo-dialog hooks unavailable: {}", e.what());
    }

    // Scaleform attachMovie DAPI bridge (v1.16 FUN_1410e00c0). Its export name
    // and initializer object describe the ActionScript object that is about to
    // enter a display-list parent; the nested high-level attach hook records the
    // actual parent/count while this thread-local context is active.
    try
    {
        modutils::hook<V3AttachMovieFn>(
            {.aob = "4C 8B DC 4D 89 4B 20 4D 89 43 18 55 56 41 57 49 8D 6B D8 "
                    "48 81 EC 10 01 00 00 48 8B 41 08 49 8B F1 48 8B 4A 28 4C 8B 78 18 "
                    "8B 81 90 00 00 00 83 E8 1F 83 F8 05"},
            v3_attach_movie_detour, o_v3_attach_movie);
        spdlog::info("[stallprobe] v3 attachMovie bridge spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 attachMovie bridge spy unavailable: {}", e.what());
    }

    // High-level display-object move/attach API (v1.16 FUN_1410c8440). The
    // detour observes the wrapper for the 104-child candidate marker layer; once the
    // depth-24 custom child also exists, v3_try_visual_move performs one normal
    // engine attach through this function's trampoline.
    try
    {
        modutils::hook<V3AttachFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 "
                    "48 8B DA 45 8B F0 48 8B 51 18 48 8B E9 48 8B 4B 38 8B 82 E0 00 00 00"},
            v3_attach_detour, o_v3_attach);
        spdlog::info("[stallprobe] v3 high-level attach experiment armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 high-level attach experiment unavailable: {}", e.what());
    }

    // Close-wait job spy (diagnostics only; pass-through outside capture windows).
    try
    {
        modutils::hook<Fn4>(
            {.aob = "48 83 EC 28 48 8B 09 48 85 C9 74 16 48 83 C1 10 E8 FB A6 08 01 85 C0 0F"},
            jobpoll_detour, o_jobpoll);
        spdlog::info("[stallprobe] job-poll spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] job-poll spy unavailable: {}", e.what());
    }
}

void goblin::stall_probe::capture(const char *tag, unsigned duration_ms)
{
    // Compile-time OFF by default: this suspends the calling (map UI) thread from a worker for the
    // whole window - see goblin_build_variants.hpp.
    if (!goblin::variants::kStallProfiler) return;
    if (!goblin::config::debugLogging) return;
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    DWORD tid = GetCurrentThreadId();
    try
    {
        std::thread(run_capture, std::string(tag ? tag : "capture"), tid, duration_ms).detach();
    }
    catch (...)
    {
        g_running.store(false);
    }
}
