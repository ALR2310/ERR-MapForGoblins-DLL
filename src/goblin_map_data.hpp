#pragma once

// Hand-maintained, NOT generated: the Category enum and the MapEntry layout that every profile's
// packed src/generated*/goblin_map_blob_data.cpp expands into. tools/generate_data.py packs
// against this header (its CATEGORY_MAP names these members, and tools/mapblob.py reads the
// Category enum straight out of this file so the two cannot drift). It used to live in src/generated/ and left
// git with that directory in 560bc65; it belongs with the sources.

#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

enum class Category : uint8_t
{
    EquipArmaments,
    EquipArmour,
    EquipAshesOfWar,
    EquipSpirits,
    EquipTalismans,
    KeyCelestialDew,
    KeyCookbooks,
    KeyCrystalTears,
    KeyImbuedSwordKeys,
    KeyLarvalTears,
    KeyScadutreeFragments,
    KeyGreatRunes,
    KeyLostAshes,
    KeyPotsNPerfumes,
    KeySeedsTears,
    KeyWhetblades,
    LootAmmo,
    LootBellBearings,
    LootConsumables,
    LootCraftingMaterials,
    LootMPFingers,
    LootMaterialNodes,
    LootMerchantBellBearings,
    LootReusables,
    LootSmithingStones,
    LootSmithingStonesLow,
    LootSmithingStonesRare,
    LootGoldenRunes,
    LootGoldenRunesLow,
    LootStoneswordKeys,
    LootThrowables,
    LootPrattlingPates,
    LootRuneArcs,
    LootDragonHearts,
    LootGloveworts,
    LootGreatGloveworts,
    LootGestures,
    LootGreases,
    LootUtilities,
    LootStatBoosts,
    ReforgedFortunes,
    WorldHostileNPC,
    MagicIncantations,
    MagicMemoryStones,
    MagicPrayerbooks,
    MagicSorceries,
    WorldBosses,
    QuestDeathroot,
    QuestProgression,
    QuestSeedbedCurses,
    ReforgedEmberPieces,
    ReforgedItemsAndChanges,
    ReforgedRunePieces,
    WorldGraces,
    WorldImpStatues,
    WorldMaps,
    WorldPaintings,
    WorldSpiritSprings,
    WorldSpiritspringHawks,
    WorldStakesOfMarika,
    WorldSummoningPools,
    WorldKindlingSpirits,
    WorldInteractables,
    // APPEND ONLY from here: the numeric value is persisted (hidden-marker file, per-slot focus),
    // so a member inserted above would shift every saved category after it. The last member is
    // goblin::progress::kCategoryCount's anchor.
    KeyReveredSpiritAshes,
    WorldStrongEnemies,
    KeySpectralSteedRegalia,
};

// A marker's world-state rule: the polarity of MapEntry::state_flag it needs to be shown. The values
// are baked by tools/mapblob.py (STATE_SHOW), so they are part of the packed format.
enum class StateShow : uint8_t
{
    Always = 0,   // no rule
    WhileOn = 1,  // shown only while state_flag is ON
    WhileOff = 2, // shown only while state_flag is OFF
};

struct MapEntry
{
    uint64_t row_id;
    from::paramdef::WORLD_MAP_POINT_PARAM_ST data;
    Category category;
    int16_t geom_slot;    // GEOF slot = InstanceID - 9000; -1 if N/A
    int16_t name_suffix;  // e.g. 9003 from "AEG099_821_9003"; -1 if N/A
    const char *object_name;  // full MSB object name e.g. "AEG099_821_9003"; nullptr if N/A
    // Source item-lot linkage for live-loot mode (read getItemFlagId/item from
    // memory at runtime → randomizer-compatible). 0/0 = not lot-backed.
    uint32_t lotId;       // ItemLotParam row id (0 = none)
    uint8_t  lotType;     // 0 = none, 1 = ItemLotParam_map (pref), 2 = ItemLotParam_enemy
    // 1 = this marker stands for SEVERAL of that lot's items (a boss's reward, split into
    // one marker per category), so the lot is not its address: slot 1 belongs to whichever
    // category came first. Live labels and live hide-flags skip it and the baked label and
    // the boss's own kill flag stand; the spoiler-free icon and label still apply.
    uint8_t  lotAggregate;
    // Where collected-geometry tracking has to LOOK, which is not always where the marker is
    // drawn: a relocated piece displays at its pickup spot while its geometry - the live
    // CSWorldGeomMan instance tracking has to find - stayed at the MSB position. Equal to
    // data.posX/posZ for every row that was not moved that way.
    // The DISPLAY position is data.posX/posZ itself. There used to be a third pair here,
    // because a generation-time pass spiralled stacked icons apart and made data.pos untrue;
    // that pass is gone (the live de-overlap in goblin_inject.cpp re-spreads from the row's own
    // position on every refresh and overwrote the baked spread anyway).
    float real_posX;
    float real_posZ;
    // World state: a marker that exists in only ONE state of the world is shown only while that
    // state holds, read live from this event flag (Leyndell: flag 300 ON = the Ashen Capital, m11_05;
    // OFF = the Royal Capital, m11_00). Baked from inputs/world_state_rules.json. Read by the
    // visibility test alone (goblin_inject.cpp), never by anything that decides "collected" - a
    // marker whose state is away still counts on the Progress page, as not collected.
    uint32_t state_flag;   // 0 = no rule
    StateShow state_show;
};

// Filled by goblin::generated::load_map_data() (src/goblin_map_blob.cpp) at startup, from
// the packed table in src/generated*/goblin_map_blob_data.cpp. A POINTER, not an array:
// every use is MAP_ENTRIES[i], which reads the same, and nothing takes sizeof it.
extern const MapEntry *MAP_ENTRIES;
extern size_t MAP_ENTRY_COUNT;

} // namespace goblin::generated
