#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "goblin_map_data.hpp"

namespace goblin::collected
{
    /// Build tile-to-row lookup tables from map data. No memory reads yet.
    void initialize();

    /// Remap row IDs after dynamic ID assignment in inject.
    /// old_to_new maps original MASSEDIT row_id -> dynamically assigned row_id.
    void remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new);

    /// Re-read GEOF/WGM from memory. Returns delta (newly hidden count).
    int refresh();

    /// Drop everything refresh() has accumulated for the character that was loaded until now.
    /// Called on a save-slot switch: the sticky carry-forward in refresh() keeps a row collected
    /// until its live object is seen alive, and that memory belongs to ONE character. Left in
    /// place, whatever A picked up stayed hidden for B until B stood on the tile.
    void forget_session_state();

    bool is_row_collected(uint64_t row_id);

    /// Same as is_row_collected, but takes the ORIGINAL MAP_ENTRIES row_id and
    /// resolves the post-remap dynamic id internally. Use this from code paths
    /// that still see original IDs (e.g. the marker dump reads MAP_ENTRIES directly).
    bool is_original_row_collected(uint64_t original_row_id);

    void register_param_ptr(uint64_t row_id, void *param_data);

    /// The areaNo a registered piece row had BEFORE hiding wrote 99 over it (live row id).
    /// False when the row is not a registered piece row. The marker dump needs it to project a
    /// hidden row back onto the map instead of losing it under "(area 99, gx N)".
    bool original_area_no(uint64_t row_id, uint8_t &area_out);

    int collected_count();
    int skipped_count();

    /// Our MAP_ENTRY behind a LIVE (post-remap) row id; nullptr when the id is not one of ours.
    const generated::MapEntry *entry_for_live_row(uint64_t live_row_id);

    /// Field diagnostic for the marker dump. For each LIVE row id that is ours and bound to an
    /// MSB object: the object, its tile and real coords, every WGM instance of its model on that
    /// tile (distance, alive flags, engine slot), the GEOF records for that model on the tile, and
    /// the outcome the tracker currently holds. WGM and GEOF are read once for the whole batch.
    std::string diagnose_rows(const std::vector<uint64_t> &live_row_ids);

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
