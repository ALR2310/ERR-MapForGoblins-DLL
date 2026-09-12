#pragma once

// Hand-maintained, NOT generated: the Category enum and the MapEntry layout that every profile's
// baked src/generated*/goblin_map_data.cpp fills. tools/generate_data.py writes the .cpp against
// this header (CATEGORY_MAP there names these members). It used to live in src/generated/ and left
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
    // REAL (pre-de-overlap) MSB-local X/Z. data.posX/posZ may be spiral-shifted by
    // the de-overlap pass so stacked icons read separately; collected-geometry
    // tracking must match the LIVE CSWorldGeomMan instance at its true coordinates,
    // so it uses these instead of the shifted display position. Equal to
    // data.posX/posZ for any row the de-overlap didn't move.
    float real_posX;
    float real_posZ;
    // Pre-de-overlap DISPLAY position: where this marker wants to sit on the map before the
    // offline pass spiralled stacked icons apart. Separate from real_pos because the two
    // diverge: a relocated piece displays at its pickup spot but its geometry - what
    // collected tracking has to find - stayed at the MSB position, and Roundtable Hold rows
    // carry an interior shift that display needs and tracking must not have. The live
    // de-overlap and the native marker path start from THIS pair.
    float display_posX;
    float display_posZ;
};

extern const MapEntry MAP_ENTRIES[];
extern const size_t MAP_ENTRY_COUNT;

} // namespace goblin::generated
