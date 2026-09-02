#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_set>
#include <vector>

// Forward-declare the generated Category enum instead of pulling the full
// (profile-scoped) goblin_map_data.hpp here: some TUs (goblin_gfx_probe.cpp)
// also include the err-path "generated/..." header explicitly, and having both
// the bare and the generated/ map-data header in one TU is a redefinition.
namespace goblin::generated { enum class Category : uint8_t; }

namespace goblin
{
    void inject_map_entries();

    // Public accessor: is this marker category currently shown (its show_*
    // config toggle on)? Used by the overlay's Progress tab. Named distinctly
    // from the internal is_category_enabled() to avoid a lookup clash.
    bool category_enabled(generated::Category cat);

    // Is a game event flag currently set? Reads the live EventFlagMan (resolves
    // the flag API by AOB on first use). Used by the Progress tab to count
    // flag-based pickups/kills (loot, bosses) that GEOF tracking doesn't cover.
    // Returns false if the flag id is 0 or the flag API isn't resolved yet.
    bool flag_is_set(uint32_t flag_id);

    // Progress-tab "focus" mode: when set, the live map shows ONLY the focused
    // category's UNCOLLECTED markers IN the focused region (everything else
    // hidden), swapped to the glow-highlight icon, so the player sees where its
    // remaining items are. Pass a generated::Category cast to int + the region's
    // PlaceName id (goblin::progress::region_place_id), or category = -1 to clear.
    // Stores the state; call reapply_live_settings() to apply.
    void set_focus_category(int category_or_negative, int32_t region_place_id = -1);
    int focus_category();
    int32_t focus_region();

    // The other focus shape: an explicit SET of markers (the item search's picks), keyed by
    // original_row_id (MAP_ENTRIES.row_id). While active, the live map shows ONLY those
    // markers (uncollected, same rules as the category focus) with the highlight ring on each.
    // Replaces any category focus; an empty set clears. set_focus_category(-1) clears it too.
    // Stores the state; call reapply_live_settings() to apply.
    void set_focus_rows(const std::vector<uint64_t> &original_row_ids);
    bool focus_rows_active();
    size_t focus_rows_count();

    // Search-index feed: one entry per injected marker, with the row's CURRENT label textId
    // (live-loot relabels and the anonymous-loot label included, so a spoiler-free player
    // cannot search past the "something" label), its progress region and category.
    struct SearchRow { uint64_t key; int32_t textId; int32_t region; uint8_t cat; };
    std::vector<SearchRow> search_row_snapshot();
    // Bumped whenever marker labels may have changed (live-loot refresh, every live-settings
    // re-apply). The search index rebuilds when it sees a new value.
    uint32_t label_epoch();

    // Swap focused rows to the glow-highlight icon (and others back to normal).
    // Called by reapply_live_settings() and after remap_injected_icons().
    void apply_focus_highlight();

    // World position of a marker to highlight on the open map, for the overlay's
    // projected ring (replaces the baked glow-icon swap: no reopen needed). area is
    // WorldMapPointParam.areaNo; world_x/z = gridNo*256 + pos. The overlay projects
    // these via goblin::mapproject. Thread-safe snapshot (row set is stable between
    // rebuilds; positions are read once here).
    // layer = WorldMapPointParam dispMask bit (0=overworld/M00, 1=underground/M01,
    // 2=DLC/M02, 0xFF=none) - the map layer this marker belongs to. The overlay draws it
    // only when the displayed layer (goblin::maphover::map_layer) matches. gx/gz/px/pz are
    // the RAW area-local grid + pos (the projector folds them per the marker's area).
    struct HighlightPoint { uint8_t area; uint8_t layer; uint16_t gx; uint16_t gz; float px; float pz; };
    // The current focus set (focus category+region) - only markers whose icon is
    // currently SHOWN (not collected, not kindling-collected, not manually hidden, and
    // no live hide/cleared flag set). Empty when no focus is active.
    std::vector<HighlightPoint> focus_highlight_points();

    /// Where one of OUR markers is DRAWN, by its live param row pointer, which is not where its row
    /// says it is: the live de-overlap moves markers that would land on each other. false = not one of
    /// ours. The beacon dump reports both, because reading the row alone answers a question nobody
    /// asked once the drawing stopped coming from it.
    bool display_position(const void *rowptr, float &px, float &pz);

    // Lightweight native-marker input. During the category-by-category debug
    // rollout, only rows whose stock WorldMapItem has been suppressed are
    // returned. The native manager consumes this snapshot on the map thread.
    struct NativeMarkerPoint
    {
        uint64_t original_row_id;
        int source_icon_id;
        uint8_t area;
        uint8_t layer;
        uint16_t gx;
        uint16_t gz;
        float px;
        float pz;
        bool visible;
        void *rowptr; // live WorldMapPointParam row (hover tooltip / manual hide)
    };
    // Key-space bit for the twin "cleared" checkmark points: defeat-capable
    // rows emit a second NativeMarkerPoint whose original_row_id carries this
    // bit and whose icon is the green check badge; its visibility is
    // base-visible AND defeat-flag-set. Real row ids never reach bit 63.
    constexpr uint64_t NATIVE_CLEARED_KEY_BIT = 1ull << 63;

    // Focus-ring points: a small FIXED pool of extra native children, keyed by this bit plus
    // the pool index. They exist from the seed build (the icon factory only pulses during a
    // build burst, so anything not created then can never appear) and are then MOVED onto the
    // focused markers live - which is why the pool is a pool and not one ring per row: one ring
    // per row would mean ~9500 extra children, and every attached child costs driver-side
    // teardown on map close.
    constexpr uint64_t NATIVE_HIGHLIGHT_KEY_BIT = 1ull << 62;
    constexpr size_t NATIVE_RING_POOL = 128;

    // include_hidden: still return every row when the master switch is off, each one forced
    // invisible. The marker manager needs that (a row it never created cannot be switched back
    // on without reopening the map); the hover pick and the rings want the default empty answer.
    std::vector<NativeMarkerPoint> native_marker_snapshot(int layer, bool include_hidden = false);

    // Live row of the VISIBLE native marker nearest the map reticle, within a
    // pin-sized radius - or nullptr. The reticle is NOT assumed to be the screen
    // centre: it is read from the map dialog when the game exposes it, and inferred
    // (centre vs cursor, measured once per session) only when it cannot be. The
    // native-tooltip proxy: called once per frame from the map hover hook (our raw
    // display objects are invisible to the engine hit test). out_dist2 (optional)
    // receives the canvas-space squared distance for nearest-wins arbitration vs
    // engine pins; out_map_x/out_map_z receive the marker's map-space position (the
    // units of CS::WorldMapPointPinData+0x10, which anchors the engine name panel).
    void *native_reticle_row(float *out_dist2 = nullptr,
                             float *out_map_x = nullptr, float *out_map_z = nullptr);

    // Same canvas-space squared reticle distance for ANY live WorldMapPointParam
    // row (engine pin rows included) - the shared metric of that arbitration.
    // It resolves the anchor through the SAME function native_reticle_row uses,
    // which is the property that matters: until 2026-07-31 this one measured from
    // the view centre while that one had already moved to the live reticle, so the
    // caller compared distances taken from different points and picked the wrong
    // marker at full zoom-out.
    // Precisely: same anchor FUNCTION, evaluated a second time - not the same
    // stored value. On the normal path both evaluations read the same published
    // reticle and agree exactly. They can differ by a pixel or two only in the
    // fallback regime, where the anchor is the OS cursor and the two calls sample
    // it separately; that is well inside the pin-sized pick radius, so it cannot
    // flip the arbitration. Call it once per frame - it is not loop-cheap.
    bool row_reticle_dist2(const void *rowptr, float &out_dist2);

    // Original (pre-remap) row ids of injected markers whose icon is currently HIDDEN
    // for any reason (collected / kindling / manually hidden / a live disable or cleared
    // flag is set). The region-progress tab counts these as done. Reads the LIVE rows so
    // live-loot flag rewrites and manual hide (both keyed on the live param) are seen.
    std::unordered_set<uint64_t> hidden_marker_original_ids();

    // (is_row_ptr_hidden(void*) was declared here for a hover-tooltip consumer that was never
    //  written. See the note at its former definition in goblin_inject.cpp.)

    // If a category focus is active but NO shown marker remains in it (all collected /
    // hidden), clear the focus. Returns true if it cleared (caller reapplies visibility).
    bool prune_focus_if_empty();

    // The ini config key ("show_*") for a category, or nullptr. Lets the Progress tab
    // reuse the Settings-tab icon + localized entry label for each category.
    const char *category_config_key(generated::Category cat);

    // Data pointers of MFG-injected WorldMapPointParam rows in the expanded
    // table. Populated by inject_map_entries(); consumed by
    // sanitize_injected_textids() after the FMG bank is built.
    const std::vector<uint8_t *> &injected_row_ptrs();

    // Remap every marker whose iconId is a custom icon we injected to that icon's runtime-injected frame
    // (gfx_probe::injected_iconid). Called from the worldmap-load hook after the icon frames are appended,
    // before pins are built. Lets our embedded bitmaps render without shipping a custom gfx, on any base.
    void remap_injected_icons();

    // Rewrites rows baked with a primary completion flag to its alternative
    // once the alternative flag turns on (quest fights with two mutually-
    // exclusive outcome flags, e.g. the Sellen/Jerren academy battle).
    // Called periodically from the refresh loop.
    void apply_flag_or_pairs();

    // Live-loot (config::liveLootFlags): read each loot marker's source
    // ItemLotParam row from live memory and set textDisableFlagId1 to the
    // lot's current getItemFlagId, so markers hide on the actual light-point
    // pickup for the loaded regulation (Item/Enemy Randomizer compatible).
    // One-shot, called once after inject_map_entries().
    void refresh_loot_from_itemlot();

    // Toggle the WorldMapPointParam swap between vanilla and expanded states.
    // Used as an ERSC-hosting workaround: revert before host, re-apply after.
    void set_param_injection_active(bool active);
    // (is_param_injection_active() was declared here and had no callers: menu_auto_toggle_loop
    //  reads the static behind it directly. Its twin setter IS live and stays.)

    // Live marker visibility: sets each injected row's textEnableFlagId1 so the
    // primary line/icon shows only when its category is enabled AND the row is
    // not collected. The engine re-evaluates this flag every frame, so the
    // effect is INSTANT on the open map (no reopen). Call after the overlay
    // changes a toggle, and from the refresh thread when the collected set
    // changes. Idempotent.
    void apply_category_visibility();

    // ---- Manual per-marker hide (hover + hotkey; overlay-managed) ----------
    // Toggle the hidden state of the marker whose live WorldMapPointParam row is
    // `rowptr` (the pin under the cursor, from goblin::maphover::hovered_row()).
    struct ManualHideResult { bool matched = false; bool now_hidden = false; int32_t textId = 0; };
    ManualHideResult toggle_hovered_marker(void *rowptr);
    // Read-only lookup of the hovered marker (for the passive hover-info overlay):
    // matches the live row ptr to one of our injected rows and returns its name
    // textId + source world Y (WorldMapPointParam.posY carries the MSB part Y).
    struct HoveredMarker { bool matched = false; int32_t textId = 0; float posY = 0.0f;
                           uint8_t area = 0; float world_x = 0.0f; float world_z = 0.0f; };
    HoveredMarker hovered_marker(void *rowptr);
    // Snapshot of the hidden set for the overlay manager.
    struct HiddenMarkerInfo { uint64_t key; int32_t textId; uint16_t iconId; int32_t region; uint8_t cat; };
    std::vector<HiddenMarkerInfo> manual_hidden_snapshot();
    size_t manual_hidden_count();
    void unhide_marker(uint64_t key);   // remove one from the hidden set
    void clear_manual_hidden();         // remove all
    void load_manual_hidden(const std::filesystem::path &path);  // low-level read
    void save_manual_hidden(const std::filesystem::path &path);  // low-level write
    void set_hidden_dir(const std::filesystem::path &dir);       // folder for per-slot hide files
    void persist_manual_hidden();                                // save to the current slot's file

    // Active save-slot / profile index (0..9) of the loaded character, or -1 when none is
    // loaded. Read live from GameMan (AOB-resolved slot, "game_man_slot") + 0xAC0
    // ("Save Slot (Profile Index)"). SEH-guarded. Manual hides are scoped per slot.
    int active_save_slot();
    // If the active slot changed since the last call, switch the persist target to that
    // slot's file (MapForGoblins_hidden_s<N>.txt) and reload the hidden set (empty when no
    // character is loaded). Returns true if the set changed (caller should reapply
    // visibility). Call from the watcher loop.
    bool sync_hidden_slot();

    // Force World Map fragment markers visible regardless of require_map_fragments
    // (config::worldMapsIgnoreFragments). Call AFTER apply_map_logic. Idempotent.
    void apply_worldmap_fragment_bypass();

    // Live re-apply of hide_killed_bosses across boss/hawk/hostile-NPC rows.
    void apply_kill_display();

    // Re-apply ALL live-capable settings at once (show_* + hide_killed_bosses +
    // require_map_fragments + ERR patch markers). Call from the overlay on a
    // settings change; effective on next world-map open. (anonymous_loot /
    // live_loot_* are not live - they rewrite markers from ItemLotParam.)
    void reapply_live_settings();

    // Master show/hide of ALL icons (the former F10 behavior), now driven from
    // the in-game overlay. The watcher (menu_auto_toggle_loop) applies it.
    void set_icons_hidden(bool hidden);
    bool icons_hidden();

    // Bumped by anything that changes which markers SHOULD be visible: the master switch and
    // every live-settings re-apply. The native-marker manager watches it so a toggle reaches
    // children that ALREADY exist without waiting for the build queue - see the tick, where
    // the periodic refresh is deliberately gated on a spent queue and this is not.
    uint32_t visibility_epoch();

    // Same bump, for a caller that has ALREADY rewritten the rows and only needs the tick to
    // notice. The background watcher is the one: it compares the collected/kindling counts every
    // two seconds and re-applies category visibility on a change, but it used to do that
    // silently, so the only thing that ever picked the change up was the tick's periodic
    // re-snapshot. That poll cost 4.3 ms of one map frame EVERY second (measured on
    // convergence3, 8425 rows, 2026-08-07) to look for something it usually could not find -
    // in single-player the world is paused while the map is up, so nothing can be collected at
    // all. With this the change announces itself, the tick merges on the next frame instead of
    // up to a second later, and the poll drops to a rare safety net.
    void note_visibility_changed();

    // Codex-toast ids, allocated DYNAMICALLY at runtime above the live max so they
    // never collide with an overhaul's / another mod's tutorial content (same
    // principle as the marker textId remap). Two independent id spaces:
    //   g_toast_fmg_id[]       = TutorialBody.fmg text ids (set by setup_messages,
    //                            above the live TutorialBody max). The TutorialParam
    //                            row's textId field points here, and the FMG text
    //                            lives here.
    //   g_toast_param_row_id[] = TutorialParam row ids (set by inject_tutorial_popup_rows,
    //                            above the live TutorialParam max). show_codex_toast
    //                            triggers a banner by one of THESE.
    // setup_messages MUST run before inject_tutorial_popup_rows (the popup rows
    // point at the fmg ids). 0 until allocated.
    enum ToastSlot { TOAST_ON = 0, TOAST_OFF, TOAST_DUMP_OK, TOAST_DUMP_FAIL, TOAST_COUNT };
    extern int g_toast_fmg_id[TOAST_COUNT];
    extern int g_toast_param_row_id[TOAST_COUNT];

    // Native settings-menu texts: GR_MenuText.fmg ids allocated dynamically
    // above the live max by setup_messages (same principle as the toast ids).
    // The native menu (goblin_stall_probe) builds its tab category label and its
    // page row labels via the game's MenuTextCtor against these ids. 0/empty
    // until allocated / if the merge failed.
    extern int g_menutext_tab_id;
    extern std::vector<int> g_menutext_row_ids; // parallel to native_menu_rows()
    // Value texts for rows rendered on the native keybinding form (its rows show a
    // name plus a VALUE column): localized "On"/"Off". 0 if the merge failed.
    extern int g_menutext_on_id;
    extern int g_menutext_off_id;

    // Rows of our native settings page: each a native checkbox bound directly to
    // a config bool. ini_key doubles as the i18n entry_labels lookup key (the
    // localized label text injected into GR_MenuText). Defined in
    // goblin_stall_probe.cpp (the native-menu owner).
    struct NativeMenuRowDef
    {
        const char *ini_key;
        bool *value;
        bool def; // schema default (drives the row's "modified" indicator)
    };
    const NativeMenuRowDef *native_menu_rows(size_t *count);

    // Inject the codex-toast TutorialParam rows for the F10/F9 banners. Rows
    // get menuType=0 (upper-left codex caption widget) with textId pointing
    // at TutorialBody.fmg entries injected by goblin_messages.
    // Returns true on success.
    bool inject_tutorial_popup_rows();

    // Fire an upper-left codex-style toast for one of the injected TutorialParam
    // rows (pass a goblin::g_toast_param_row_id[...] value). Static text, no FMG
    // rewrite - same path as the F10 banner.
    // ONLY CALL THIS FROM THE UI THREAD. It runs the game's own tutorial-popup routine, which
    // builds on-screen menu state; the "safe from any thread" this comment used to claim was
    // wrong. Measured 2026-07-31 in a player log: with menu_enabled=false the toggle key is the
    // icon master switch, the user flipped it seven times in eighteen seconds, every flip fired
    // this from the 10 ms polling thread, and 0.8 s after the last one the game died in Scaleform
    // (exe+0x1177413, a null container). Off-thread callers must use queue_codex_toast.
    void show_codex_toast(int tutorial_id);

    // Thread-safe request for the toast above: stores the id and returns. pump_codex_toast()
    // fires it from the UI thread (the CSMenuMan::updateTask detour drives it).
    void queue_codex_toast(int tutorial_id);
    void pump_codex_toast();

    // Background thread polling the toggle hotkey.
    void toggle_hotkey_loop();

    // Background thread owning the WorldMapPointParam expand/revert state. It
    // applies the F10/gamepad personal show/hide and shows the toggle banner.
    void menu_auto_toggle_loop();
};
