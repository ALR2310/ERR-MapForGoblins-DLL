#pragma once

#include <cstdint>
#include <unordered_map>

namespace goblin::collected
{
    /// Build tile-to-row lookup tables from map data. No memory reads yet.
    void initialize();

    /// Remap row IDs after dynamic ID assignment in inject.
    /// old_to_new maps original MASSEDIT row_id -> dynamically assigned row_id.
    void remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new);

    /// Re-read GEOF/WGM from memory. Returns delta (newly hidden count).
    int refresh();

    bool is_row_collected(uint64_t row_id);

    /// Same as is_row_collected, but takes the ORIGINAL MAP_ENTRIES row_id and
    /// resolves the post-remap dynamic id internally. Use this from code paths
    /// that still see original IDs (e.g. the marker dump reads MAP_ENTRIES directly).
    bool is_original_row_collected(uint64_t original_row_id);

    void register_param_ptr(uint64_t row_id, void *param_data);

    int collected_count();
    int skipped_count();

    /// Live player world position (block-local): CSWorldGeomMan + 0x70/0x74/0x78 =
    /// X/Z/Y. Returns false if the manager isn't resolved/available. Used by the
    /// map hover-info overlay for the "height vs player" readout.
    bool read_player_pos(float &x, float &z, float &y);

    /// The map the PLAYER is standing in, packed the way the game packs a map id:
    /// m{AA}_{BB}_{CC}_{DD} -> 0xAABBCCDD (area, gridX, gridZ, index). This is the
    /// block the block-local coordinates above belong to, which is why it sits right
    /// behind them in the same ChrIns field group. false = no live player.
    /// Not to be confused with goblin::maphover::map_layer(), which is the map TAB
    /// being looked at; this is where the character actually is.
    bool read_player_map_id(uint32_t &map_id);
};
