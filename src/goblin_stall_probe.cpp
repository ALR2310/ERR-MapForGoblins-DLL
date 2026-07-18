#include "goblin_stall_probe.hpp"
#include "goblin_config.hpp"
#include "generated_shared/goblin_map_icons.hpp"
#include "goblin_maphover.hpp"
#include "goblin_mapproject.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_inject.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
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

    // Leaf 1: scan the parent's logical child vector (owner+0xd8: base@[0],
    // count@+0xe0, entry stride 0x10 with the child ptr at +0) and record the
    // indices whose child ptr is in the pre-sorted `sorted[0..n)` set (our
    // markers). Pure raw memory + POD only, so SEH is legal here (no C++ unwind).
    // Returns the count of ascending indices written to out_idx (capped).
    uint32_t v3_detach_scan(uintptr_t parent, const uintptr_t *sorted, uint32_t n,
                            uint32_t *out_idx, uint32_t out_cap)
    {
        uint32_t found = 0;
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
            // Return whatever was collected from the consistent pre-fault snapshot;
            // those ascending indices are still valid for a descending removal.
        }
        return found;
    }

    // Leaf 2: remove the collected indices via the engine's remove-at primitive,
    // DESCENDING (out_idx is ascending) so each surviving index stays valid and the
    // per-remove tail memmove shrinks. Removing drops the container's reference,
    // freeing the child. SEH-guarded. Returns the count removed.
    uint32_t v3_detach_remove(uintptr_t wrapper, const uint32_t *idx, uint32_t n)
    {
        uint32_t removed = 0;
        __try
        {
            for (uint32_t k = n; k-- > 0;)
            {
                g_v3_remove_at(reinterpret_cast<void *>(wrapper), idx[k]);
                ++removed;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
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
                              // render node). Always true unless nativeViewportWindow.
        bool ref_held = false; // lever B: we kept an extra reference at build so this
                               // child survives being detached. Only children built with
                               // nativeViewportWindow ON have it -> only they may be
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
        ctx.active = goblin::config::debugLogging;
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
                v3_native_set_visible(g_v3_native.objects[found->second], point.visible);
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
        if (!goblin::config::debugLogging) return;
        const int layer = goblin::maphover::map_layer();
        if (layer < 0 || layer > 2) return;

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
            g_v3_native.observed_count = live_count;
            return;
        }
        if (g_v3_native.layer != layer)
        {
            // Same shared parent hosts every layer's markers: a layer switch
            // is pure show/hide of the right rows - re-snapshot immediately.
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
            spdlog::warn("[v3native] {} queued markers never built (no factory "
                         "pulses); dropping the queue to keep live refresh alive",
                         g_v3_native.pending.size() - g_v3_native.pending_index);
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
                if (goblin::config::nativeViewportWindow && ok)
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
                obj.ref_held = goblin::config::nativeViewportWindow;
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

    // Lever B (nativeViewportWindow): keep out-of-view markers DETACHED so they carry
    // no TreeCacheNode; attach only those within the visible map rect (+ hysteresis).
    // Runs once per map frame on the map UI thread; bounded ops/frame so a big pan
    // reconciles across a few frames instead of one spike. Detaching happens while the
    // movie is alive and rendering, so the freed render nodes recycle on normal frames
    // (async) - unlike the close-time lever C, whose nodes never get a reconcile pass.
    size_t g_v3_vp_cursor = 0; // (unused; reserved for round-robin if needed)
    void v3_viewport_reconcile()
    {
        if (!goblin::config::nativeViewportWindow || !g_v3_remove_at)
            return;
        if (!g_v3_native.seeded || g_v3_native.objects.empty() ||
            g_v3_map_closed.load(std::memory_order_relaxed))
            return;
        const uintptr_t wrapper = g_v3_native.wrapper;
        const uintptr_t parent = g_v3_native.parent;
        if (!v3_heap_ptr(wrapper) || !v3_heap_ptr(parent))
            return;

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
            const uint32_t found = v3_detach_scan(parent, det_ptrs.data(),
                                                  static_cast<uint32_t>(det_ptrs.size()),
                                                  idxbuf.data(),
                                                  static_cast<uint32_t>(idxbuf.size()));
            v3_detach_remove(wrapper, idxbuf.data(), found);
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
        // The map's native children are inserted before the first per-marker
        // refresh call starts a sampling window. Keep this tiny probe active for
        // the whole debug-logging session and report it at the next window end.
        const bool capture = goblin::config::debugLogging;
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
                spdlog::warn("[v3native] deferred child rejected: child=0x{:X} "
                             "charId={} depth={} roots 0x{:X}/0x{:X} held={} staged={}",
                             child, char_id, depth, child_root, target_root, held, staged);
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
        if (!goblin::config::debugLogging)
            return;

        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(wrapper_ptr);
        uint64_t parent = 0, count = 0;
        if (!v3_heap_ptr(wrapper) || !v3_read64(wrapper + 0x18, parent) ||
            !v3_heap_ptr(parent) || !v3_read64(static_cast<uintptr_t>(parent) + 0xe0, count))
            return;
        v3_note_movie_attach(wrapper, static_cast<uintptr_t>(parent), count);
        v3_note_build_attach(wrapper, static_cast<uintptr_t>(parent), count);
        v3_record_candidate(wrapper, static_cast<uintptr_t>(parent), caller, count);
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
                if (goblin::config::fastMapOpen && a)
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
    g_v3_map_closed.store(true, std::memory_order_release);
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
    if (!goblin::config::nativeSelfDetach || !g_v3_remove_at)
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
    const uint32_t found = v3_detach_scan(parent, ours.data(),
                                          static_cast<uint32_t>(ours.size()),
                                          idxbuf.data(),
                                          static_cast<uint32_t>(idxbuf.size()));
    const uint32_t removed = v3_detach_remove(wrapper, idxbuf.data(), found);
    QueryPerformanceCounter(&t1);
    const uint64_t us = freq.QuadPart
        ? static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart)
        : 0;
    g_v3_last_detached.store(removed, std::memory_order_relaxed);
    g_v3_last_detach_us.store(us, std::memory_order_relaxed);
    spdlog::info("[v3native] self-detach: matched={} removed={} tracked={} in {} us",
                 found, removed, g_v3_native.objects.size(), us);

    // Those children are unlinked and their container reference dropped (freed).
    // Drop every cached pointer now so nothing downstream (re-arm, quick-reopen
    // refresh) can touch a freed child. on_map_close() ran just before us and set
    // g_v3_map_closed, so the next real build burst re-seeds from scratch.
    g_v3_native.objects.clear();
    g_v3_native.by_row.clear();
    g_v3_native.queued.clear();
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
