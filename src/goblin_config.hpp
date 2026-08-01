#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace goblin
{
    // Create the ini from the schema defaults if missing; otherwise migrate it
    // in place (add new keys, apply renames, comment out obsolete keys),
    // preserving user-set values. Then load values into goblin::config.
    void load_config(const std::filesystem::path &ini_path);
    void ensure_ini(const std::filesystem::path &ini_path);

    // Write the CURRENT live config back to the ini (used by the in-game config
    // overlay's "Save" button). Bool/U8 values come from the live config vars;
    // key/gamepad bindings are preserved from the existing file. Keeps the
    // schema's section layout + comments.
    void save_config(const std::filesystem::path &ini_path);

    // Path of the ini load_config() last used; the overlay saves back here.
    extern std::filesystem::path g_ini_path;

    namespace config
    {
        extern bool requireMapFragments;
        extern bool debugLogging;

        // Location emphasis: markers of the map the player is standing in draw at
        // ownScale, markers of any OTHER map at otherScale. A dungeon lies under the
        // overworld, so both sets share the same patch of map; the size difference is
        // what says which is which without hovering.
        extern bool locationEmphasis;
        extern float locationEmphasisOwnScale;
        extern float locationEmphasisOtherScale;
        // Colour half of the same lever: how much a marker of another map fades. Our icons
        // are premultiplied, so one factor scales colour and alpha together = a clean fade.
        extern float locationEmphasisOtherFade;
        // ...and how far its hue is pushed cold on top of that (0 = none, 1 = full). True
        // desaturation is out of reach - a colour transform is per-channel and cannot mix
        // luminance back in - so the nearest honest thing is holding blue while dropping
        // red and green.
        extern float locationEmphasisOtherCool;
        // The map-open levers (fast open, self-detach at close, near-view attach window) are no longer
        // ini keys: they are compile-time variants in goblin_build_variants.hpp.
        // NOTE: icon/resource injection is unconditional now (no ini toggle) - it's how icons render without
        // a gfx. The dev-only SpriteDef/dict dumps + RM2 trace in goblin_gfx_probe are gated by debugLogging.

        // Equipment
        extern bool showArmaments;
        extern bool showArmour;
        extern bool showAshesOfWar;
        extern bool showSpirits;
        extern bool showTalismans;

        // Key items
        extern bool showCelestialDew;
        extern bool showCookbooks;
        extern bool showCrystalTears;
        extern bool showImbuedSwordKeys;
        extern bool showLarvalTears;
        extern bool showScadutreeFragments;
        extern bool showGreatRunes;
        extern bool showLostAshes;
        extern bool showPotsNPerfumes;
        extern bool showSeedsTears;
        extern bool showWhetblades;

        // Loot
        extern bool showAmmo;
        extern bool showBellBearings;
        extern bool showMerchantBellBearings;
        extern bool showConsumables;
        extern bool showCraftingMaterials;
        extern bool showMPFingers;
        extern bool showMaterialNodes;
        extern bool showReusables;
        extern bool showSmithingStones;
        extern bool showSmithingStonesLow;
        extern bool showSmithingStonesRare;
        extern bool showGoldenRunes;
        extern bool showGoldenRunesLow;
        extern bool showStoneswordKeys;
        extern bool showThrowables;
        extern bool showPrattlingPates;
        extern bool showRuneArcs;
        extern bool showDragonHearts;
        extern bool showGloveworts;
        extern bool showGreatGloveworts;
        extern bool showGestures;
        extern bool showGreases;
        extern bool showUtilities;
        extern bool showStatBoosts;
        extern bool showFortunes;
        extern bool showHostileNPC;

        // Magic
        extern bool showIncantations;
        extern bool showMemoryStones;
        extern bool showPrayerbooks;
        extern bool showSorceries;

        // World - Bosses
        extern bool showBosses;
        extern bool hideKilledBosses;  // true=hide killed icons, false=green checkmark

        // Compatibility
        extern bool liveLootFlags;  // read live ItemLotParam getItemFlagId at runtime
                                    // → loot markers hide on the actual light-point
                                    // pickup for the current regulation (Randomizer-safe)
        extern bool liveLootLabels; // read live ItemLotParam item+category at runtime
                                    // → loot marker name shows the item the lot now
                                    // gives (Randomizer-safe). Needs full-band FMG copy.
        extern bool liveLootIcons;  // re-icon & re-gate loot markers by the LIVE item's
                                    // category at inject (so a randomized item shows its
                                    // own icon under its own show_* toggle).
        extern bool anonymousLoot;  // spoiler-free mode: every loot marker shows a
                                    // gray "?" icon + a generic localized label instead
                                    // of the real item (blind randomizer runs).

        // Quest
        extern bool showDeathroot;
        extern bool showProgression;
        extern bool showSeedbedCurses;

        // Reforged
        extern bool showEmberPieces;
        extern bool showItemsAndChanges;
        extern bool showRunePieces;

        // World
        extern bool showGraces;
        extern bool showImpStatues;
        extern bool showWorldMaps;
        extern bool worldMapsIgnoreFragments;
        extern bool showPaintings;
        extern bool showSpiritSprings;
        extern bool showSpiritspringHawks;
        extern bool showStakesOfMarika;
        extern bool showSummoningPools;
        extern bool showKindlingSpirits;
        extern bool showInteractables;

        // ── ERR Markers ─────────────────────────────────────────────────
        // Patches to ERR's pre-placed WorldMapPointParam entries (camps,
        // merchants, field bosses, dungeon entrances). Each `patch*` flag
        // decides whether we rewrite that category's flags so the marker
        // appears/hides in sync with map-fragment discovery and (for
        // dungeons) boss completion. `false` = leave the row alone - the
        // icon still appears with whatever flags ERR ships.
        extern bool patchOverworldBossIcons;
        extern bool patchDungeonBossIcons;
        extern bool patchCampIcons;
        extern bool patchMerchantIcons;
        // Cosmetic options layered on top of the patch flags above. Each
        // requires its corresponding `patch*` flag to be true to take
        // effect (we only touch the icon when we're already rewriting
        // the row).
        extern bool hideDungeonIconsOnClear;    // dungeon entrances: hide on boss kill

        // In-game config overlay (Dear ImGui on a DX12 hook). Opens with the
        // toggle key. Set false if a DX-hook conflict (Steam overlay/RTSS/etc.)
        // or a GPU driver issue makes the game unstable.
        extern std::string overlayUiLanguage; // OVERLAY menu language (ini overlay_ui_language);
                                              // the in-game menu follows the GAME language
    // key native_menu_icons: how the in-game menu draws category icons - 0 off,
    // 1 masked strip, 2 one child per icon (both need the movie rebuilt at load),
    // 3 drawn straight from our own pixels into a clip the row already has.
    extern float fontScale;        // overlay text size multiplier (live io.FontGlobalScale)
        extern bool menuEnabled; // ini menu_enabled (was enable_menu, was enable_overlay)
        extern std::string menuRenderMode; // native | imgui | dev (ini menu_render_mode)

        // WHICH menu the hotkey opens. Parsed from overlay_render_mode in ONE place - ask through
        // these rather than comparing the string, so a legacy value cannot come to mean two
        // different things in two files.
        enum class MenuMode
        {
            Native, // the game's own screen; the overlay is never created
            ImGui,  // the overlay window; nothing of the in-game menu is injected
            Dev,    // both: hotkey opens the overlay, F8 the in-game menu
        };
        MenuMode menu_mode();
        bool native_menu_enabled();  // Native or Dev
        bool overlay_menu_enabled(); // ImGui or Dev
        extern float overlayOpacity;   // overlay menu panel opacity 0.3..1.0 (window bg alpha)
        // Overlay menu window geometry, persisted across sessions. overlayWinX = window
        // CENTER x as a fraction of screen width (0.5 = centered); overlayWinY = window
        // TOP y as a fraction of screen height (0 = flush top). Fractions survive a
        // resolution/aspect change; overlayWinW/H are pixels. Auto-saved on close.
        extern float overlayWinX, overlayWinY, overlayWinW, overlayWinH;

        // Marker dump (hotkey → dump beacon/stamp coords to file)
        extern bool enableMarkerDump;
        extern uint32_t markerDumpKey;  // Win32 VK_* code (default VK_F9 = 0x78)
        extern bool enableManualHide;
        extern uint32_t hideMarkerKey;  // Win32 VK_* code (default VK_DELETE = 0x2E)
        extern uint16_t hideMarkerGamepad;  // XINPUT_GAMEPAD_* mask (default RB = 0x0200)
        extern bool enableHoverInfo;

        // ERSC-hosting workaround: hotkey toggles WorldMapPointParam +
        // PlaceName FMG between vanilla and expanded states. Press before
        // hosting a co-op session, press again after.
        extern bool enableToggleHotkey;
        extern uint32_t toggleInjectionKey;  // default VK_F10 = 0x79
        // XInput button bitmask combo. All buttons in the mask must be
        // held simultaneously to fire. Default Y + R3 (right stick click).
        extern uint16_t toggleGamepadMask;
    };

    uint32_t parse_vk_code(std::string name);
    uint16_t parse_gamepad_combo(std::string s);
    // Inverse of the above - produce a string that parse_*() round-trips, used by
    // save_config to persist hotkeys rebound in the overlay.
    std::string format_vk_code(uint32_t vk);
    std::string format_gamepad_combo(uint16_t mask);
};
