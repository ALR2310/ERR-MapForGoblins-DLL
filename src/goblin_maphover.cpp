// World-map hover detection (production). See goblin_maphover.hpp.
//
// FUN_14087a8e0 (v2.6.2.0) = the map dialog's per-frame "update the PlaceName panel for
// the currently-focused pin". The dialog picks the pin NEAREST the reticle within a
// radius each frame and passes it here to display its name - so this item IS the icon
// the game highlights + popups. __fastcall(rcx = panel, RDX = item pin, r8 = map area);
// item is null when nothing is focused.
//
// item = CS::WorldMapPointPinData (vtable RVA 0x2AD6688). The underlying param row is at
// *(item+0x248) (live-confirmed: row+0x30 holds our offset-encoded textId). We publish
// that row ptr; the inject layer matches it to one of our CategoryRow::p.
#include "goblin_maphover.hpp"

#include "goblin_anchors.hpp"     // every base+RVA engine helper resolves through this
#include "goblin_guarded.hpp"     // these engine calls fault on purpose; the logger stays quiet
#include "goblin_collected.hpp"  // read_player_pos() for the height line
#include "goblin_config.hpp"     // hover_info: this panel is now its ONLY consumer
#include "goblin_i18n.hpp"    // the localized hover sentences
#include "goblin_progress.hpp" // region names for the focus banner    // the localized hover sentences
#include "goblin_inject.hpp"
// (goblin_messages.hpp was included here "for lookup_text()"; no export of that header is used
//  in this file. The trailing note about native_reticle_row belongs to goblin_inject.hpp above.)
#include "goblin_map_timing.hpp"
#include "goblin_stall_probe.hpp"
#include "goblin_safemem.hpp" // validate-then-read for the pin/dialog pointer checks
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace
{
    using PlaceNameFn = void *(void *, void *, void *);  // __fastcall
    PlaceNameFn *o_placename = nullptr;

    // vtable addresses, resolved by AOB from each vtable's ctor lea in setup() (were
    // hardcoded RVAs 0x2AD6688 / 0x2B2CB08 - both move on game updates). 0 if the AOB
    // missed (the corresponding gate then never matches -> feature stays disabled).
    uintptr_t g_pin_vt = 0;      // CS::WorldMapPointPinData vtable
    uintptr_t g_maparea_vt = 0;  // CS::WorldMapArea vtable

    // The hover POPUP panel (rcx of the hook). FUN_14087A8E0 fills it entirely through
    // primitives we can call ourselves, so capturing it is all that is needed to drive our own:
    //   FUN_140735A60(panel + 8, 0|1)            hide / show the panel
    //   FUN_1407356E0(panel + 8, &float3{x,y,z}) place it (coords already clamped to the map)
    // and inside it a variant-indexed sub-panel with an array of text lines:
    //   base  = panel + 0x108 + (*(uint*)(panel + 0x19C0)) * 0xC80, rounded up to 8
    //   line  = base + i * 0x180,  slot count = *(int64*)(base + 0xC08)
    //   FUN_140735A60(line, visible) / FUN_1407353B0(line + 0x70, DLString)
    std::atomic<void *> g_popup_panel{nullptr};
    // One-shot latch for the line-slot log below. It is an int32 because it used to publish the
    // popup's line-slot count for popup_line_slots(), which was removed with the overlay's on-map
    // panel; the only thing left that reads it is its own "have I logged this yet" test.
    std::atomic<int32_t> g_popup_lines{-1};
    std::atomic<bool> g_root_probed{false};

    std::atomic<void *> g_hovered_row{nullptr};
    // Where the game last told us its reticle is (map space), and when.
    std::atomic<float> g_reticle_mx{0.0f};
    std::atomic<float> g_reticle_mz{0.0f};
    std::atomic<uint64_t> g_reticle_ms{0};
    std::atomic<void *> g_dialog{nullptr};
    std::atomic<void *> g_map_owner{nullptr};  // buildMarkers `this` (r15); its +0x398 map = current-layer pins
    std::atomic<uint64_t> g_last_hook_ms{0};

    // buildMarkers (0x140A82A80): builds the CURRENT map layer's pins into [owner+0x398]
    // (visibility-filtered, fold already applied). We capture `owner` so the overlay can
    // read that live current-layer pin set. __fastcall(rcx=owner, rdx=layer-context).
    using BuildFn = void *(void *, void *, void *, void *);
    BuildFn *o_build = nullptr;


    // Native-tooltip proxy: the last REAL pin the engine focused this map
    // session. When the engine focuses nothing but one of OUR native markers
    // sits under the reticle, we re-point this pin's row at ours for exactly
    // one o_placename call, so the game's own name panel renders the label
    // (works with the overlay disabled). Cleared whenever pins are rebuilt.
    std::atomic<void *> g_proxy_pin{nullptr};

    // Preferred proxy: our OWN pin, built once by the engine's own pin ctor
    // (FUN_14087ba70 in v2.6.2.0) into a static buffer. RE-verified
    // (scratch/re_pin_ctor_results.md): the ctor reads ONLY POD fields of the
    // source wrapper (+0x08 id, +0x10 row ptr; it installs the wrapper vftable
    // itself), plus a float[2] map-space position - so a fabricated source is
    // safe, no live engine object is ever mutated, and the label works before
    // any real pin was focused. Constructed lazily on the map UI thread; the
    // buffer is never handed to engine ownership (the panel only reads it for
    // the duration of one call).
    using PinCtorFn = void *(void *, void *, void *); // (pin, posPtr, srcWrapper)
    PinCtorFn *g_pin_ctor = nullptr;
    alignas(16) uint8_t g_own_pin[0x310]{};
    bool g_own_pin_ready = false; // map UI thread only
    struct FakeSrcWrapper
    {
        void *vft;    // ignored by the ctor (it writes its own constant)
        uint64_t id;  // row id -> pin+0x240 (panel does not consume it)
        void *row;    // WORLD_MAP_POINT_PARAM_ST* -> pin+0x248 (must outlive the pin)
    };

    bool seh_construct_own_pin(void *row, float mx, float mz)
    {
        FakeSrcWrapper src{nullptr, 0, row};
        float pos[2] = {mx, mz};
        __try
        {
            g_pin_ctor(g_own_pin, pos, &src);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void *build_detour(void *owner, void *ctx, void *a, void *b)
    {
        g_map_owner.store(owner, std::memory_order_relaxed);
        g_proxy_pin.store(nullptr, std::memory_order_relaxed);
        // (A g_dialog_data pointer was stored here with a note saying map_layer() reads the
        //  displayed map id live from *(int*)(dialogData+8). It does not: map_layer() reads
        //  MapArea+0x904 off g_dialog. Nothing ever loaded g_dialog_data.)
        goblin::stall_probe::v3_pin_build_begin(owner, ctx);
        void *result = o_build(owner, ctx, a, b);
        goblin::stall_probe::v3_pin_build_end();
        return result;
    }

    constexpr size_t PIN_ROW_OFF = 0x248;  // item -> underlying WorldMapPointParam row
    // CS::WorldMapPointPinData layout (Ghidra pass, scratch/pin_layout_re*.log):
    // +0x0C computed-visible byte (panel gates on it), +0x10 f32[2] map-space
    // position - the panel projects the label anchor from it (screen =
    // mapPos*scale - pan), so a borrowed pin must get OUR position too.
    constexpr size_t PIN_VIS_OFF = 0x0C;
    constexpr size_t PIN_POS_OFF = 0x10;
    // The hook's r8 (map_area) is CS::WorldMapArea. It carries the live view transform:
    // pan @+0x378/+0x37C, zoom/scale @+0x380, fullRect side @+0x358 (10496). We publish
    // this object directly (no dialog hunt) for the overlay projection. Verified live.

    // ── where the game itself looks for a pin ────────────────────────────────────────
    // Its per-frame update (FUN_1409C32F0, the routine that calls our hook) hands its own pin finder a
    // position pair, and it has TWO of them: the dialog's flag at +0x2F01 selects which. That is the
    // very question our own pick used to answer by guessing from the build ("the reticle is the view
    // centre, unless this is Convergence, where it follows the pointer") - and that guess was only true
    // while the map can still pan. At full zoom-out it cannot, the reticle leaves the centre, and the
    // guess described whatever sat in the middle of the screen instead.
    //
    // Both pairs are MAP space - the same space the pins the finder returns carry at pin+0x10 - so they
    // can be handed straight to our own nearest-marker search. Offsets are on the DIALOG, which is
    // MapArea - 0x27D8 (the same relation the map-layer field uses).
    constexpr size_t kDlgFromArea = 0x27D8;
    constexpr size_t kDlgSearchMode = 0x2F01;   // 0 = search around the pair below, else the other one
    constexpr size_t kDlgReticlePair = 0xA38;   // float x, float z (the dialog copies it from +0x2EB4)
    constexpr size_t kDlgPointerPair = 0x2EBC;  // float x, float z
    // The map is a square of a bit over 10000 units a side, so anything far outside that is not a
    // position and the caller keeps whatever it did before.
    constexpr float kMapCoordLo = -4000.0f;
    constexpr float kMapCoordHi = 18000.0f;

    bool read_reticle_pair(void *map_area, float *mx, float *mz, bool *pointer_mode)
    {
        if (!map_area)
            return false;
        bool ok = false;
        ++goblin::guarded::depth;
        __try
        {
            const uintptr_t dlg = reinterpret_cast<uintptr_t>(map_area) - kDlgFromArea;
            const bool ptr_mode = *reinterpret_cast<uint8_t *>(dlg + kDlgSearchMode) != 0;
            const float *p = reinterpret_cast<const float *>(
                dlg + (ptr_mode ? kDlgPointerPair : kDlgReticlePair));
            const float x = p[0], z = p[1];
            if (x > kMapCoordLo && x < kMapCoordHi && z > kMapCoordLo && z < kMapCoordHi)
            {
                *mx = x;
                *mz = z;
                *pointer_mode = ptr_mode;
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        --goblin::guarded::depth;
        return ok;
    }

    // Validate-then-read (goblin_safemem.hpp): a pin/dialog pointer that has been freed is the
    // normal case here, not the exceptional one.
    bool read_vt(void *obj, uintptr_t &out)
    {
        return goblin::safemem::copy(&out, obj, sizeof(out));
    }

    // ── our own hover panel ──────────────────────────────────────────────────────────
    // Body/MfgTip is a second named placement of the tooltip sprite, added by the load-time
    // transform. It is OURS: own position, own line count, own text.
    //
    // The game's own Body/PlaceName IS read and written in this file (kGamePanel below, and the
    // show/hide of the game popup) - an earlier version of this note claimed otherwise.
    //
    // Coordinates: map space -> the space Body's children live in. FUN_1409CC470 does it for the
    // game, but it needs a pin, and our markers have none - so the same formula is applied inline
    // (`out = zoom * v - pan` over WorldMapArea +0x380 / +0x378 / +0x37C). The converter's address
    // is therefore NOT called from here; only its arithmetic is reused.
    constexpr uintptr_t kSetVisible = 0x733340;
    constexpr uintptr_t kSetPosI = 0x7331A0;
    // ── the game's own wrappers are a different type from a resolved proxy ───────────
    // A path lookup hands back a PLAIN proxy: the object itself, and the two primitives above take it
    // directly. The popup and each of its lines are a RICH wrapper that embeds such a proxy at +8 and
    // remembers what it last applied: visibility at +0x69, position at +0x78/+0x7C. Both of the
    // game's own setters below take that wrapper, and they SKIP a write that matches their memory -
    // so touching one of those objects through its inner proxy instead would leave the memory lying,
    // and the game's own next write would be dropped as redundant. That is why the popup is shown
    // with kPanelVisible and placed with kPanelPosF, never with the two primitives above.
    constexpr uintptr_t kPanelVisible = 0x735A60;  // FUN_140735A60(wrapper, 0|1)
    constexpr uintptr_t kPanelPosF = 0x7356E0;     // FUN_1407356E0(wrapper, float[2])
    constexpr uintptr_t kSetTextHtml = 0x74A000;
    constexpr uintptr_t kResolve = 0x74A2F0;
    constexpr uintptr_t kProxyValid = 0x733150;
    constexpr uintptr_t kProxyDtor = 0xD7F850;
    // Text colour. These fields are html=0, so markup is not an option here - unlike the menu,
    // where the same job is done with a <font> tag.
    constexpr uintptr_t kSetTextColor = 0x74A1D0;
    constexpr int kTipLines = 8;
    constexpr const char *kTipPanel = "MfgTip";
    constexpr const char *kBannerPanel = "MfgBanner";
    // The game's own name popup. Only the fallback path touches it - see drive_own_tip.
    constexpr const char *kGamePanel = "PlaceName";
    constexpr uint32_t kBannerRed = 0xFFE05050; // top byte = alpha, or it draws clear
    // Set explicitly rather than left to the field's authored colour, which read as grey.
    constexpr uint32_t kTipColor = 0xFFE8D9A0;  // the parchment the menu uses for values

    // The map's own left/right bounds for a popup, the pair the game clamps against
    // (FUN_14087A8E0 tests popupX against +0x340 and popupX+shoulder against +0x348).
    // The popup's own left/right shoulder offsets, read from the game's panel object. The clamp
    // in FUN_14087A8E0 tests `panel+0x88 + x` against the left bound, so pinning to that bound
    // means x = bound - shoulder; using the bound directly is what left the panel hanging half
    // off the screen.
    // How far to sit right of the visible left edge so the whole panel is on screen.
    //
    // Measured, not assumed: the bounds come back as left = -267.2, right = 1652.8 (a span of
    // exactly 1920, so this IS the visible width and the left edge really is -267.2), while
    // panel+0x88 reads +17.35 - the same value as the authored tx of a line's text. So that
    // field is the text's inset INTO the panel, not the panel's overhang, and subtracting it
    // pushed the panel further out instead of in. What actually hangs left of the origin is the
    // line backdrop (cid 218 sits at tx 0 with sx 1.0984 and is drawn about its own centre),
    // whose width is not exposed anywhere we can read - hence a constant, tuned by eye.
    constexpr float kPanelLeftInset = 270.0f;
    // ULTRAWIDE. `left` above is the game's own visible-rect left edge in STAGE units, and on a
    // 16:9 display it reads -267.2, so left + 270 lands at +2.8 - just inside the stage. An
    // ultrawide setup (reported on 3440x1440 with the Ultrawide Fix, mod 283, variant 21.5x9)
    // shows more of the world horizontally, so that edge moves far to the left, and the panel
    // follows it clean off the screen: the reporter sees only a sliver of the backdrop and no text
    // at all. The Ultrawide Fix ships its own 02_120_worldmap.gfx, but the stage rect in it is
    // still exactly 1920x1080 - verified by parsing both files - so the stage is a fixed, aspect-
    // independent frame while the visible rect is not.
    //
    // So: follow the visible edge, but never past the STAGE's own left edge, which is 0 by
    // definition and does not move with the display. On 16:9 the wanted position is +2.8, so the
    // floor never engages and nothing changes at all; on ultrawide the panel stops hugging the
    // screen edge and sits at the stage edge instead - further in than intended, but ON SCREEN.
    //
    // The floor was 4.0 for one build and that was wrong: it sat ABOVE the natural 16:9 value, so
    // it fired on the aspect everyone plays at (measured: wanted=2.7999878, used=4, logged as
    // clamped). Harmless as a 1.2-unit shift, but it made the log claim a clamp on a display that
    // needs none - and a diagnostic that cries wolf is worse than none.
    //
    // This is a floor, not the real answer. The real answer needs the visible rect from an actual
    // ultrawide machine - the "[maphover] map bounds:" line, logged once per session - because
    // without it there is no way to know whether that edge is even reported correctly under the
    // fix. Ask for that log before tuning anything here further.
    constexpr float kPanelMinStageX = 0.0f;

    // Where the panels sit horizontally, as a percentage the player can move.
    //
    //   100 = the corner the panels were authored for   (visible_left + kPanelLeftInset)
    //     0 = the centre of the map area                ((visible_left + visible_right) / 2)
    //  >100 = further left,   <0 = right of centre
    //
    // Expressed through the visible rect rather than through constants, so if a display or a mod
    // ever does report a different rect, 100 still means "where it was meant to be" on that setup.
    //
    // Why a setting at all: the ultrawide report cannot be reproduced here. Three aspect packs were
    // diffed placement by placement and the rect's left edge is -267.2 in every one of them - the
    // packs scale the map mask, they never move it - so on every .gfx we possess the panels land in
    // the same place and the reported symptom cannot occur. Rather than guess at a mechanism we
    // cannot see, give the player a slider: one minute of their time against an unbounded number of
    // display and mod combinations we would otherwise have to chase.
    //
    // Read fresh on every call, never cached: the tooltip path recomputes this each frame it draws,
    // so moving the slider shows up on the next frame with the map still open.
    float panel_left_x(float visible_left, float visible_right)
    {
        const float authored = visible_left + kPanelLeftInset;
        const float centre = (visible_left + visible_right) * 0.5f;
        float pct = goblin::config::mapPanelOffsetPercent;
        if (!(pct > -1000.0f && pct < 1000.0f))
            pct = 100.0f;  // a broken ini value must not park the panel off in nowhere
        const float x = centre + (pct * 0.01f) * (authored - centre);
        static float s_logged = -99999.0f;
        if (x != s_logged)
        {
            s_logged = x;
            spdlog::info("[maphover] panel x: visibleLeft={} centre={} authored={} pct={} used={}",
                         visible_left, centre, authored, pct, x);
        }
        return x;
    }
    // Both panels sit in the corner: the tooltip first, the focus banner a line and a half below
    // it (the sprite spaces its own lines 35.9 apart).
    constexpr int kTipTopY = 60;
    constexpr int kBannerTopY = kTipTopY + 54;

    // The visible rectangle. +0x340 / +0x348 are the horizontal pair the game clamps popups
    // against (measured: -267.2 and 1652.8, a span of exactly 1920), so the vertical pair is
    // almost certainly interleaved with them as {left, top, right, bottom}. Logged once so the
    // span can be checked against 1080 instead of assumed.
    bool map_bounds(void *map_area, float *left, float *top, float *right, float *bottom)
    {
        bool ok = false;
        ++goblin::guarded::depth;
        __try
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(map_area);
            *left = *reinterpret_cast<float *>(a + 0x340);
            *top = *reinterpret_cast<float *>(a + 0x344);
            *right = *reinterpret_cast<float *>(a + 0x348);
            *bottom = *reinterpret_cast<float *>(a + 0x34C);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        --goblin::guarded::depth;
        return ok;
    }

    bool map_x_bounds(void *map_area, float *left, float *right)
    {
        float t = 0.0f, b = 0.0f;
        if (!map_bounds(map_area, left, &t, right, &b))
            return false;
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            spdlog::info("[maphover] map bounds: left={} top={} right={} bottom={} "
                         "(spans {} x {})",
                         *left, t, *right, b, *right - *left, b - t);
        }
        return true;
    }

    // Vertical middle of the visible area, for a panel pinned at the left edge.
    int map_middle_y(void *map_area, int fallback)
    {
        float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
        if (!map_bounds(map_area, &l, &t, &r, &b))
            return fallback;
        const float span = b - t;
        if (span < 200.0f || span > 4000.0f)
            return fallback; // not the pair we think it is - keep the known-good constant
        return static_cast<int>(t + span * 0.5f);
    }

    // ── where is the game's own popup right now? ─────────────────────────────────────
    // FUN_1407356E0(wrapper, float[2]) stores the position it is about to apply at wrapper+0x78
    // before applying it, and the popup's wrapper is `panel + 8` (the hook's rcx + 8, the object the
    // game shows/hides and places). So this pair IS the popup's live position, in the same space our
    // own panel is placed in - Body's - with the game's own clamping already folded in. Reading it
    // beats re-deriving the projection: the game clamps against the map's bounds with the panel's own
    // shoulder widths, and any drift there would show as a panel sitting a few pixels off.
    constexpr size_t kWrapperPosOff = 8 + 0x78;

    bool popup_pos(void *panel, float *x, float *y)
    {
        if (!panel)
            return false;
        bool ok = false;
        ++goblin::guarded::depth;
        __try
        {
            const uintptr_t p = reinterpret_cast<uintptr_t>(panel);
            const float px = *reinterpret_cast<float *>(p + kWrapperPosOff);
            const float py = *reinterpret_cast<float *>(p + kWrapperPosOff + 4);
            // Before the first hover the cache is still zero while the clip sits at its authored
            // spot, and a delta computed from that would be wrong by exactly that offset. One frame
            // without our line is better than one frame with it in the wrong place.
            if (px != 0.0f || py != 0.0f)
            {
                *x = px;
                *y = py;
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        --goblin::guarded::depth;
        return ok;
    }

    // Where the sprite's first line sits inside its panel, and how far apart the lines are (authored;
    // identical in the vanilla movie and in the Convergence one, which is vanilla-derived). Borrowing
    // a line of the game's panel means placing the LINE, so these keep it exactly where our own
    // panel's lines land.
    constexpr float kLineInsetX = 17.0f;
    constexpr float kLineInsetY = -39.0f;
    constexpr float kLinePitch = 35.9f;

    // Place / show the game's popup through the very primitives it uses on it itself: the float
    // placer also refreshes the wrapper's remembered position, so the game's next hover still moves
    // the popup wherever it wants instead of deciding it is already there.
    bool popup_place(uintptr_t base, void *panel, const float *at)
    {
        if (!panel)
            return false;
        // On a build where the anchor could not be placed at() answers 0, and calling it would
        // raise a first-chance AV here EVERY frame the map is open - exceptions other tools'
        // filters see even though our SEH swallows them. Dead anchor = feature off, quietly.
        auto p_place = reinterpret_cast<char (*)(void *, const float *)>(goblin::anchors::at(kPanelPosF));
        if (!p_place)
            return false;
        bool ok = false;
        ++goblin::guarded::depth;
        __try
        {
            p_place(reinterpret_cast<uint8_t *>(panel) + 8, at);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        --goblin::guarded::depth;
        return ok;
    }

    void popup_show(uintptr_t base, void *panel, bool show)
    {
        if (!panel)
            return;
        auto p_visible = reinterpret_cast<void (*)(void *, char)>(goblin::anchors::at(kPanelVisible));
        if (!p_visible)
            return;  // dead anchor - see popup_place
        ++goblin::guarded::depth;
        __try
        {
            p_visible(reinterpret_cast<uint8_t *>(panel) + 8, show ? 1 : 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        --goblin::guarded::depth;
    }

    // The game's own wrapper for line `i` of its popup, at the address its per-frame loop walks:
    // panel + 0x108 + variant * 0xC80, rounded up to 8, then i * 0x180 - with the slot count at
    // +0xC08 of that base. null when the layout is not the one we parsed or the slot is out of range.
    void *game_line(void *panel, int i)
    {
        if (!panel || i < 0)
            return nullptr;
        void *res = nullptr;
        ++goblin::guarded::depth;
        __try
        {
            const uintptr_t p = reinterpret_cast<uintptr_t>(panel);
            const uint32_t variant = *reinterpret_cast<uint32_t *>(p + 0x19C0);
            uintptr_t b = p + 0x108 + static_cast<uintptr_t>(variant) * 0xC80;
            b += (-static_cast<intptr_t>(b)) & 7;
            const int64_t slots = *reinterpret_cast<int64_t *>(b + 0xC08);
            if (slots > 0 && slots <= 64 && i < slots)
                res = reinterpret_cast<void *>(b + static_cast<uintptr_t>(i) * 0x180);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            res = nullptr;
        }
        --goblin::guarded::depth;
        return res;
    }

    // Hide one of the game's OWN lines the way the game would, so its wrapper's memory stays true and
    // its next "show this line" is not dropped as redundant (that would stop the map naming places).
    void game_line_hide(uintptr_t base, void *panel, int i)
    {
        void *w = game_line(panel, i);
        if (!w)
        {
            // Said once: without it the popup we force visible for the banner can keep showing the
            // last place name it was given, and that would otherwise look like a phantom label.
            static bool s_said = false;
            if (!s_said)
            {
                s_said = true;
                spdlog::info("[maphover] the popup's line array is not where we parse it - the game's "
                             "own lines cannot be blanked while the focus banner borrows the panel");
            }
            return;
        }
        auto p_visible = reinterpret_cast<void (*)(void *, char)>(goblin::anchors::at(kPanelVisible));
        if (!p_visible)
            return;  // dead anchor - see popup_place
        ++goblin::guarded::depth;
        __try
        {
            p_visible(w, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        --goblin::guarded::depth;
    }

    // POD-only worker (SEH): set one of our lines, or hide it.
    // Is a panel actually in this movie? Resolved once per map open: the answer only changes when a
    // different 02_120 is loaded. -1 unknown, 0 no, 1 yes.
    int g_have_own_tip = -1;

    bool panel_present(uintptr_t base, void *root, const char *panel)
    {
        auto p_resolve = reinterpret_cast<void *(*)(void *, void *, const char *)>(goblin::anchors::at(kResolve));
        auto p_valid = reinterpret_cast<char (*)(void *)>(goblin::anchors::at(kProxyValid));
        auto p_dtor = reinterpret_cast<void (*)(void *)>(goblin::anchors::at(kProxyDtor));
        if (!p_resolve || !p_valid || !p_dtor)
            return false;  // dead anchor - see popup_place
        bool ok = false;
        ++goblin::guarded::depth;
        __try
        {
            char path[128];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "Body/%s", panel);
            uint8_t buf[0x60] = {};
            void *pp = p_resolve(root, buf, path);
            ok = p_valid(pp) != 0;
            p_dtor(buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        --goblin::guarded::depth;
        return ok;
    }

    // `at` (optional, 2 floats) moves the line inside its panel. Only the borrowed-panel path uses it:
    // our own panel is placed as a whole, while a line of the GAME's panel has to be pulled out from
    // under a panel that follows the marker. The game never positions these lines itself (its
    // per-frame routine only sets each line's text and visibility), so the write is not fought.
    void tip_line(uintptr_t base, void *root, const char *panel, int i, const wchar_t *text,
                  uint32_t rgb, const float *at = nullptr)
    {
        auto p_resolve = reinterpret_cast<void *(*)(void *, void *, const char *)>(goblin::anchors::at(kResolve));
        auto p_valid = reinterpret_cast<char (*)(void *)>(goblin::anchors::at(kProxyValid));
        auto p_dtor = reinterpret_cast<void (*)(void *)>(goblin::anchors::at(kProxyDtor));
        auto p_visible = reinterpret_cast<void (*)(void *, char)>(goblin::anchors::at(kSetVisible));
        auto p_pos = reinterpret_cast<void (*)(void *, int32_t, int32_t)>(goblin::anchors::at(kSetPosI));
        auto p_settext =
            reinterpret_cast<void (*)(void *, const wchar_t *)>(goblin::anchors::at(kSetTextHtml));
        auto p_color = reinterpret_cast<void (*)(void **, uint32_t)>(goblin::anchors::at(kSetTextColor));
        if (!p_resolve || !p_valid || !p_dtor || !p_visible || !p_pos || !p_settext || !p_color)
            return;  // dead anchor - see popup_place
        ++goblin::guarded::depth;
        __try
        {
            char path[128];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "Body/%s/State_0/Text_%d", panel, i);
            uint8_t buf[0x60] = {};
            void *line = p_resolve(root, buf, path);
            if (p_valid(line))
            {
                p_visible(line, text ? 1 : 0);
                if (text && at)
                    p_pos(line, static_cast<int32_t>(at[0]), static_cast<int32_t>(at[1]));
                if (text)
                {
                    _snprintf_s(path, sizeof(path), _TRUNCATE,
                                "Body/%s/State_0/Text_%d/Text", panel, i);
                    uint8_t tb[0x60] = {};
                    void *fld = p_resolve(root, tb, path);
                    if (p_valid(fld))
                    {
                        p_settext(reinterpret_cast<uint8_t *>(fld) + 8, text);
                        if (rgb)
                        {
                            // FUN_14074A1D0(void** proxy, u32 argb): it does
                            // (**(code**)(*(longlong*)*param_1 + 8))(), i.e. TWO dereferences,
                            // so it wants the ADDRESS of the proxy - handing it the proxy itself
                            // made it call through a code pointer as if it were a vtable.
                            // The colour is re-packed from the argument's bytes with the top one
                            // as alpha, so it must be 0xFFRRGGBB, not 0x00RRGGBB (that was the
                            // "grey" - fully transparent).
                            void *fldp = fld;
                            p_color(&fldp, rgb);
                        }
                    }
                    p_dtor(tb + 0x28);
                }
            }
            p_dtor(buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        --goblin::guarded::depth;
    }

    // Two SEH readers (proxy_object / read_float) lived here for a one-off measurement: find the
    // offset of a display object's position pair by writing a known position and searching for
    // (x*20, y*20) in twips. The measurement is long done - the reticle field is documented and
    // popup_pos() reads it directly - and neither helper had a caller left. The comment that
    // introduced them described a "-1 until found" variable that no longer existed either.

    // POD-only: show/hide our panel root and place it.
    void tip_root(uintptr_t base, void *root, const char *panel, bool show, int x, int y)
    {
        auto p_resolve = reinterpret_cast<void *(*)(void *, void *, const char *)>(goblin::anchors::at(kResolve));
        auto p_valid = reinterpret_cast<char (*)(void *)>(goblin::anchors::at(kProxyValid));
        auto p_dtor = reinterpret_cast<void (*)(void *)>(goblin::anchors::at(kProxyDtor));
        auto p_visible = reinterpret_cast<void (*)(void *, char)>(goblin::anchors::at(kSetVisible));
        auto p_pos = reinterpret_cast<void (*)(void *, int32_t, int32_t)>(goblin::anchors::at(kSetPosI));
        if (!p_resolve || !p_valid || !p_dtor || !p_visible || !p_pos)
            return;  // dead anchor - see popup_place
        ++goblin::guarded::depth;
        __try
        {
            char path[128];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "Body/%s", panel);
            uint8_t buf[0x60] = {};
            void *tip = p_resolve(root, buf, path);
            if (p_valid(tip))
            {
                p_visible(tip, show ? 1 : 0);
                if (show)
                {
                    p_pos(tip, x, y);
                }
            }
            p_dtor(buf + 0x28);
            // The sprite carries two text variants; only ours is used, so the other stays off.
            _snprintf_s(path, sizeof(path), _TRUNCATE, "Body/%s/State_1", panel);
            uint8_t sb[0x60] = {};
            void *st1 = p_resolve(root, sb, path);
            if (p_valid(st1))
                p_visible(st1, 0);
            p_dtor(sb + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        --goblin::guarded::depth;
    }

    // A tip_pos_from_map(map_area, map_x, map_z, out2) stood here - the map-space to Body-space
    // conversion, `out = zoom * v - pan`. It had no callers: the live tooltip is laid out by
    // map_x_bounds() / map_middle_y() / popup_pos() instead, which place it against the panel
    // rather than against the marker. The constant it was written for, kWorldToScreen
    // (FUN_1409CC470), had a single occurrence in the tree - its own declaration - and went too.

    // Does this movie carry our own panels? Answered once, from the movie itself: a mod that ships
    // its own 02_120_worldmap is served by a route the load-time transform never sees, and then our
    // named placements are simply not there.
    void detect_own_tip(uintptr_t base, void *root)
    {
        if (g_have_own_tip >= 0)
            return;
        g_have_own_tip = panel_present(base, root, kTipPanel) ? 1 : 0;
        spdlog::info("[maphover] own panels in this movie: {}. {}", g_have_own_tip ? "yes" : "NO",
                     g_have_own_tip
                         ? "the height line and the focus banner go into our own panels"
                         : "the movie was replaced by a mod, so both borrow spare lines of the game's "
                           "own name popup (height on the last, banner on three before it) and are "
                           "placed against it so they stand still");
    }

    // Take the hover line off screen. The borrowed line has to be hidden EXPLICITLY: the game only
    // ever shows as many lines as its own pin has text for, so ours would otherwise stay up and hang
    // under the next place name the map shows.
    void clear_own_tip(uintptr_t base, void *root)
    {
        if (g_have_own_tip)
            tip_root(base, root, kTipPanel, false, 0, 0);
        else
            tip_line(base, root, kGamePanel, kTipLines - 1, nullptr, 0);
    }

    // Fill and place the hover line for `row`, or hide it when nothing is hovered. On a normal build
    // that is OUR clip, our lines, our text and our position, with the game's Body/PlaceName
    // untouched. When the movie was replaced (so our clip is absent) it is the spare last line of the
    // game's own popup, pulled to the same corner our panel would occupy.
    void drive_own_tip(void *map_area, void *row, bool have_map, float map_x, float map_z)
    {
        if (!map_area)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void *root =
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(map_area) - 0x27D8 + 0x120);
        detect_own_tip(base, root);
        // hover_info used to gate the OVERLAY's hover panel, retired in favour of this one. Honour
        // it here, or the setting silently becomes a no-op: the player turns hover info off and this
        // panel keeps showing. It is now the only consumer of that key.
        if (!goblin::config::enableHoverInfo)
        {
            clear_own_tip(base, root);
            return;
        }
        const goblin::HoveredMarker hm =
            row ? goblin::hovered_marker(row) : goblin::HoveredMarker{};
        (void)map_x;
        (void)map_z;
        if (!hm.matched || !have_map)
        {
            clear_own_tip(base, root);
            return;
        }
        // No name line: the game's own popup already shows it, so ours would only repeat it.
        // What the game does NOT show is how far above or below the player the marker sits.
        // Height relative to the player - the same three cases and the same 1.5 unit dead band
        // the overlay's hover panel used (goblin_overlay.cpp + collected::read_player_pos).
        wchar_t l1[192] = {};
        float px = 0.0f, pz = 0.0f, py = 0.0f;
        if (hm.posY != 0.0f && goblin::collected::read_player_pos(px, pz, py))
        {
            const float dy = hm.posY - py;
            const float ad = dy < 0 ? -dy : dy;
            // The localized sentences the overlay used, not a bare number: HoverLevel has no
            // placeholder, HoverAbove/HoverBelow take the distance.
            char utf8[192] = {};
            if (ad < 1.5f)
                _snprintf_s(utf8, sizeof(utf8), _TRUNCATE, "%s",
                            goblin::i18n::tr(goblin::i18n::TextId::HoverLevel));
            else
                _snprintf_s(utf8, sizeof(utf8), _TRUNCATE,
                            goblin::i18n::tr(dy > 0 ? goblin::i18n::TextId::HoverAbove
                                                    : goblin::i18n::TextId::HoverBelow),
                            ad);
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, l1, 192);
        }
        if (!l1[0])
        {
            clear_own_tip(base, root);  // nothing to say about this marker's height
            return;
        }
        // STATIC in the corner, like the overlay's hover panel was. Letting the vertical follow
        // the marker made it ride the map while panning, and worse: the reticle re-picks the
        // nearest marker every frame, so the panel also jumped between different ones.
        float left = 0.0f, right = 0.0f;
        map_x_bounds(map_area, &left, &right);
        const float tx = panel_left_x(left, right);
        const float ty = static_cast<float>(map_middle_y(map_area, kTipTopY));
        if (!g_have_own_tip)
        {
            // Borrowed line. Its host panel is the game's, and the game keeps that panel ON the
            // marker - so the line is placed against the panel rather than with it: the position we
            // give a line is relative to its panel, and subtracting where the panel currently is
            // leaves the line standing still at our corner while the name above keeps following the
            // pin. Without this the height line rode the popup down to the reticle.
            float hx = 0.0f, hy = 0.0f;  // where the host panel is
            if (popup_pos(g_popup_panel.load(std::memory_order_relaxed), &hx, &hy))
            {
                const float local[2] = {tx + kLineInsetX - hx, ty + kLineInsetY - hy};
                tip_line(base, root, kGamePanel, kTipLines - 1, l1, kTipColor, local);
            }
            else
            {
                // The popup's position is not established yet (nothing has been hovered since the
                // map opened). Show the text where the panel puts it rather than nowhere; the very
                // next frame has the position.
                tip_line(base, root, kGamePanel, kTipLines - 1, l1, kTipColor);
            }
            return;
        }
        tip_line(base, root, kTipPanel, 0, l1, kTipColor);
        for (int i = 1; i < kTipLines; ++i)
            tip_line(base, root, kTipPanel, i, nullptr, 0);
        tip_root(base, root, kTipPanel, true, static_cast<int>(tx), static_cast<int>(ty));
    }

    // Which lines of the GAME's popup the banner borrows when our own clip is absent. The last line is
    // the height line's, so the banner takes the three before it - the ones a place name never reaches
    // (the map's own pins carry one or two lines).
    constexpr int kBorrowBannerFirst = 3;
    constexpr int kBorrowBannerLines = 3;

    // The focus banner: the same two red lines the overlay put in the corner while a progress
    // category is isolated on the map. Its own panel, so it and the hover tooltip never fight.
    // `popup_hidden` says the game had nothing to name this frame, so it left its own popup hidden
    // with the lines it owns still carrying the last name - which the borrowed path has to undo, or
    // the banner would only ever appear while the reticle happens to sit on something.
    void drive_own_banner(void *map_area, bool popup_hidden)
    {
        if (!map_area)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void *root =
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(map_area) - 0x27D8 + 0x120);
        const int cat = goblin::focus_category();
        if (cat < 0)
        {
            if (g_have_own_tip)
                tip_root(base, root, kBannerPanel, false, 0, 0);
            else
                for (int i = 0; i < kBorrowBannerLines; ++i)
                    tip_line(base, root, kGamePanel, kBorrowBannerFirst + i, nullptr, 0);
            return;
        }
        // Line 1: what is being shown. Category name from its ini key (the same label the menu
        // and the overlay use), region name from the progress snapshot.
        const char *ckey =
            goblin::category_config_key(static_cast<goblin::generated::Category>(cat));
        char utf8[320] = {};
        const int32_t reg_id = goblin::focus_region();
        // One name copied under the progress lock - iterating the shared table from this thread would
        // race a rebuild() on the overlay thread (the same freed-container shape as the reticle cache).
        std::string reg_name_buf;
        goblin::progress::region_name(reg_id, reg_name_buf);
        const char *reg_name = reg_name_buf.c_str();
        // Two lines, not one: a line's text field is 454px wide, and the heading plus the
        // category plus the region ran past it and came out clipped.
        wchar_t wl[kBorrowBannerLines][320] = {};
        _snprintf_s(utf8, sizeof(utf8), _TRUNCATE, "%s",
                    goblin::i18n::tr(goblin::i18n::TextId::ProgressShowingOnly));
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wl[0], 320);
        char subject[320] = {};
        _snprintf_s(subject, sizeof(subject), _TRUNCATE, "%s - %s",
                    ckey ? goblin::i18n::entry_label(ckey) : "?", reg_name);
        MultiByteToWideChar(CP_UTF8, 0, subject, -1, wl[1], 320);
        // Line 2: how to clear it - the hint names the menu's own Reset row.
        char hint[320] = {};
        _snprintf_s(hint, sizeof(hint), _TRUNCATE,
                    goblin::i18n::tr(goblin::i18n::TextId::ProgressFocusResetHint),
                    goblin::i18n::tr(goblin::i18n::TextId::ProgressFocusClear));
        MultiByteToWideChar(CP_UTF8, 0, hint, -1, wl[2], 320);
        // Top-left corner: the map's own left bound, and far enough down that the first line
        // (authored at ty -39.3) is on screen.
        float left = 0.0f, right = 0.0f;
        map_x_bounds(map_area, &left, &right);
        const float bx = panel_left_x(left, right);
        const float by = static_cast<float>(kBannerTopY);
        if (!g_have_own_tip)
        {
            // Borrowed lines, placed against the game's popup the same way the height line is.
            void *ppanel = g_popup_panel.load(std::memory_order_relaxed);
            float px = 0.0f, py = 0.0f;
            if (!popup_pos(ppanel, &px, &py))
            {
                // The popup has no position of its own yet (nothing hovered since the map opened).
                // The banner does not depend on a hover, so it cannot wait for one: place the popup
                // ourselves, through the game's own placer so its next hover still moves it.
                const float at[2] = {bx, by};
                if (!popup_place(base, ppanel, at))
                    return;
                px = bx;
                py = by;
            }
            if (popup_hidden)
            {
                // Nothing was named this frame, so the lines the game owns go off with it - it left
                // them as the last name found them, and an empty line still draws its backdrop strip.
                // Only on such a frame: doing it when the game DID name something would blank a name
                // the player is reading. Through the game's own setter, so it can show them again.
                for (int i = 0; i < kBorrowBannerFirst; ++i)
                    game_line_hide(base, ppanel, i);
            }
            // Unconditional: the game hides its popup on every frame it has nothing to name, and our
            // lines live inside it. Showing one that is already visible costs a single property write.
            popup_show(base, ppanel, true);
            for (int i = 0; i < kBorrowBannerLines; ++i)
            {
                const float at[2] = {bx + kLineInsetX - px,
                                     by + kLineInsetY + kLinePitch * static_cast<float>(i) - py};
                tip_line(base, root, kGamePanel, kBorrowBannerFirst + i, wl[i], kBannerRed, at);
            }
            return;
        }
        for (int i = 0; i < kBorrowBannerLines; ++i)
            tip_line(base, root, kBannerPanel, i, wl[i], kBannerRed);
        for (int i = kBorrowBannerLines; i < kTipLines; ++i)
            tip_line(base, root, kBannerPanel, i, nullptr, 0);
        tip_root(base, root, kBannerPanel, true, static_cast<int>(bx), kBannerTopY);
    }

    void *placename_detour(void *panel, void *item, void *map_area)
    {
        // This hook fires once per frame on the game UI thread while the world map is open (the
        // dialog's per-frame Update calls it, item may be null). It drives the map's per-frame
        // work: today that is stall_probe::on_map_frame() (marker viewport reconcile + the
        // location-emphasis pass). It is NOT a replay driver - the deferred-relayout replay queue
        // this comment used to name was removed for good, and `fast_map_open` is a retired ini key.
        goblin::map_timing::on_map_frame();

        // Publish the popup panel and how many line slots its current variant has, so the
        // native tooltip can be driven with our own text and coordinates.
        if (panel)
        {
            g_popup_panel.store(panel, std::memory_order_relaxed);
            if (g_popup_lines.load(std::memory_order_relaxed) < 0)
            {
                int32_t slots = -1;
                __try
                {
                    const uintptr_t p = reinterpret_cast<uintptr_t>(panel);
                    const uint32_t variant = *reinterpret_cast<uint32_t *>(p + 0x19C0);
                    uintptr_t base = p + 0x108 + static_cast<uintptr_t>(variant) * 0xC80;
                    base += (-static_cast<intptr_t>(base)) & 7; // the game aligns it the same way
                    slots = static_cast<int32_t>(*reinterpret_cast<int64_t *>(base + 0xC08));
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    slots = -1;
                }
                g_popup_lines.store(slots, std::memory_order_relaxed);
                spdlog::info("[maphover] popup panel 0x{:X}: {} line slots",
                             reinterpret_cast<uintptr_t>(panel), slots);
            }
        }

        // One-shot: is the map movie's clip ROOT reachable the same way the keybinding dialog's
        // is (dialog + 0x120)? WorldMapDialog shares the MenuWindow base and is MapArea - 0x27D8
        // (the map-layer RE). If a known path resolves from it, our OWN popup can be a named
        // placement in the movie and be driven independently of the game's PlaceName clip.
        if (map_area && !g_root_probed.exchange(true, std::memory_order_acq_rel))
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            using ResolveFn = void *(void *parent, void *out, const char *path);
            using ValidFn = char(void *proxy);
            using DtorFn = void(void *proxy);
            auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(kResolve));
            auto p_valid = reinterpret_cast<ValidFn *>(goblin::anchors::at(kProxyValid));
            auto p_dtor = reinterpret_cast<DtorFn *>(goblin::anchors::at(kProxyDtor));
            char found[3] = {};
            if (p_resolve && p_valid && p_dtor)
            {
                __try
                {
                    void *root = reinterpret_cast<void *>(
                        reinterpret_cast<uintptr_t>(map_area) - 0x27D8 + 0x120);
                    static const char *const kPaths[] = {"Body", "Body/PlaceName",
                                                         "Body/PlaceName/State_0/Text_0/Text"};
                    for (int i = 0; i < 3; ++i)
                    {
                        uint8_t buf[0x60] = {};
                        void *r = p_resolve(root, buf, kPaths[i]);
                        found[i] = p_valid(r);
                        p_dtor(buf + 0x28);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
                spdlog::info("[maphover] map movie root via dialog+0x120: Body={} PlaceName={} "
                             "line0={}",
                             found[0] ? "yes" : "no", found[1] ? "yes" : "no",
                             found[2] ? "yes" : "no");
            }
            else
                spdlog::info("[maphover] map movie root probe skipped: the proxy-resolve helpers "
                             "were not placed on this build");
        }

        void *row = nullptr;
        if (item)
        {
            uintptr_t vt = *reinterpret_cast<uintptr_t *>(item);
            if (vt == g_pin_vt)
            {
                row = *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(item) + PIN_ROW_OFF);
                g_proxy_pin.store(item, std::memory_order_relaxed);
            }
        }

        // Publish the WorldMapArea (r8) if its vtable matches; it drives the projection.
        // Published BEFORE the proxy branch: native_reticle_row() projects through
        // map_dialog()/map_layer() and needs this frame's state.
        void *area = nullptr;
        uintptr_t avt = 0;
        if (g_maparea_vt && map_area && read_vt(map_area, avt) && avt == g_maparea_vt)
            area = map_area;
        else
        {
            static bool logged = false;
            if (!logged)
            {
                spdlog::info("[maphover] map_area.vt=0x{:X} != expected 0x{:X} (maparea AOB {})",
                             avt, g_maparea_vt, g_maparea_vt ? "ok" : "MISSED");
                logged = true;
            }
        }
        g_dialog.store(area, std::memory_order_relaxed);
        g_last_hook_ms.store(GetTickCount64(), std::memory_order_relaxed);

        // Native-tooltip proxy: mirror the engine's nearest-pin-wins rule with
        // our pinless markers in the pool. The engine picked the nearest of ITS
        // pins (or none); if our best candidate is closer to the reticle, borrow
        // a real pin - the live item when there is one, else the cached proxy -
        // re-point its row at ours for exactly this call, restore straight
        // after. The engine's own focus state is untouched.
        float ours_d2 = 0.0f, ours_mx = 0.0f, ours_mz = 0.0f;
        void *ours = goblin::native_reticle_row(&ours_d2, &ours_mx, &ours_mz);
        if (ours)
        {
            bool use_ours = false;
            if (row)
            {
                float item_d2 = 0.0f;
                use_ours = goblin::row_reticle_dist2(row, item_d2) && ours_d2 < item_d2;
            }
            else if (!item)
            {
                use_ours = true;
            }
            void *pin = nullptr;
            if (use_ours)
            {
                // The engine renders this frame's popup FROM THE PIN OBJECT IT IS HOLDING. So when
                // it has one, re-point THAT object - which is what the note above always said
                // ("borrow a real pin - the live item when there is one"). The code used to prefer
                // our own constructed pin instead, and because g_own_pin_ready latches on first
                // success it then took that branch forever: we wrote our row into an object the
                // engine was not drawing, reported success, and the player saw the engine's own
                // popup or nothing at all. Diagnosed 2026-07-31 from a live log where
                // `item=yes ... pin=own -> SHOWN` coincided with no popup on screen.
                pin = item;
                if (!pin && g_pin_ctor)
                {
                    if (!g_own_pin_ready && seh_construct_own_pin(ours, ours_mx, ours_mz))
                        g_own_pin_ready = true;
                    if (g_own_pin_ready)
                        pin = g_own_pin;
                }
                if (!pin)
                    pin = g_proxy_pin.load(std::memory_order_relaxed);
            }
            if (pin)
            {
                g_hovered_row.store(ours, std::memory_order_relaxed);
                auto *base = reinterpret_cast<uint8_t *>(pin);
                auto *slot = reinterpret_cast<void **>(base + PIN_ROW_OFF);
                auto *pos = reinterpret_cast<float *>(base + PIN_POS_OFF);
                auto *vis = base + PIN_VIS_OFF;
                void *orig_row = *slot;
                const float orig_pos0 = pos[0], orig_pos1 = pos[1];
                const uint8_t orig_vis = *vis;
                *slot = ours;
                pos[0] = ours_mx;
                pos[1] = ours_mz;
                *vis = 1;
                void *ret = o_placename(panel, pin, map_area);
                *slot = orig_row;
                pos[0] = orig_pos0;
                pos[1] = orig_pos1;
                *vis = orig_vis;
                drive_own_tip(map_area, ours, true, ours_mx, ours_mz);
                // The game was handed a pin, so its popup is up: the banner only has to add its own
                // lines to it.
                drive_own_banner(map_area, false);
                return ret;
            }
        }
        // The game hovered one of ITS pins: that pin's map position is where the reticle is, which
        // is the one measurement that says whether this build's reticle is centred or follows the
        // cursor. Read-only, and only when the engine handed us a pin.
        if (item)
        {
            __try
            {
                const float *pp = reinterpret_cast<const float *>(
                    reinterpret_cast<uint8_t *>(item) + PIN_POS_OFF);
                g_reticle_mx.store(pp[0], std::memory_order_relaxed);
                g_reticle_mz.store(pp[1], std::memory_order_relaxed);
                g_reticle_ms.store(GetTickCount64(), std::memory_order_relaxed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        g_hovered_row.store(row, std::memory_order_relaxed);
        // AFTER the game's own pass, not before: on a build whose movie was replaced, our lines live
        // inside the game's popup, and the game rewrites those lines (and hides the popup) every
        // frame. Running first meant writing text the same frame then wiped.
        void *ret = o_placename(panel, item, map_area);
        // Only the native reticle gives us MAP coordinates, and only our own rows ever match
        // hovered_marker() anyway - so that is the row the panel follows here.
        drive_own_tip(map_area, ours, ours != nullptr, ours_mx, ours_mz);
        // No pin of ours went in, so the game's popup is hidden unless the engine focused one of its
        // own pins - which is exactly what `item` says.
        drive_own_banner(map_area, item == nullptr);
        return ret;
    }
}  // namespace

void goblin::maphover::setup()
{
    // Resolve the two vtable gates by AOB (each from its vtable's ctor `lea rax,[rip+vt]`),
    // so a game update that moves the vtable doesn't silently misgate. relative_offsets
    // {{3,7}} yields the lea target = the vtable address. A miss leaves the gate 0.
    try
    {
        g_pin_vt = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 8D 05 ?? ?? ?? ?? 48 89 06 48 89 BE 30 02 00 00",
             .relative_offsets = {{3, 7}}}));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] pin-vtable AOB miss (hover/manual-hide disabled): {}", e.what());
    }
    try
    {
        g_maparea_vt = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 79 70 48 8B 07 48 8B CF FF 50 08",
             .relative_offsets = {{3, 7}}}));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] maparea-vtable AOB miss (layer/projection disabled): {}", e.what());
    }
    try
    {
        // Pin ctor (FUN_14087ba70): resolved via the unique wrapper-vftable lea
        // site inside it (0x14087bb82 in v2.6.2.0); function entry = site - 0x112.
        // A miss only disables the own-pin proxy (borrowed-pin fallback stays).
        auto *site = modutils::scan<uint8_t>(
            {.aob = "48 89 BE 30 02 00 00 48 8D 05 ?? ?? ?? ?? 48 89 86 38 02 00 00 "
                    "8B 45 08 89 86 40 02 00 00"});
        g_pin_ctor = reinterpret_cast<PinCtorFn *>(site - 0x112);
        spdlog::info("[maphover] pin factory resolved @ 0x{:X}",
                     reinterpret_cast<uintptr_t>(g_pin_ctor));
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[maphover] pin-factory AOB miss (own-pin proxy disabled, "
                     "borrowed-pin fallback active): {}",
                     e.what());
    }
    try
    {
        auto *fn = modutils::hook<PlaceNameFn>(
            {.aob = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 "
                    "48 C7 45 B7 FE FF FF FF 48 89 9C 24 08 01 00 00 48 8B 05 ?? ?? ?? ?? "
                    "48 33 C4 48 89 45 1F 49 8B F8"},
            placename_detour, o_placename);
        spdlog::info("[maphover] hover hook armed @ 0x{:X}", reinterpret_cast<uintptr_t>(fn));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] setup failed (marker hover/hide disabled): {}", e.what());
    }
    try
    {
        auto *bf = modutils::hook<BuildFn>(
            {.aob = "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF "
                    "FF FF 4C 8B F9 8B 42 34"},
            build_detour, o_build);
        spdlog::info("[maphover] buildMarkers hook armed @ 0x{:X}", reinterpret_cast<uintptr_t>(bf));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] buildMarkers hook failed (layer pin list disabled): {}", e.what());
    }
}

// The last position the GAME's own hover reported, in map space, with the time it arrived. That
// position is the reticle: the engine only reports a pin the reticle is on. Used to work out which
// anchor the build's map uses - see the calibration in goblin_overlay.cpp.
bool goblin::maphover::reticle_map(float *mx, float *mz, uint64_t max_age_ms)
{
    const uint64_t at = g_reticle_ms.load(std::memory_order_relaxed);
    if (!at || GetTickCount64() - at > max_age_ms)
        return false;
    *mx = g_reticle_mx.load(std::memory_order_relaxed);
    *mz = g_reticle_mz.load(std::memory_order_relaxed);
    return true;
}

bool goblin::maphover::reticle_live(float *mx, float *mz, bool *pointer_mode)
{
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return false;  // map closed - the dialog's fields are last frame's at best
    void *area = g_dialog.load(std::memory_order_relaxed);
    bool ptr_mode = false;
    if (!read_reticle_pair(area, mx, mz, &ptr_mode))
        return false;
    if (pointer_mode)
        *pointer_mode = ptr_mode;
    // Said once per session, and only once there is something to say it AGAINST: the game's own hover
    // reports a pin the reticle was ON, so the two being close is what says this is the field it
    // searches around - on a build nobody has run this on. Waiting for the sample matters; done on the
    // first read it fired on the frame the map opened, before anything had ever been focused, and
    // printed a comparison with nothing.
    static std::atomic<int> s_said{0};
    if (!s_said.load(std::memory_order_relaxed))
    {
        float rmx = 0.0f, rmz = 0.0f;
        if (goblin::maphover::reticle_map(&rmx, &rmz, 400) &&
            s_said.exchange(1, std::memory_order_relaxed) == 0)
        {
            const float d = (rmx - *mx) * (rmx - *mx) + (rmz - *mz) * (rmz - *mz);
            // The sample is a PIN, not the reticle itself, so it is only ever as close as the engine's
            // own focus radius. Well beyond that means a different field is being read.
            const bool agree = d < 300.0f * 300.0f;
            spdlog::info("[maphover] reticle from the dialog: ({:.0f},{:.0f}) via the {} pair; the "
                         "game's own focused pin: ({:.0f},{:.0f}) - {}",
                         *mx, *mz, ptr_mode ? "pointer" : "reticle", rmx, rmz,
                         agree ? "they agree, so this is the position it searches around"
                               : "THEY DISAGREE, so this is not that position");
        }
    }
    return true;
}

void *goblin::maphover::hovered_row()
{
    // Heartbeat gate: the hook only fires while the world map is open. If it hasn't
    // fired very recently the map is closed - report "no hover" so a stale pin from the
    // last session's map can't be acted on.
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return nullptr;
    return g_hovered_row.load(std::memory_order_relaxed);
}

// popup_panel() / popup_line_slots() were exported here and never called: the code inside this
// file that needs the panel reads g_popup_panel directly.

uint64_t goblin::maphover::last_activity_ms()
{
    return g_last_hook_ms.load(std::memory_order_relaxed);
}

void *goblin::maphover::map_dialog()
{
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return nullptr;  // map closed - do not hand back a stale dialog
    return g_dialog.load(std::memory_order_relaxed);
}

int goblin::maphover::map_layer()
{
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return -1;  // map closed
    // Currently DISPLAYED map, read from the WorldMapDialog. The dialog encodes both map
    // dimensions in one int at dialogBase+0x30DC: value = world*10 + sublayer, where
    // world 0 = Lands Between, 1 = Shadow Realm (DLC, a full canvas redraw); sublayer
    // 0 = surface, 1 = underground (the UG overlay drawn on top of OW). So 0 = OW(m60),
    // 1 = UG(m12), 10 = DLC surface(m61), 11 = DLC underground. Our HighlightPoint layer
    // is the WorldMapPointParam dispMask bit: 0=OW, 1=UG, 2=DLC (both DLC sublayers -> 2).
    // We reach the field via the MapArea we already publish (hover hook r8, vt rva
    // 0x2B2CB08): the dialog's per-frame Update (FUN_1409c32f0) calls the hover routine
    // with r8 = dialogBase + 0x27D8, so the field sits at MapArea + 0x904 (0x30DC-0x27D8).
    // Live-verified 2026-07-13 (OW=0, UG=1, DLC=10). This is the DISPLAYED tab, unlike the
    // old CS::FieldArea+0xDC read, which was a generation counter (rings stuck on DLC).
    void *area = g_dialog.load(std::memory_order_relaxed);
    if (!area) return -1;
    int v = -1;
    __try { v = *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(area) + 0x904); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (v < 0) return -1;
    if (v >= 10) return 2;      // Shadow Realm / DLC (m61), either sublayer
    return v;                   // 0 = overworld (m60), 1 = underground (m12)
}
