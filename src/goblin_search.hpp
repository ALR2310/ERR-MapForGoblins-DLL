#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Item search over the injected markers: substring match against the marker's name in the
// player's language AND its English name at once (both keyed by the same textId, so a
// Russian player can type either "кузнечный" or "smithing"), plus the category label.
// Results are grouped by progress region - "Smithing Stone" alone matches hundreds of
// markers, and an ungrouped list of those is useless. The player picks results and shows
// ONLY those on the map (goblin::set_focus_rows), with the highlight ring on each.
//
// The index is built lazily on the first query (the FMG strings only exist after
// setup_messages) and rebuilt when goblin::label_epoch() moves (live-loot relabels,
// anonymous loot toggled) or the overlay language changes. It reads the rows' CURRENT
// label, so in spoiler-free mode every lot marker is just "something" and the search
// cannot see past that.
namespace goblin::search
{
    struct Hit
    {
        uint64_t key;      // original_row_id (the focus / pick key)
        int32_t textId;    // live label textId (lookup_text_any resolves it)
        int32_t region;    // progress region PlaceName id (-1 = Other)
        uint8_t cat;       // generated::Category
        std::string name;  // display: localized name, plus " (English)" when that differs
    };

    struct Group
    {
        int32_t region;
        std::string region_name;
        std::vector<Hit> hits;  // sorted by name
    };

    struct Results
    {
        size_t total = 0;          // hits across all groups
        bool truncated = false;    // more matched than kMaxHits; the tail was dropped
        std::vector<Group> groups; // sorted by region name, "Other" last
    };

    // Matches kept per query. Past this the list stops being something a player scrolls, and
    // a "type more letters" hint replaces the tail.
    constexpr size_t kMaxHits = 2000;

    // Run the query (UTF-8, whitespace-separated tokens; every token must occur in one of the
    // names). Cheap when neither the query nor the index changed. The result is an immutable
    // snapshot: the overlay (its own thread) and the native menu (the game's UI thread) can
    // each hold one while the other runs a different query. Never null. An empty query yields
    // empty results (never the whole map).
    std::shared_ptr<const Results> query(std::string_view utf8);

    // The pick set (the markers the player ticked), keyed like Hit::key. Kept across queries
    // so a player can gather picks from several searches. Every change is applied to the map
    // AT ONCE (the pick focus, goblin::set_focus_rows): a non-empty set isolates exactly its
    // markers, emptying it lifts that focus (a category focus set elsewhere is left alone).
    bool is_picked(uint64_t key);
    void set_picked(uint64_t key, bool on);
    void pick_many(const std::vector<uint64_t> &keys, bool on); // one map re-apply for the batch
    void clear_picks();
    size_t pick_count();
    std::vector<uint64_t> picks();

    // Number of indexed markers (0 until the first query built the index). Diagnostics.
    size_t index_size();
}
