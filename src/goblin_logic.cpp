#include "goblin_logic.hpp"
#include "goblin_config.hpp"
#include "goblin_inject.hpp" // injected_row_baked_group2: our rows' group 2 as baked
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "goblin/goblin_structs.hpp"
#include "goblin/goblin_map_flags.hpp"
#include "goblin/goblin_map_tiles.hpp"
#include "goblin/goblin_map_exceptions.hpp"

#include <spdlog/spdlog.h>
#include <mutex>
#include <unordered_map>

using namespace goblin;
using namespace goblin::mapPoint;

// Goblin icon ID ranges (same as Goblin-ERR)
static constexpr ParamRange goblinIcons(1, 78500);
static constexpr ParamRange goblinIconsERR(1000000, 10025000);

// ---- The patch_* options: every field they write, as it was -------------------------------------
// patch_camp_icons / patch_merchant_icons / patch_overworld_boss_icons / patch_dungeon_boss_icons
// (and hide_dungeon_icons_on_clear) rewrite the game's own camp, merchant and boss rows so they
// follow require_map_fragments. apply_map_logic runs again on every live settings apply, so each
// row's fields are kept here as they were before the FIRST write, keyed by row id (the game's own
// rows: their ids are stable, never our reassigned ones), and every pass rebuilds the row FROM them:
//   - a row whose option is on gets the patch computed from its originals. Reading the LIVE row
//     instead stashed an already-patched value on the 2nd pass (SetupDungeonERR zeroes eventFlagId,
//     so textEnableFlagId3 became 0 = always enabled -> the icon showed with no map fragment);
//   - a row whose option is off now gets its originals back, exactly. This used to be missing: an
//     option switched off left the last patched values in place (ERR's Stone Platform boss row
//     30190000 kept eventFlagId 118 and textEnableFlagId2 1039560053 after
//     patch_overworld_boss_icons went off), as did hide_dungeon_icons_on_clear going off.
struct StockFields
{
    unsigned eventFlagId;
    unsigned enable[3];   // textEnableFlagId1..3
    unsigned disable[5];  // textDisableFlagId1..5
    unsigned clearFlag;   // textEnableFlagId5, only READ: the dungeon boss's clear flag, which
                          // hide_dungeon_icons_on_clear copies into the disable flags
};
static std::unordered_map<int, StockFields> g_stock_orig;

static void ReadStock(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, StockFields &f)
{
    f.eventFlagId = row.eventFlagId;
    f.enable[0] = row.textEnableFlagId1;
    f.enable[1] = row.textEnableFlagId2;
    f.enable[2] = row.textEnableFlagId3;
    f.disable[0] = row.textDisableFlagId1;
    f.disable[1] = row.textDisableFlagId2;
    f.disable[2] = row.textDisableFlagId3;
    f.disable[3] = row.textDisableFlagId4;
    f.disable[4] = row.textDisableFlagId5;
    f.clearFlag = row.textEnableFlagId5;
}

// Each field written once, with its final value: the map may be reading this row right now.
static void WriteStock(from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, const StockFields &f)
{
    row.eventFlagId = f.eventFlagId;
    row.textEnableFlagId1 = f.enable[0];
    row.textEnableFlagId2 = f.enable[1];
    row.textEnableFlagId3 = f.enable[2];
    row.textDisableFlagId1 = f.disable[0];
    row.textDisableFlagId2 = f.disable[1];
    row.textDisableFlagId3 = f.disable[2];
    row.textDisableFlagId4 = f.disable[3];
    row.textDisableFlagId5 = f.disable[4];
}

// The row's originals; nullptr = never patched (it still carries them) and capture is false.
static const StockFields *StockOrig(int rowId, const from::paramdef::WORLD_MAP_POINT_PARAM_ST &row,
                                    bool capture)
{
    auto it = g_stock_orig.find(rowId);
    if (it != g_stock_orig.end())
        return &it->second;
    if (!capture)
        return nullptr;
    StockFields f{};
    ReadStock(row, f);
    return &g_stock_orig.emplace(rowId, f).first->second;
}

static bool HasException(int paramId, int &mapFragment)
{
    if (ExceptionList.count(paramId))
    {
        mapFragment = ExceptionList.at(paramId);
        return true;
    }
    return false;
}

static int GetMapFlagFromTile(MapTile location)
{
    for (const auto &fragment : MapList)
    {
        for (auto &chunk : fragment.mapFragmentTile)
        {
            if (chunk == location)
                return fragment.mapFragmentId;
        }
    }
    return 0;
}

// ---- The post-event gate (require_map_fragments) ------------------------------------------------
// Areas that only exist after a story event are gated on that event's flag through group 2
// (textEnableFlag2Id1..8), next to the map-fragment gate in eventFlagId. The gate has to be
// REVERSIBLE: apply_map_logic runs again on every live settings apply, and with the option off
// every row it touched must carry exactly what it had before. It used to write all eight slots
// and never put them back, so switching require_map_fragments on and off left the story flag
// behind (Leyndell's Ashen Capital: 31 markers gone until a restart) and wiped any gate that was
// already there - the switched-chest gate our bake writes into these same fields, and the
// overhauls' own group-2 gates on their rows.
//
// Group 2 is read PER TEXT SLOT by the engine: line N shows only if textEnableFlag2IdN holds, and
// the icon goes only when every line is gone. Our own renderer (goblin_inject.cpp,
// row_group2_gate_off) hides the marker when ANY slot's flag is unset. So the story flag goes only
// into the slots the original row left at 0 (free) and never over a flag that is there:
//   - our markers: both gates hold together, because our renderer ANDs every slot;
//   - rows the ENGINE draws: a line that already carries its own gate keeps it and is NOT
//     story-gated. Seen once across the nine profiles: one Reborn row on the Stone Platform (m19,
//     id 764) gated on its own flag in slots 1-4. Its own gate wins there; overwriting it would
//     show that row whenever the Erdtree has burned, whatever its own flag says.
//
// Leyndell is not here any more. Which capital stands is world state (flag 300, which ERR can set
// and clear at will), not a one-way story flag, and it is independent of map fragments: it is
// a per-marker rule applied live by the visibility test (MapEntry::state_flag).

// The post-event flag that gates a tile, or 0.
static int StoryFlagForTile(const MapTile &chunk)
{
    if (chunk == MapTile(19))
        return flag::StoryErdtreeOnFire;  // Stone Platform
    if (chunk == MapTile(21) || chunk == MapTile(21, 1) || chunk == MapTile(21, 2) ||
        chunk == MapTile(22))
        return flag::StoryCharmBroken;    // Shadow Keep, Specimen Storehouse, Stone Coffin Fissure
    if (chunk == MapTile(20, 1))
        return flag::StorySealingTreeBurnt;  // Enir-Ilim
    return 0;
}

// The original group 2 of the game's own rows (camps, bosses, merchants, and any stock row whose
// id falls in the goblin ranges), captured ONCE, before the first write - the same way
// g_stock_orig keeps the patch_* fields. Keyed by row id: these are the game's own ids, and our injected
// rows are never looked up here (their originals are the bake's; see OrigGroup2).
struct Group2
{
    int slot[8];
};
static std::unordered_map<int, Group2> g_orig_group2;

static void ReadGroup2(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, Group2 &g)
{
    const int v[8] = {row.textEnableFlag2Id1, row.textEnableFlag2Id2, row.textEnableFlag2Id3,
                      row.textEnableFlag2Id4, row.textEnableFlag2Id5, row.textEnableFlag2Id6,
                      row.textEnableFlag2Id7, row.textEnableFlag2Id8};
    for (int k = 0; k < 8; ++k) g.slot[k] = v[k];
}

// What this row's group 2 is when no post-event gate is on it. false = the row has never been
// touched and is not ours: it still carries its original, and there is nothing to capture unless
// the gate is about to write it (capture = true).
static bool OrigGroup2(int rowId, const from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, bool capture,
                       Group2 &out)
{
    if (goblin::injected_row_baked_group2(rowId, out.slot))
        return true;  // one of ours: from the bake, never from the live row
    auto it = g_orig_group2.find(rowId);
    if (it != g_orig_group2.end())
    {
        out = it->second;
        return true;
    }
    if (!capture)
        return false;
    ReadGroup2(row, out);
    g_orig_group2.emplace(rowId, out);
    return true;
}

// Put the post-event gate on (storyFlag != 0) or take it off (0). Idempotent: always derived from
// the original, never from what the live row carries now.
static void ApplyStoryGate(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, int storyFlag)
{
    Group2 orig{};
    if (!OrigGroup2(rowId, row, storyFlag != 0, orig))
        return;
    int *slot[8] = {&row.textEnableFlag2Id1, &row.textEnableFlag2Id2, &row.textEnableFlag2Id3,
                    &row.textEnableFlag2Id4, &row.textEnableFlag2Id5, &row.textEnableFlag2Id6,
                    &row.textEnableFlag2Id7, &row.textEnableFlag2Id8};
    for (int k = 0; k < 8; ++k)
        *slot[k] = (storyFlag != 0 && orig.slot[k] == 0) ? storyFlag : orig.slot[k];
}

static int GetMapFragment(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    int requiredMapFragment = 0;
    auto chunk = MapTile(row.areaNo, row.gridXNo, row.gridZNo);

    if (!HasException(rowId, requiredMapFragment))
    {
        requiredMapFragment = GetMapFlagFromTile(chunk);
    }

    // Lake of Rot shares fine tile m12_01 with Ainsel River but is revealed by its
    // OWN map fragment (LakeOfRot 62061, not Ainsel 62060). The two are one tile, so
    // the tile->fragment table put the whole tile under Ainsel - which leaked Lake of
    // Rot icons onto the map the moment the Ainsel fragment was owned (e.g. opening
    // the Ainsel River map). Distinguish by the marker's runtime-resolved location
    // text (LOCATION_ALT runs in inject_map_entries, before this): PlaceName 12011 =
    // Lake of Rot. (12011 is a raw PlaceName id, stored un-remapped on the row.)
    if (chunk == MapTile(12, 1))
    {
        const int loc[8] = {row.textId1, row.textId2, row.textId3, row.textId4,
                            row.textId5, row.textId6, row.textId7, row.textId8};
        for (int t : loc)
            if (t == 12011) { requiredMapFragment = flag::LakeOfRot; break; }
    }

    return requiredMapFragment;
}

static int GetIconFlag(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    if (config::requireMapFragments)
        return GetMapFragment(rowId, row);
    else
        return flag::AlwaysOn;
}

// Each Setup* fills `t` - which starts as the row's originals - and never reads the live row's
// patched fields, so a pass gives the same answer however many passes came before it.

// DUNGEON rows only. The one caller is SetupDungeonERR, which is itself only reached from the
// `else` of `if (row.textId2 == 5100)` and never writes textId2 before calling - so the overworld
// branch this used to carry (disable flags taken from textEnableFlagId4 when textId2 == 5100) could
// not be reached, and is gone. Overworld bosses go through SetupOverworldERR, which does not hide.
static void HideOnCompletion(const StockFields &orig, StockFields &t)
{
    for (unsigned &d : t.disable)
        d = orig.clearFlag;
}

static void SetupOverworldERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row,
                              const StockFields &orig, StockFields &t)
{
    t.enable[1] = orig.eventFlagId;
    t.eventFlagId = static_cast<unsigned>(GetIconFlag(rowId, row));
}

static void SetupDungeonERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row,
                            const StockFields &orig, StockFields &t)
{
    const unsigned mapFragment = static_cast<unsigned>(GetIconFlag(rowId, row));
    t.enable[0] = mapFragment;
    t.enable[1] = mapFragment;
    t.enable[2] = orig.eventFlagId;
    t.eventFlagId = 0;

    // Off: the disable flags stay the originals `t` started from.
    if (config::hideDungeonIconsOnClear)
    {
        HideOnCompletion(orig, t);
    }
}

static void SetupCampsERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row,
                          const StockFields &orig, StockFields &t)
{
    t.enable[1] = orig.eventFlagId;
    t.eventFlagId = static_cast<unsigned>(GetIconFlag(rowId, row));
}

static void SetupMerchants(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row,
                           const StockFields &, StockFields &t)
{
    // One assignment, not two branches: GetIconFlag already returns flag::AlwaysOn when
    // config::requireMapFragments is false, so the else arm computed the same value by hand.
    t.enable[2] = static_cast<unsigned>(GetIconFlag(rowId, row));
}

using StockSetup = void (*)(int, from::paramdef::WORLD_MAP_POINT_PARAM_ST &, const StockFields &,
                            StockFields &);

// One pass over one of the game's camp/merchant/boss rows. setup = the patch to apply this pass,
// or nullptr when its option is off: then a row patched on an earlier pass gets its originals
// back, and a row never patched is left alone.
static void ApplyStockPatch(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, StockSetup setup)
{
    const StockFields *orig = StockOrig(rowId, row, setup != nullptr);
    if (!orig)
        return;
    StockFields t = *orig;
    if (setup)
        setup(rowId, row, *orig, t);
    WriteStock(row, t);
}

// ONE pass at a time. apply_map_logic is reached from the overlay thread, the map/UI thread (the
// native menu), the hotkey thread and the watcher, and the originals above and in g_orig_group2 are
// filled lazily on the first pass that patches a row - two passes at once would race on them.
// Locked and unlocked by hand around a __try/__finally rather than with a lock_guard: this build
// uses /EHsc, where a fault under an outer __except (the init step's guard) need not run a
// destructor, and a mutex left locked would stop every later pass for the rest of the session.
static std::mutex g_map_logic_mtx;
static void apply_map_logic_locked();

void goblin::apply_map_logic()
{
    g_map_logic_mtx.lock();
    __try
    {
        apply_map_logic_locked();
    }
    __finally
    {
        g_map_logic_mtx.unlock();
    }
}

static void apply_map_logic_locked()
{

    int modified_goblin = 0;
    int modified_boss = 0;
    int modified_camp = 0;
    int modified_merchant = 0;

    int story_gated = 0;

    for (auto [rowId, row] :
         from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>(L"WorldMapPointParam"))
    {
        // true = this row takes the require_map_fragments gates this pass (every branch that asks
        // GetIconFlag). The post-event gate below follows it, and comes OFF a row no branch took
        // this time - so switching the option, or a patch_* toggle, off puts group 2 back.
        bool gated = false;
        // Goblin icons (our injected entries + any existing ones)
        if (goblinIcons.IsInRange(rowId) || goblinIconsERR.IsInRange(rowId))
        {
            row.eventFlagId = GetIconFlag(rowId, row);
            gated = true;
            modified_goblin++;
        }
        else
        {
            // The game's own camp / merchant / boss rows, by their text ids (never written by us,
            // so a row's class is the same on every pass). setup stays nullptr when the row's
            // option is off, which is what restores a row an earlier pass patched.
            StockSetup setup = nullptr;
            int *counter = nullptr;
            // Camp markers (textId2=5000) - ERR-placed, opt-in patching
            if (row.textId2 == 5000)
            {
                if (config::patchCampIcons) { setup = &SetupCampsERR; counter = &modified_camp; }
            }
            // Merchant markers (textId4=8800) - ERR-placed, opt-in patching
            else if (row.textId4 == 8800)
            {
                if (config::patchMerchantIcons) { setup = &SetupMerchants; counter = &modified_merchant; }
            }
            // Boss markers - overworld (textId2=5100) or dungeon (textId3=5100/5300)
            else if (row.textId2 == 5100)
            {
                if (config::patchOverworldBossIcons) { setup = &SetupOverworldERR; counter = &modified_boss; }
            }
            else if (row.textId3 == 5100 || row.textId3 == 5300)
            {
                if (config::patchDungeonBossIcons) { setup = &SetupDungeonERR; counter = &modified_boss; }
            }
            ApplyStockPatch(rowId, row, setup);
            if (setup)
            {
                gated = true;
                ++*counter;
            }
        }

        const int story = (gated && config::requireMapFragments)
                              ? StoryFlagForTile(MapTile(row.areaNo, row.gridXNo, row.gridZNo))
                              : 0;
        ApplyStoryGate(rowId, row, story);
        if (story) ++story_gated;
    }

    spdlog::debug("Map logic applied: {} goblin icons, {} bosses, {} camps, {} merchants, {} "
                  "post-event gated",
                  modified_goblin, modified_boss, modified_camp, modified_merchant, story_gated);
}
