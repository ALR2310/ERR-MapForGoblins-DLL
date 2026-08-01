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

#include "goblin_collected.hpp"  // read_player_pos() for the height line
#include "goblin_i18n.hpp"    // the localized hover sentences
#include "goblin_progress.hpp" // region names for the focus banner    // the localized hover sentences
#include "goblin_inject.hpp"
#include "goblin_messages.hpp" // lookup_text() for the marker name     // native_reticle_row() - the native-tooltip proxy
#include "goblin_map_timing.hpp"
#include "goblin_stall_probe.hpp"
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
    std::atomic<int32_t> g_popup_lines{-1};
    std::atomic<bool> g_root_probed{false};

    std::atomic<void *> g_hovered_row{nullptr};
    std::atomic<void *> g_dialog{nullptr};
    std::atomic<void *> g_map_owner{nullptr};  // buildMarkers `this` (r15); its +0x398 map = current-layer pins
    std::atomic<uint64_t> g_last_hook_ms{0};

    // buildMarkers (0x140A82A80): builds the CURRENT map layer's pins into [owner+0x398]
    // (visibility-filtered, fold already applied). We capture `owner` so the overlay can
    // read that live current-layer pin set. __fastcall(rcx=owner, rdx=layer-context).
    using BuildFn = void *(void *, void *, void *, void *);
    BuildFn *o_build = nullptr;

    std::atomic<void *> g_dialog_data{nullptr};  // buildMarkers param_2 (dialogData)

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
        // ctx = dialogData. The displayed map id lives at *(int*)(dialogData+8) and its top
        // byte is the area (60=overworld, 12=underground, 61=DLC) - it updates live on layer
        // switch (FUN_1401b9390). We keep the pointer and read it live in map_layer().
        g_dialog_data.store(ctx, std::memory_order_relaxed);
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

    bool read_vt(void *obj, uintptr_t &out)
    {
        __try { out = *reinterpret_cast<uintptr_t *>(obj); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ── our own hover panel ──────────────────────────────────────────────────────────
    // Body/MfgTip is a second named placement of the tooltip sprite, added by the load-time
    // transform. It is OURS: own position, own line count, own text. The game's Body/PlaceName is
    // never read or written here.
    //
    // Coordinates: the pin's world position (item->vt[0x20]) is converted by the game's own
    // FUN_1409CC470(mapArea, &out2f, v). Reusing that converter is deliberate - re-deriving the
    // projection would drift from the map's view transform on every zoom/pan change.
    constexpr uintptr_t kSetVisible = 0x733340;
    constexpr uintptr_t kSetPosI = 0x7331A0;
    constexpr uintptr_t kSetTextHtml = 0x74A000;
    constexpr uintptr_t kResolve = 0x74A2F0;
    constexpr uintptr_t kProxyValid = 0x733150;
    constexpr uintptr_t kProxyDtor = 0xD7F850;
    constexpr uintptr_t kWorldToScreen = 0x9CC470;
    // Text colour. These fields are html=0, so markup is not an option here - unlike the menu,
    // where the same job is done with a <font> tag.
    constexpr uintptr_t kSetTextColor = 0x74A1D0;
    constexpr int kTipLines = 8;
    constexpr const char *kTipPanel = "MfgTip";
    constexpr const char *kBannerPanel = "MfgBanner";
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
        __try
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(map_area);
            *left = *reinterpret_cast<float *>(a + 0x340);
            *top = *reinterpret_cast<float *>(a + 0x344);
            *right = *reinterpret_cast<float *>(a + 0x348);
            *bottom = *reinterpret_cast<float *>(a + 0x34C);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
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

    // POD-only worker (SEH): set one of our lines, or hide it.
    void tip_line(uintptr_t base, void *root, const char *panel, int i, const wchar_t *text,
                  uint32_t rgb)
    {
        auto p_resolve = reinterpret_cast<void *(*)(void *, void *, const char *)>(base + kResolve);
        auto p_valid = reinterpret_cast<char (*)(void *)>(base + kProxyValid);
        auto p_dtor = reinterpret_cast<void (*)(void *)>(base + kProxyDtor);
        auto p_visible = reinterpret_cast<void (*)(void *, char)>(base + kSetVisible);
        auto p_settext =
            reinterpret_cast<void (*)(void *, const wchar_t *)>(base + kSetTextHtml);
        __try
        {
            char path[128];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "Body/%s/State_0/Text_%d", panel, i);
            uint8_t buf[0x60] = {};
            void *line = p_resolve(root, buf, path);
            if (p_valid(line))
            {
                p_visible(line, text ? 1 : 0);
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
                            reinterpret_cast<void (*)(void **, uint32_t)>(base + kSetTextColor)(
                                &fldp, rgb);
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
    }

    // POD-only: show/hide our panel root and place it.
    void tip_root(uintptr_t base, void *root, const char *panel, bool show, int x, int y)
    {
        auto p_resolve = reinterpret_cast<void *(*)(void *, void *, const char *)>(base + kResolve);
        auto p_valid = reinterpret_cast<char (*)(void *)>(base + kProxyValid);
        auto p_dtor = reinterpret_cast<void (*)(void *)>(base + kProxyDtor);
        auto p_visible = reinterpret_cast<void (*)(void *, char)>(base + kSetVisible);
        auto p_pos = reinterpret_cast<void (*)(void *, int32_t, int32_t)>(base + kSetPosI);
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
                    p_pos(tip, x, y);
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
    }

    // Map space -> the space Body's children live in. The game's converter (FUN_1409CC470) is
    // just `out = zoom * v - pan` over WorldMapArea +0x380 (zoom) / +0x378,+0x37C (pan), and its
    // `v` is a plain float[2] in MAP space - so ANY point of ours can be placed, with no pin
    // involved. That matters: our markers are drawn by the native path and have no game pin, so
    // the first attempt (which needed one) never showed anything on hover.
    bool tip_pos_from_map(void *map_area, float map_x, float map_z, float *out2)
    {
        __try
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(map_area);
            const float pan_x = *reinterpret_cast<float *>(a + 0x378);
            const float pan_z = *reinterpret_cast<float *>(a + 0x37C);
            const float zoom = *reinterpret_cast<float *>(a + 0x380);
            out2[0] = zoom * map_x - pan_x;
            out2[1] = zoom * map_z - pan_z;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Fill and place OUR panel for `row`, or hide it when nothing is hovered. Everything here is
    // ours: our clip, our lines, our text, our position. The game's Body/PlaceName is untouched.
    void drive_own_tip(void *map_area, void *row, bool have_map, float map_x, float map_z)
    {
        if (!map_area)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void *root =
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(map_area) - 0x27D8 + 0x120);
        const goblin::HoveredMarker hm =
            row ? goblin::hovered_marker(row) : goblin::HoveredMarker{};
        (void)map_x;
        (void)map_z;
        if (!hm.matched || !have_map)
        {
            tip_root(base, root, kTipPanel, false, 0, 0);
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
        tip_line(base, root, kTipPanel, 0, l1[0] ? l1 : nullptr, kTipColor);
        for (int i = 1; i < kTipLines; ++i)
            tip_line(base, root, kTipPanel, i, nullptr, 0);
        // STATIC in the corner, like the overlay's hover panel was. Letting the vertical follow
        // the marker made it ride the map while panning, and worse: the reticle re-picks the
        // nearest marker every frame, so the panel also jumped between different ones.
        float left = 0.0f, right = 0.0f;
        map_x_bounds(map_area, &left, &right);
        tip_root(base, root, kTipPanel, true, static_cast<int>(left + kPanelLeftInset),
                 map_middle_y(map_area, kTipTopY));
    }

    // The focus banner: the same two red lines the overlay put in the corner while a progress
    // category is isolated on the map. Its own panel, so it and the hover tooltip never fight.
    void drive_own_banner(void *map_area)
    {
        if (!map_area)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void *root =
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(map_area) - 0x27D8 + 0x120);
        const int cat = goblin::focus_category();
        if (cat < 0)
        {
            tip_root(base, root, kBannerPanel, false, 0, 0);
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
        _snprintf_s(utf8, sizeof(utf8), _TRUNCATE, "%s",
                    goblin::i18n::tr(goblin::i18n::TextId::ProgressShowingOnly));
        wchar_t w0[320] = {};
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w0, 320);
        tip_line(base, root, kBannerPanel, 0, w0, kBannerRed);
        char subject[320] = {};
        _snprintf_s(subject, sizeof(subject), _TRUNCATE, "%s - %s",
                    ckey ? goblin::i18n::entry_label(ckey) : "?", reg_name);
        wchar_t wsub[320] = {};
        MultiByteToWideChar(CP_UTF8, 0, subject, -1, wsub, 320);
        tip_line(base, root, kBannerPanel, 1, wsub, kBannerRed);
        // Line 2: how to clear it - the hint names the menu's own Reset row.
        char hint[320] = {};
        _snprintf_s(hint, sizeof(hint), _TRUNCATE,
                    goblin::i18n::tr(goblin::i18n::TextId::ProgressFocusResetHint),
                    goblin::i18n::tr(goblin::i18n::TextId::ProgressFocusClear));
        wchar_t w1[320] = {};
        MultiByteToWideChar(CP_UTF8, 0, hint, -1, w1, 320);
        tip_line(base, root, kBannerPanel, 2, w1, kBannerRed);
        for (int i = 3; i < kTipLines; ++i)
            tip_line(base, root, kBannerPanel, i, nullptr, 0);
        // Top-left corner: the map's own left bound, and far enough down that the first line
        // (authored at ty -39.3) is on screen.
        float left = 0.0f, right = 0.0f;
        map_x_bounds(map_area, &left, &right);
        tip_root(base, root, kBannerPanel, true,
                 static_cast<int>(left + kPanelLeftInset), kBannerTopY);
    }

    void *placename_detour(void *panel, void *item, void *map_area)
    {
        // This hook fires once per frame on the game UI thread while the world map
        // is open (the dialog's per-frame Update calls it, item may be null) - it is
        // the per-frame driver for fast_map_open's deferred-relayout replay.
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
            auto p_resolve = reinterpret_cast<ResolveFn *>(base + 0x74A2F0);
            auto p_valid = reinterpret_cast<ValidFn *>(base + 0x733150);
            auto p_dtor = reinterpret_cast<DtorFn *>(base + 0xD7F850);
            char found[3] = {};
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
                // Preferred: our own engine-constructed pin. Fallback when the
                // ctor AOB missed: borrow the live item (or the cached pin).
                if (g_pin_ctor)
                {
                    if (!g_own_pin_ready && seh_construct_own_pin(ours, ours_mx, ours_mz))
                        g_own_pin_ready = true;
                    if (g_own_pin_ready)
                        pin = g_own_pin;
                }
                if (!pin)
                    pin = item ? item : g_proxy_pin.load(std::memory_order_relaxed);
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
                drive_own_banner(map_area);
                return ret;
            }
        }
        g_hovered_row.store(row, std::memory_order_relaxed);
        // Only the native reticle gives us MAP coordinates, and only our own rows ever match
        // hovered_marker() anyway - so that is the row the panel follows here.
        drive_own_tip(map_area, ours, ours != nullptr, ours_mx, ours_mz);
        drive_own_banner(map_area);
        return o_placename(panel, item, map_area);
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

void *goblin::maphover::hovered_row()
{
    // Heartbeat gate: the hook only fires while the world map is open. If it hasn't
    // fired very recently the map is closed - report "no hover" so a stale pin from the
    // last session's map can't be acted on.
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return nullptr;
    return g_hovered_row.load(std::memory_order_relaxed);
}

void *goblin::maphover::popup_panel() { return g_popup_panel.load(std::memory_order_relaxed); }
int32_t goblin::maphover::popup_line_slots()
{
    return g_popup_lines.load(std::memory_order_relaxed);
}

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
