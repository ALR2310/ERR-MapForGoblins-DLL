#pragma once

#include <cstddef>
#include <cstdint>

#include "goblin_map_data.hpp"  // Category enum

namespace goblin::generated
{
    // Live-loot icon/category lookup. Maps an offset-encoded item key (the same
    // encoding used for marker textIds: goods+500M, weapon+100M / ammo as-is,
    // protector+200M, accessory+300M, gem+400M) to the iconId and MFG Category
    // that item would get as a normal marker. Built offline by
    // generate_item_icons_cpp from the LOOT_CATEGORIES classifier. The DLL reads
    // the live ItemLotParam item, looks it up here, and re-icons + re-gates the
    // marker so randomized loot shows the right icon under its own toggle.
    struct ItemIcon
    {
        int32_t  key;       // offset-encoded item id
        uint16_t iconId;
        Category category;
    };

    extern const size_t ITEM_ICON_COUNT;
    extern const ItemIcon ITEM_ICONS[];  // sorted ascending by key (binary search)

    // Spoiler-free "?" map-icon frame id for this profile (anonymous_loot mode).
    // 440 on a vanilla-base gfx, shifted by the icon-frame offset on bases that
    // add their own frames (Convergence). Generated per profile.
    extern const uint16_t ANON_ICON_ID;

    // Green "cleared" check badge source iconId: the native-marker twin child
    // shown over defeated boss/NPC/hawk markers (no engine Cleared subclip on
    // the native path). Generated per profile like ANON_ICON_ID.
    extern const uint16_t CLEARED_ICON_ID;

    // Focus-ring source iconId: the EXTRA marker row drawn over the isolated category on the
    // progress screen. The markers' own icons are never swapped. Generated per profile.
    extern const uint16_t HIGHLIGHT_ICON_ID;
}
