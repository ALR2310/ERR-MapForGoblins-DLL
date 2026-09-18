#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "goblin_map_data.hpp"
#include "goblin_geof_models.hpp"
#include "modutils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using Category = goblin::generated::Category;

// Model IDs are: 10,000,000 + AEG_group*1000 + model_number
// e.g. AEG099_821 = 10,099,821 = 0x009A1C6D
static constexpr uint16_t GEOM_IDX_MIN = 0x1194;

// ─── data-driven model tracking ─────────────────────────────────────
// Built from MAP_ENTRIES at init time - no hardcoded model lists.

static std::set<std::string> g_tracked_prefixes;   // "AEG099_821", "AEG099_651", etc.
static std::set<uint32_t>    g_tracked_model_ids;   // 10099821, 10099651, etc.

static std::string prefix_from_object_name(const char *name)
{
    // "AEG099_821_9000" → "AEG099_821"
    const char *last_us = strrchr(name, '_');
    if (!last_us || last_us == name) return {};
    return std::string(name, last_us - name);
}

static std::string prefix_from_model_id(uint32_t model_id)
{
    if (model_id < 10000000) return {};
    uint32_t raw = model_id - 10000000;
    uint32_t group = raw / 1000;
    uint32_t number = raw % 1000;
    char buf[20];
    snprintf(buf, sizeof(buf), "AEG%03u_%03u", group, number);
    return buf;
}

static uint32_t model_id_from_prefix(const std::string &prefix)
{
    // "AEG099_821" → 10099821
    if (prefix.size() < 10 || prefix.substr(0, 3) != "AEG" || prefix[6] != '_')
        return 0;
    unsigned group = 0, number = 0;
    if (sscanf(prefix.c_str(), "AEG%u_%u", &group, &number) != 2) return 0;
    return 10000000u + group * 1000u + number;
}

static std::set<uint64_t> g_collected_rows;
static int g_collected_count = 0;
static int g_unmatched_count = 0;
// Guards g_collected_rows against the refresh thread's reassignment while other
// threads query it (e.g. the overlay's Progress tab iterating MAP_ENTRIES on the
// render thread). refresh() builds a local set and only takes the lock for the
// swap; the query helpers take it for the lookup. Leaf-only, no reentrancy.
static std::mutex g_collected_mutex;

struct ParamRef {
    uint8_t *ptr;
    uint8_t original_areaNo;
};
static std::map<uint64_t, ParamRef> g_param_ptrs;

static std::map<uint32_t, std::vector<uint64_t>> g_tile_to_rows;                // tile → ordered row_ids
// A g_tile_name_to_row (tile -> object_name -> row_ids) was built, cleared and remapped here and
// never read once. It served the name-keyed fast path, which the rewrite to slot-and-position
// classification replaced - the comment above it still described that path as current.
// 3D slot map: tile → prefix → geom_slot → row_ids (duplicate-named parts share the
// suffix-derived slot: the game writes one GEOF entry PER instance with the SAME slot
// value - verified live: two collected AEG099_931_9006 produced GEOF slots [6, 6]).
static std::map<uint32_t, std::map<std::string, std::map<int, std::vector<uint64_t>>>> g_tile_slot_to_row;
// MSB-local (posX, posY, posZ) per tracked row - used to detect ERR-style
// "replacement" where a different AEG099_* spawns at the same coords.
static std::map<uint64_t, std::tuple<float, float, float>> g_entry_positions;
static bool g_initialized = false;


struct GEOFEntry
{
    uint32_t tile_id;
    uint8_t flags;
    uint16_t geom_idx;
    uint32_t model_hash;  // bytes 4-7 of GEOF entry, identifies model type
};

// Each geom_idx holds two slots: flags=0x00 → even, flags=0x80 → odd
static int aeg099_index_from_geof(uint16_t geom_idx, uint8_t flags)
{
    return (geom_idx - GEOM_IDX_MIN) * 2 + ((flags & 0x80) ? 1 : 0);
}

// ─── GEOF from memory (GeomFlagSaveDataManager) ──────────────────────

// Singleton .data slots resolved by AOB (patch-resilient) instead of hardcoded
// RVAs: the game's static slots move on every update, so we pin them by a unique
// surrounding-code signature. relative_offsets {{3,7}} extracts the slot address
// from the `mov reg,[rip+slot]` xref; the AOB wildcards the rip-disp and branch
// targets so it survives patches. Resolved once, cached. (GeomNonActiveBlock-
// Manager is deliberately NOT read - see read_geof_from_memory and
// docs/geom_nonactive_block_manager.md.)
static uintptr_t resolve_slot(const char *aob)
{
    return reinterpret_cast<uintptr_t>(modutils::scan<void>({
        .aob = aob, .relative_offsets = {{3, 7}}}));
}
static uintptr_t geom_flag_slot()  // GeomFlagSaveDataManager (was RVA 0x3D69D18)
{
    static uintptr_t s = resolve_slot(
        "48 8B 3D ?? ?? ?? ?? 33 F6 48 85 FF 74 ?? 48 8B CF E8 ?? ?? ?? ?? 4C 8B 07");
    return s;
}
static uintptr_t world_geom_man_slot()  // CSWorldGeomMan (was RVA 0x3D69BA8)
{
    static uintptr_t s = resolve_slot(
        "48 8B 0D ?? ?? ?? ?? 48 8D 53 10 E8 ?? ?? ?? ?? 4C 8B E8");
    return s;
}
// WorldChrMan .data slot (was RVA 0x3D65F88). Pinned by the "load slot, null-check,
// deref +0x1E508 (LocalPlayer)" idiom - +0x1E508 is the distinctive LocalPlayer
// field offset, so the signature stays anchored even as the slot RVA moves.
static uintptr_t world_chr_man_slot()
{
    static uintptr_t s = resolve_slot(
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 98 08 E5 01 00");
    return s;
}

static bool safe_read(void *addr, void *out, size_t count)
{
    __try
    {
        memcpy(out, addr, count);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Player world position from the local ChrIns physics block. Chain:
//   WorldChrMan (AOB slot) -> LocalPlayer (+0x1E508)
//   -> physics vector at +0x6C0 = (X, Y, Z) as floats.
// X/Z are block-local (small, per-tile origin) so they are NOT directly
// comparable to a marker's world posX/posZ; Y is the true world height
// (the vertical axis is not tiled), which is all the hover-height readout
// needs. The slot RVA moves each patch, so it is AOB-resolved; the two
// struct offsets (+0x1E508, +0x6C0) are stable layout fields.
bool goblin::collected::read_player_pos(float &x, float &z, float &y)
{
    uintptr_t slot = world_chr_man_slot();
    if (!slot) return false;
    uintptr_t wcm = 0;
    if (!safe_read((void *)slot, &wcm, 8) || !wcm) return false;
    uintptr_t lp = 0;
    if (!safe_read((void *)(wcm + 0x1E508), &lp, 8) || !lp) return false;
    float v[3] = {0, 0, 0};
    if (!safe_read((void *)(lp + 0x6C0), v, sizeof v)) return false;  // +0x6C0 X, +0x6C4 Y, +0x6C8 Z
    x = v[0]; y = v[1]; z = v[2];
    return true;
}

// Player map id. The ChrIns field group read above is not three loose floats but a
// five-field block - X, Y, Z, radius, mapId - and the map id is the piece that makes
// the block-local X/Z mean anything: they are local to THAT block. So it sits one
// field past the radius, at +0x6D0. A SECOND identical block follows at +0x6D4..+0x6E4
// (the chunk/tile one); its id is logged beside the first under debug_logging so the
// two can be told apart from real play instead of assumed.
// (Layout cross-checked against a maintained CE table, whose build has the whole group
// 0x10 lower - +0x6B0 there for the coordinates we read at +0x6C0.)
bool goblin::collected::read_player_map_id(uint32_t &map_id)
{
    uintptr_t slot = world_chr_man_slot();
    if (!slot) return false;
    uintptr_t wcm = 0;
    if (!safe_read((void *)slot, &wcm, 8) || !wcm) return false;
    uintptr_t lp = 0;
    if (!safe_read((void *)(wcm + 0x1E508), &lp, 8) || !lp) return false;
    uint32_t ids[2] = {0, 0};
    if (!safe_read((void *)(lp + 0x6D0), &ids[0], 4)) return false;
    safe_read((void *)(lp + 0x6E4), &ids[1], 4);
    if (goblin::config::debugLogging)
    {
        static uint32_t s_last[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        if (ids[0] != s_last[0] || ids[1] != s_last[1])
        {
            s_last[0] = ids[0];
            s_last[1] = ids[1];
            spdlog::info("[playermap] +0x6D0 = m{:02d}_{:02d}_{:02d}_{:02d} (0x{:08X}) | "
                         "+0x6E4 = m{:02d}_{:02d}_{:02d}_{:02d} (0x{:08X})",
                         (ids[0] >> 24) & 0xFF, (ids[0] >> 16) & 0xFF,
                         (ids[0] >> 8) & 0xFF, ids[0] & 0xFF, ids[0],
                         (ids[1] >> 24) & 0xFF, (ids[1] >> 16) & 0xFF,
                         (ids[1] >> 8) & 0xFF, ids[1] & 0xFF, ids[1]);
        }
    }
    // 0xFFFFFFFF is the game's "no map" value; an area byte outside the shipped
    // range means we are not looking at a map id at all - report nothing rather
    // than emphasise a random group of markers.
    const uint32_t area = (ids[0] >> 24) & 0xFF;
    if (ids[0] == 0xFFFFFFFFu || area == 0 || area > 61) return false;
    map_id = ids[0];
    return true;
}

// SEH-guarded single byte write. Returns true on success, false if the
// write access-violated (stale param pointer after another mod or the
// game relocated our buffer). Used per-pointer in refresh() so we can
// evict just the bad entries instead of tripping the top-level SEH and
// skipping the whole refresh cycle.
static bool safe_write_byte(uint8_t *addr, uint8_t val)
{
    __try
    {
        *addr = val;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void read_singleton_entries(uintptr_t slot,
                                    std::vector<GEOFEntry> &out)
{
    void *gf_ptr = nullptr;
    if (!slot || !safe_read((void *)slot, &gf_ptr, 8) || !gf_ptr)
        return;

    // The manager keeps a SORTED DENSE vector of (tile_id, blob) pairs: base = gf_ptr + 8, stride 0x10,
    // and the live element count is a qword at gf_ptr + 0x189D0. The engine walks exactly that many.
    // Scanning to a fixed 0x20000 instead read stale slots past the end (the vector removes elements in
    // place, so those slots keep previously-removed records -> phantom "collected" geometry) and also
    // read past the object entirely, which only looked harmless because safe_read swallows the faults.
    // Audited 2026-07-28; if the count looks insane we fall back to the old bounded scan.
    uint64_t live_count = 0;
    if (!safe_read((char *)gf_ptr + 0x189D0, &live_count, 8) || live_count == 0 || live_count > 0x2000)
        live_count = 0;
    const int scan_limit = live_count ? (int)(0x08 + live_count * 16) : 0x20000;
    if (goblin::config::debugLogging)
    {
        static std::atomic<int> once{0};
        if (once.fetch_add(1, std::memory_order_relaxed) < 2)
            spdlog::info("[verify] GEOF table: live count from +0x189D0 = {} -> scanning to 0x{:X} "
                         "(the old code always scanned to 0x20000)", live_count, scan_limit);
    }

    int tiles_found = 0, tiles_skipped = 0, consecutive_empty = 0;
    for (int off = 0x08; off < scan_limit; off += 16)
    {
        uint64_t id_val = 0, ptr_val = 0;
        if (!safe_read((char *)gf_ptr + off, &id_val, 8))
            break;
        if (!safe_read((char *)gf_ptr + off + 8, &ptr_val, 8))
            break;

        if (id_val == 0 && ptr_val == 0)
        {
            if (++consecutive_empty > 256)
                break;
            continue;
        }
        consecutive_empty = 0;

        // Normalise to (area,gx,gz): the raw key's low byte is the 4th map component
        // (DD - e.g. the _10 in m60_44_37_10), which encode_tile (area<<24|gx<<16|gz<<8)
        // does not carry. A geom recorded under a DD!=0 block would otherwise never match
        // its marker keyed at DD=0. Masking here keeps GEOF tile keys aligned with markers.
        uint32_t tile_id = (uint32_t)id_val & 0xFFFFFF00u;
        uint8_t area = (tile_id >> 24) & 0xFF;
        if (area < 0x0A || area > 0x3D)
        {
            tiles_skipped++;
            continue;
        }
        if (ptr_val < 0x10000 || ptr_val > 0x7FFFFFFFFFFF)
        {
            tiles_skipped++;
            continue;
        }
        tiles_found++;

        // Layout A: count @+8, entries @+16 | Layout B: count @+0, entries @+8
        uint8_t header[16] = {};
        if (!safe_read((void *)ptr_val, header, 16))
            continue;

        uint32_t count = 0;
        uintptr_t entries_start = 0;

        uint32_t countA = 0;
        memcpy(&countA, header + 8, 4);
        uint32_t countB = 0;
        memcpy(&countB, header + 0, 4);

        if (countA > 0 && countA < 100000)
        {
            count = countA;
            entries_start = ptr_val + 16;
        }
        else if (countB > 0 && countB < 100000)
        {
            count = countB;
            entries_start = ptr_val + 8;
        }
        else
            continue;

        for (uint32_t ei = 0; ei < count; ei++)
        {
            uint8_t entry[8] = {};
            if (!safe_read((void *)(entries_start + ei * 8), entry, 8))
                break;

            // The record is `key | value`, where the key is ((model_id << 0x11) | geom_idx) << 0xf and
            // BIT 0 IS THE STORED BOOLEAN. The engine's setter does not drop a record when the value
            // goes false, it clears bit 0 in place, and the tile-load applier only treats an instance as
            // collected when bit 0 is set. Reading mere presence as "collected" therefore hid markers for
            // geometry the player had NOT collected (anything collected once and later reset).
            // Audited 2026-07-28. Bits 1..14 are always 0, which is why the entry[1] filter below has
            // been working as a de-facto key check.
            if ((entry[0] & 1) == 0)
            {
                // VERIFY (2026-07-28 audit): every one of these is a record the OLD code counted as
                // collected while the engine considers it NOT collected.
                static std::atomic<int> rejected{0};
                const int n = rejected.fetch_add(1, std::memory_order_relaxed) + 1;
                if (goblin::config::debugLogging && (n <= 5 || n % 100 == 0))
                    spdlog::info("[verify] GEOF record with value bit CLEAR ignored (#{}): "
                                 "model={} geom={}", n,
                                 (uint32_t)(entry[4] | (entry[5] << 8) | (entry[6] << 16) |
                                            (entry[7] << 24)),
                                 (uint16_t)(entry[2] | (entry[3] << 8)));
                continue;
            }

            uint8_t entry_flags = entry[1];
            uint16_t geom_idx = entry[2] | (entry[3] << 8);
            uint32_t model_hash = entry[4] | (entry[5] << 8) | (entry[6] << 16) | (entry[7] << 24);

            if (g_tracked_model_ids.count(model_hash) && (entry_flags == 0x00 || entry_flags == 0x80))
                out.push_back({tile_id, entry_flags, geom_idx, model_hash});
        }
    }

    // tiles_found / tiles_skipped were counted per table row and then dropped on the floor. They
    // are the answer to "did the walk actually see the table, or did it reject everything", which
    // is the first question when collected-tracking goes quiet.
    //
    // Edge-triggered ON PURPOSE. This runs on every refresh - 10 Hz while a map is open - and the
    // interesting information is entirely in the TRANSITIONS: the walk starting to see the table,
    // or going quiet. Printing the same pair on every pass buries the rest of the log (a 53 s
    // Graceborne session on 2026-07-31 was 650 identical lines out of 716) without adding a fact.
    static std::atomic<uint64_t> last_reported{UINT64_MAX};
    const uint64_t now = (static_cast<uint64_t>(static_cast<uint32_t>(tiles_found)) << 32) |
                         static_cast<uint32_t>(tiles_skipped);
    if (goblin::config::debugLogging &&
        last_reported.exchange(now, std::memory_order_relaxed) != now)
        spdlog::debug("[GEOF] table walk: {} tile(s) read, {} rejected by the area/pointer sanity "
                      "tests", tiles_found, tiles_skipped);
}

// ─── Read geom state from CSWorldGeomMan (loaded tiles) ─────────────
//
// Returns per-tile the live instances keyed by (model prefix -> geom slot), which is what
// refresh() consumes. The name-set and the two position lists this used to also return
// (alive_names / occupied / alive_occupied) were removed 2026-07-30: the classifier stopped
// reading them when it moved to the position match, but they were still being filled for
// EVERY instance on EVERY refresh - a std::string copy plus a tuple each.
//
// Name-based detection alone was never enough anyway: the game spawns gathering-node
// CSWorldGeomIns lazily, so a truly-alive instance may simply not be loaded yet (and would
// be wrongly treated as dead by "not in alive list" logic).
struct WGMSnapshot
{

    // COORDINATE-FREE identity (engine's own key). Per ACTUAL-model prefix -> geom slot
    // -> live instances. The engine keys collected state on (model_id, geom_idx) only
    // (FUN_1406b2b80: key = ((geom_idx | model_id<<0x11) << 0xf)); slot = runtime
    // geom_idx - 0x2328 == GEOF slot == part suffix-9000, so this matches
    // g_tile_slot_to_row directly - immune to any display-coord shift. px/pz kept
    // only to disambiguate twins (same slot). suffix_slot = slot from the MSB name, for
    // a live cross-check of the geom_idx->slot formula.
    struct SlotInst { bool alive; float px, pz; int suffix_slot;
                      uint8_t f263, f26B; uint32_t model_id, gidx; };  // raw flags + engine id (diag)
    std::map<std::string, std::map<int, std::vector<SlotInst>>> slot_insts;
};

static std::map<uint32_t, WGMSnapshot> read_wgm_snapshot()
{
    std::map<uint32_t, WGMSnapshot> result;

    void *wgm = nullptr;
    if (!safe_read((void *)world_geom_man_slot(), &wgm, 8) || !wgm)
        return result;

    // Tree at WGM+0x18: +0x08 head_ptr, +0x10 size
    void *tree_head = nullptr;
    uint64_t tree_size = 0;
    if (!safe_read((char *)wgm + 0x18 + 0x08, &tree_head, 8) || !tree_head)
        return result;
    safe_read((char *)wgm + 0x18 + 0x10, &tree_size, 8);

    if (tree_size == 0 || tree_size > 1000)
        return result;

    // RB tree node: +0x00 left, +0x08 parent, +0x10 right, +0x19 is_nil, +0x20 value
    // Value (BlocksEntry): +0x00 block_id(u32), +0x08 data_ptr
    void *root = nullptr;
    safe_read((char *)tree_head + 0x08, &root, 8); // parent of head = root

    auto get_is_nil = [](void *node) -> bool {
        uint8_t val = 1;
        safe_read((char *)node + 0x19, &val, 1);
        return val != 0;
    };

    auto get_left = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x00, &p, 8);
        return p;
    };

    auto get_right = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x10, &p, 8);
        return p;
    };

    auto get_parent = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x08, &p, 8);
        return p;
    };

    auto min_node = [&](void *node) -> void * {
        while (node && !get_is_nil(node)) {
            void *left = get_left(node);
            if (!left || get_is_nil(left)) break;
            node = left;
        }
        return node;
    };

    void *current = min_node(root);
    int nodes_visited = 0;

    while (current && current != tree_head && !get_is_nil(current) && nodes_visited < 500)
    {
        nodes_visited++;

        uint32_t block_id = 0;
        void *block_data = nullptr;
        safe_read((char *)current + 0x20, &block_id, 4);
        safe_read((char *)current + 0x28, &block_data, 8);

        if (block_data)
        {
            // geom_ins vector at BlockData+0x288
            void *vec_begin = nullptr, *vec_end = nullptr;
            safe_read((char *)block_data + 0x288 + 0x08, &vec_begin, 8);
            safe_read((char *)block_data + 0x288 + 0x10, &vec_end, 8);

            if (vec_begin && vec_end && vec_end > vec_begin)
            {
                size_t count = ((uintptr_t)vec_end - (uintptr_t)vec_begin) / 8;
                if (count > 10000) count = 10000;

                for (size_t i = 0; i < count; i++)
                {
                    void *geom_ins = nullptr;
                    safe_read((char *)vec_begin + i * 8, &geom_ins, 8);
                    if (!geom_ins) continue;

                    void *msb_part_ptr = nullptr;
                    safe_read((char *)geom_ins + 0x18 + 0x18 + 0x18, &msb_part_ptr, 8);
                    if (!msb_part_ptr) continue;

                    void *name_ptr = nullptr;
                    safe_read(msb_part_ptr, &name_ptr, 8);
                    if (!name_ptr) continue;

                    wchar_t name_buf[64] = {};
                    safe_read(name_ptr, name_buf, sizeof(name_buf) - 2);

                    char narrow[64] = {};
                    for (int c = 0; c < 63 && name_buf[c]; c++)
                        narrow[c] = (char)(name_buf[c] & 0xFF);

                    // Keep only tracked AEG families. ERR uses both AEG099_*
                    // (base-game gathering assets) and AEG463_* (DLC flowers/
                    // bodies). Everything else (AEG001 decorations, colliders,
                    // signs, …) is noise - drop it early.
                    std::string narrow_str(narrow);
                    bool is_tracked_family =
                        narrow_str.compare(0, 7, "AEG099_") == 0 ||
                        narrow_str.compare(0, 7, "AEG463_") == 0;
                    if (!is_tracked_family)
                        continue;

                    // Runtime position lives in MsbPart +0x20 (3 floats). Only X and Z are taken:
                    // they disambiguate twins at the same slot. Y was read into a `py` that nothing
                    // used - the Y that reaches g_entry_positions comes from e.data.posY.
                    float px = 0, pz = 0;
                    safe_read((char *)msb_part_ptr + 0x20 + 0, &px, 4);
                    safe_read((char *)msb_part_ptr + 0x20 + 8, &pz, 4);

                    // Normalise the live block key to (area,gx,gz) - drop the 4th map
                    // component (DD) in the low byte (e.g. m60_44_37_10) so geoms loaded
                    // from a DD!=0 block match their marker keyed via encode_tile (DD=0).
                    // This is why some tiles' collected nodes never hid (NO-ROW-MATCH).
                    auto &snap = result[block_id & 0xFFFFFF00u];

                    // Track alive state only for models we're actually hiding on the map
                    std::string prefix = prefix_from_object_name(narrow);
                    if (!prefix.empty() && g_tracked_prefixes.count(prefix))
                    {
                        // +0x263 bit1: persistent alive flag (survives restart)
                        // +0x26B bit4: immediate collection flag (lost on tile reload)
                        uint8_t f263 = 0, f26B = 0;
                        safe_read((char *)geom_ins + 0x263, &f263, 1);
                        safe_read((char *)geom_ins + 0x26B, &f26B, 1);

                        bool alive = (f263 & 0x02) && !(f26B & 0x10);
                        // Coordinate-free record, keyed by ACTUAL model + geom slot
                        // (the engine's identity). model_id @inst+0x28; geom_idx @
                        // [[inst+0x48]+8]; slot = geom_idx - 0x2328. Keyed by the
                        // model's prefix (not the part-name prefix) so ERR model
                        // substitution (part AEG099_753 -> model AEG463_860) lines up
                        // with g_tile_slot_to_row's geof_prefix.
                        uint32_t model_id = 0;
                        safe_read((char *)geom_ins + 0x28, &model_id, 4);
                        void *gobj = nullptr;
                        safe_read((char *)geom_ins + 0x48, &gobj, 8);
                        uint32_t gidx = 0;
                        if (gobj) safe_read((char *)gobj + 8, &gidx, 4);
                        std::string mprefix = prefix_from_model_id(model_id);
                        if (!mprefix.empty() && g_tracked_prefixes.count(mprefix))
                        {
                            int slot = (int)gidx - 0x2328;
                            // suffix_slot from the MSB name (e.g. _9006 -> 6) for the
                            // formula cross-check logged in refresh().
                            int suffix_slot = -1;
                            const char *us = strrchr(narrow, '_');
                            if (us && us[1]) suffix_slot = atoi(us + 1) - 9000;
                            snap.slot_insts[mprefix][slot].push_back(
                                {alive, px, pz, suffix_slot, f263, f26B, model_id, gidx});
                        }
                    }
                }
            }
        }

        // In-order successor
        void *right = get_right(current);
        if (right && !get_is_nil(right))
        {
            current = min_node(right);
        }
        else
        {
            void *parent = get_parent(current);
            int walk_up = 0;
            while (parent && parent != tree_head && ++walk_up < 500)
            {
                void *parent_right = get_right(parent);
                if (current != parent_right) break;
                current = parent;
                parent = get_parent(current);
            }
            if (walk_up >= 500) break;
            current = parent;
        }
    }

    return result;
}

static std::vector<GEOFEntry> read_geof_from_memory()
{
    std::vector<GEOFEntry> result;

    read_singleton_entries(geom_flag_slot(), result);

    // NOTE: GeomNonActiveBlockManager (RVA_GEOM_NONACTIVE) is intentionally NOT
    // scanned. Despite the name, its layout is nothing like GeomFlagSaveData-
    // Manager - it is a 0x820-byte object holding a fixed inline array of 0x20-
    // byte block records (count at +0x818, active flag at +0x08), not a
    // (tile_id, ptr) table at +0x08. read_singleton_entries assumes the GeomFlag
    // layout, so applied here it walked ~126 KB past the object end into
    // unrelated heap, raising a stream of (safe_read-caught) access violations
    // and never returning valid data on any game version. Collected-geometry
    // state comes from GeomFlagSaveDataManager (above) + CSWorldGeomMan (loaded
    // tiles) + the immediate per-AEG +0x26B flag. Full RE:
    // docs/geom_nonactive_block_manager.md.

    if (!result.empty())
        spdlog::debug("[GEOF] Memory: {} flag-save entries", result.size());

    return result;
}

// ─── tile ID helper ──────────────────────────────────────────────────

static uint32_t encode_tile(uint8_t area, uint8_t gridX, uint8_t gridZ)
{
    return ((uint32_t)area << 24) | ((uint32_t)gridX << 16) | ((uint32_t)gridZ << 8);
}

// ─── main initialization ─────────────────────────────────────────────

void goblin::collected::initialize()
{
    g_collected_rows.clear();
    g_collected_count = 0;
    g_unmatched_count = 0;

    g_tile_to_rows.clear();
    g_tile_slot_to_row.clear();
    g_tracked_prefixes.clear();
    g_tracked_model_ids.clear();
    g_entry_positions.clear();

    // Build tracking tables from MAP_ENTRIES - any entry with object_name is tracked.
    // Adding a new model type only requires adding entries + _slots.json.
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        if (!e.object_name || !e.object_name[0])
            continue;

        std::string prefix = prefix_from_object_name(e.object_name);
        if (prefix.empty())
            continue;

        g_tracked_prefixes.insert(prefix);

        // GEOF buckets must use the part's ACTUAL model, which ERR sometimes substitutes
        // (part AEG099_753_9000 instantiating DLC model AEG463_860): the game writes GEOF
        // entries under the actual model's hash, so matching by the NAME prefix misses
        // them and collected flowers stay visible on unloaded tiles after a restart.
        std::string geof_prefix = prefix;
        {
            auto *end = generated::GEOF_MODEL_OVERRIDES + generated::GEOF_MODEL_OVERRIDE_COUNT;
            auto *ov = std::lower_bound(
                generated::GEOF_MODEL_OVERRIDES, end, e.row_id,
                [](const generated::GeofModelOverride &o, uint64_t id) { return o.row_id < id; });
            if (ov != end && ov->row_id == e.row_id)
            {
                geof_prefix = prefix_from_model_id(ov->model_id);
                g_tracked_prefixes.insert(geof_prefix);  // feeds g_tracked_model_ids (GEOF filter)
            }
        }

        uint32_t tile = encode_tile(e.data.areaNo, e.data.gridXNo, e.data.gridZNo);
        g_tile_to_rows[tile].push_back(e.row_id);

        // 3D slot map: tile → ACTUAL-model prefix → geom_slot → row_ids
        if (e.geom_slot >= 0)
            g_tile_slot_to_row[tile][geof_prefix][e.geom_slot].push_back(e.row_id);

        // MSB-local position for replacement detection via WGM occupancy. Use the
        // REAL geometry X/Z, which is not always where the marker is drawn: a relocated piece
        // displays at its pickup spot while its CSWorldGeomIns stayed at the MSB position.
        // Matching on the drawn position missed the live instance and broke immediate WGM hiding.
        g_entry_positions[e.row_id] = {e.real_posX, e.data.posY, e.real_posZ};
    }

    // Build model_id set for GEOF filtering
    for (auto &prefix : g_tracked_prefixes)
    {
        uint32_t mid = model_id_from_prefix(prefix);
        if (mid)
            g_tracked_model_ids.insert(mid);
    }

    int total_piece_entries = 0;
    for (auto &[t, rows] : g_tile_to_rows)
        total_piece_entries += (int)rows.size();

    spdlog::info("[COLLECTED] {} entries across {} tiles, {} tracked prefixes ({} model IDs)",
                 total_piece_entries, g_tile_to_rows.size(),
                 g_tracked_prefixes.size(), g_tracked_model_ids.size());


    g_initialized = true;
    spdlog::info("[COLLECTED] Initialized, awaiting refresh()");
}

// ─── remap row IDs after dynamic assignment ────────────────────────

static std::unordered_map<uint64_t, uint64_t> g_original_to_dynamic;
static std::unordered_map<uint64_t, uint64_t> g_dynamic_to_original;  // the dump reads LIVE ids

void goblin::collected::remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new)
{
    g_original_to_dynamic = old_to_new;
    g_dynamic_to_original.clear();
    for (const auto &[orig, dyn] : old_to_new)
        g_dynamic_to_original[dyn] = orig;

    // Remap g_tile_to_rows
    for (auto &[tile, rows] : g_tile_to_rows)
    {
        for (auto &rid : rows)
        {
            auto it = old_to_new.find(rid);
            if (it != old_to_new.end())
                rid = it->second;
        }
    }

    // Remap g_tile_slot_to_row (3D: tile → prefix → slot → row_ids)
    for (auto &[tile, prefix_map] : g_tile_slot_to_row)
    {
        for (auto &[prefix, slot_map] : prefix_map)
        {
            for (auto &[slot, rids] : slot_map)
            {
                for (auto &rid : rids)
                {
                    auto it = old_to_new.find(rid);
                    if (it != old_to_new.end())
                        rid = it->second;
                }
            }
        }
    }

    // Remap g_entry_positions (keyed by row_id)
    decltype(g_entry_positions) remapped;
    for (auto &[rid, pos] : g_entry_positions)
    {
        auto it = old_to_new.find(rid);
        uint64_t new_rid = (it != old_to_new.end()) ? it->second : rid;
        remapped[new_rid] = pos;
    }
    g_entry_positions = std::move(remapped);

    spdlog::debug("[COLLECTED] Remapped {} row IDs", old_to_new.size());
}

// ─── hide icon in-place ─────────────────────────────────────────────

// A hide_icon(void*) that wrote areaNo = 99 straight through the pointer lived here with no
// callers. The live path does the same write through safe_write_byte() in refresh(), which is
// SEH-guarded - the param buffer can be relocated or freed by another mod between our snapshot
// and the write, and an unguarded twin is exactly the thing that turns that into a crash.

// ─── register param pointer for real-time hiding ────────────────────

void goblin::collected::register_param_ptr(uint64_t row_id, void *param_data)
{
    auto *p = reinterpret_cast<uint8_t *>(param_data);
    g_param_ptrs[row_id] = {p, p[0x20]};  // save original areaNo
}

bool goblin::collected::original_area_no(uint64_t row_id, uint8_t &area_out)
{
    auto it = g_param_ptrs.find(row_id);
    if (it == g_param_ptrs.end())
        return false;
    area_out = it->second.original_areaNo;
    return true;
}

// ─── refresh from memory (real-time update) ─────────────────────────

int goblin::collected::refresh()
{
    void *wgm_check = nullptr;
    safe_read((void *)world_geom_man_slot(), &wgm_check, 8);
    void *geof_check = nullptr;
    safe_read((void *)geom_flag_slot(), &geof_check, 8);
    if (!wgm_check && !geof_check)
        return 0;

    if (!g_initialized || g_tile_to_rows.empty())
        return 0;

    auto geof = read_geof_from_memory();

    std::set<uint64_t> new_collected;

    // Group GEOF entries by tile + prefix
    // key: tile_id → prefix → list of GEOF-computed slots
    std::map<uint32_t, std::map<std::string, std::vector<int>>> geof_tile_prefix_slots;
    for (auto &e : geof)
    {
        if (g_tile_to_rows.find(e.tile_id) == g_tile_to_rows.end())
            continue;
        std::string prefix = prefix_from_model_id(e.model_hash);
        if (prefix.empty())
            continue;
        int slot = aeg099_index_from_geof(e.geom_idx, e.flags);
        geof_tile_prefix_slots[e.tile_id][prefix].push_back(slot);
    }

    // ── WGM: tracking for loaded tiles ──
    // HISTORY, not the current rule: rows used to be classified by an (A)/(B) pair of
    // name-and-position tests over alive_names/occupied - "the name is not alive AND another
    // AEG099_* instance sits at the same MSB-local position" or "the name is present but
    // dead". Both fields and both tests are gone; what ships is the position match inside
    // (tile, model) described where the loop actually starts, below. The one part of that
    // reasoning still worth carrying: absence of a name never means collected on its own,
    // because the game spawns alive gathering-node CSWorldGeomIns lazily, so a name we do
    // not see may simply not have spawned near the player yet.
    auto wgm = read_wgm_snapshot();
    std::set<uint32_t> wgm_tiles;

    // Rows we positively observed alive in WGM this refresh. Used to override
    // sticky carry-forward below: a row is "uncollected" only when we see its
    // live instance, not just because the tile is unloaded.
    std::set<uint64_t> demonstrably_alive_rows;

    // WGM classification. The slot survives only as the shape of the map we walk - the match
    // itself is by position, for the reason spelled out at the inner loop below. (An earlier
    // rewrite here WAS coordinate-free and keyed on the engine's (model_id, geom_idx) identity;
    // it left collected nodes as NO-ROW-MATCH and was replaced. Do not restore that claim
    // without also changing the loop.)
    for (auto &[tile_id, snap] : wgm)
    {
        wgm_tiles.insert(tile_id);

        auto tile_it = g_tile_slot_to_row.find(tile_id);
        if (tile_it == g_tile_slot_to_row.end())
            continue;

        // Position-based match within (tile, model), NOT slot-keyed. The engine's live
        // geom_idx (slot = geom_idx - 0x2328) does NOT always equal the MSB name-suffix
        // our geom_slot was baked from, so slot-keyed matching left collected nodes as
        // NO-ROW-MATCH and they never hid (the vanilla "gathered lily stays on the map"
        // report). Instead flatten every row and every live instance of this model in the
        // tile, and match each row to its nearest instance by REAL geometry coords.
        // g_entry_positions holds real_posX/posZ, so a marker displayed away from its geometry -
        // which broke the ORIGINAL position heuristic and prompted the slot rewrite - is moot.
        for (auto &[prefix, slot_map] : tile_it->second)
        {
            auto pref_it = snap.slot_insts.find(prefix);
            if (pref_it == snap.slot_insts.end())
                continue;  // no live instance of this model on the tile → GEOF/sticky handle

            std::vector<const WGMSnapshot::SlotInst *> insts;
            for (auto &[slot, sv] : pref_it->second)
                for (auto &in : sv) insts.push_back(&in);
            if (insts.empty())
                continue;

            for (auto &[slot, row_ids] : slot_map)
                for (uint64_t row_id : row_ids)
                {
                    auto pt = g_entry_positions.find(row_id);
                    if (pt == g_entry_positions.end()) continue;
                    auto [ex, ey, ez] = pt->second;
                    // Two candidates per row: the nearest instance of the model, and the
                    // nearest instance whose MSB name carries THIS row's slot.
                    const WGMSnapshot::SlotInst *best = nullptr;
                    float best_d2 = 1e18f;
                    const WGMSnapshot::SlotInst *named = nullptr;
                    float named_d2 = 1e18f;
                    for (auto *in : insts)
                    {
                        float dx = in->px - ex, dz = in->pz - ez;
                        float d2 = dx * dx + dz * dz;
                        if (d2 < best_d2) { best_d2 = d2; best = in; }
                        if (in->suffix_slot == slot && d2 < named_d2) { named_d2 = d2; named = in; }
                    }
                    // Same physical node: baked real coords match the live MsbPart
                    // position to well under 1u. Reject a far "nearest" (the row's own
                    // instance hasn't spawned - don't cross-match to another node's).
                    //
                    // Within that radius the same-named instance wins over the merely
                    // nearest one. Twins - several rows of ONE model at ONE MSB position
                    // (ERR m30_13 AEG099_821_9000/_9001; 13 such groups, 27 rows, in the err
                    // bake) - otherwise all resolve to whichever instance the map yields
                    // first, and picking up one of them hid every twin's marker
                    // (2026-09-14: row 805, the _9001 relocated onto its pickup target,
                    // vanished the moment _9000 was collected).
                    if (named && named_d2 <= 16.0f)
                    {
                        best = named;
                        best_d2 = named_d2;
                    }
                    else if (!best || best_d2 > 16.0f)
                    {
                        // Nothing at the baked position. The asset may have moved (a game/mod
                        // update relocating it, or a marker placed on a pickup target away
                        // from the asset itself). Fall back to the MSB identity: the live
                        // part name's suffix - parsed from the SAME MSB name our geom_slot was
                        // baked from, NOT the engine's geom_idx (the formula that failed for
                        // lilies). Nearest same-named instance wins; duplicate-named twins
                        // share one collected key in the engine anyway.
                        best = named;
                        best_d2 = named_d2;
                        if (!best) continue;
                    }
                    if (best->alive) demonstrably_alive_rows.insert(row_id);
                    else             new_collected.insert(row_id);
                }
        }
    }

    // ── GEOF: for unloaded tiles ──
    for (auto &[tid, prefix_slots] : geof_tile_prefix_slots)
    {
        if (wgm_tiles.count(tid))
            continue;

        auto tile_it = g_tile_slot_to_row.find(tid);
        if (tile_it == g_tile_slot_to_row.end()) continue;

        for (auto &[prefix, slots] : prefix_slots)
        {
            auto prefix_it = tile_it->second.find(prefix);
            if (prefix_it == tile_it->second.end()) continue;

            // Single-instance fallback. If this tile has exactly one row for
            // this prefix, any GEOF entry means that row was collected - no
            // need to match a slot. aeg099_index_from_geof() is calibrated
            // for AEG099_*; AEG463_* uses a different encoding (verified via
            // memory dump: m60_48_36 AEG463_840 has geom_slot=5 in MAP_ENTRY
            // but GEOF reports geom_idx=4503 → AEG099 formula computes
            // slot=6, off by one). For single-instance prefixes the slot is
            // irrelevant, so skip the lookup entirely.
            if (prefix_it->second.size() == 1 && prefix_it->second.begin()->second.size() == 1)
            {
                new_collected.insert(prefix_it->second.begin()->second.front());
                continue;
            }

            // Per-slot match. Duplicate-named parts (ERR copy-pastes that keep the
            // part Name) collapse to the SAME (model_id, geom_idx) key in the engine -
            // their GEOF records are byte-identical and the slot is shared. The save/
            // load path (eldenring.exe 0x6b2b80, RE'd 2026-06) keys collected state
            // ONLY on (model_id, geom_idx): on tile reload the engine marks EVERY
            // instance with a matching key collected - all-or-nothing per slot, twins
            // are fused and one becomes un-collectable as a separate object. So any
            // GEOF entry for a slot ⇒ hide ALL rows mapped to that slot (matches the
            // engine; the loaded-tile WGM path above still distinguishes twins live).
            for (int slot : slots)
            {
                auto row_it = prefix_it->second.find(slot);
                if (row_it == prefix_it->second.end()) continue;
                for (uint64_t r : row_it->second)
                    new_collected.insert(r);
            }
        }
    }

    // ── Sticky carry-forward ──
    //
    // For models that don't leave a replacement asset and aren't tracked by
    // GeomFlagSaveDataManager (notably AEG463_840 "Dragon's Calorbloom" with
    // isBreakOnPickUp=True), both WGM-replacement and GEOF detection lose
    // track after the player walks far from the tile - WGM goes empty and
    // GEOF has no entry. Without carry-forward the row drops back to
    // "uncollected" and the icon reappears on the map.
    //
    // Rule: if a row was collected last refresh and we don't positively see
    // its live instance now, it stays collected. Respawn (e.g. after grace
    // rest, when the tile reloads with the flower alive) puts the row into
    // demonstrably_alive_rows and breaks the sticky retention.
    int carried = 0;
    for (uint64_t prev : g_collected_rows)
    {
        if (demonstrably_alive_rows.count(prev))
            continue;  // respawn observed → release
        if (new_collected.insert(prev).second)
            carried++;
    }
    if (carried > 0)
        spdlog::debug("[COLLECTED] Sticky carry-forward kept {} row(s) hidden", carried);

    // ── DIAGNOSTIC (debug_logging only): log WGM instance alive-flag TRANSITIONS
    //    + the slot->row match outcome, so a live pickup can be traced end to end:
    //      * does the flag we read (+0x263 bit1 / +0x26B bit4) actually flip?
    //      * does the flipped slot map to a row (g_tile_slot_to_row), or NO-MATCH?
    //      * did that row land in new_collected (→ will be hidden) or stay SHOWN?
    //    On-change only (no first-seen spam). Runs BEFORE the unchanged-early-return
    //    below so it still fires when a flip failed to produce a row-level change
    //    (the exact blind spot: flag flipped but match missed → nothing hidden).
    if (goblin::config::debugLogging)
    {
        // Bounded on purpose. This grew for the life of the process - one std::string key per WGM
        // instance per scan - so a long debug session leaked steadily while only producing a log line.
        // ~1500 tracked instances is the real ceiling; past 4096 we stop remembering rather than grow.
        static std::map<std::string, int> dbg_prev_alive;
        constexpr size_t kDbgAliveCap = 4096;
        for (auto &[tile_id, snap] : wgm)
        {
            const int a = (tile_id >> 24) & 0xFF, gx = (tile_id >> 16) & 0xFF, gz = (tile_id >> 8) & 0xFF;
            for (auto &[prefix, slot_map] : snap.slot_insts)
                for (auto &[slot, insts] : slot_map)
                    for (auto &in : insts)
                    {
                        std::string key = std::to_string(tile_id) + ":" + prefix + ":" +
                                          std::to_string(slot) + ":" + std::to_string((int)in.px) +
                                          ":" + std::to_string((int)in.pz);
                        const int cur = in.alive ? 1 : 0;
                        auto it = dbg_prev_alive.find(key);
                        if (it != dbg_prev_alive.end() && it->second != cur)
                        {
                            std::string rows;
                            auto ti = g_tile_slot_to_row.find(tile_id);
                            if (ti != g_tile_slot_to_row.end())
                            {
                                auto pi = ti->second.find(prefix);
                                if (pi != ti->second.end())
                                {
                                    auto ri = pi->second.find(slot);
                                    if (ri != pi->second.end())
                                        for (uint64_t r : ri->second)
                                            rows += " row=" + std::to_string(r) +
                                                    (new_collected.count(r) ? "(hidden)" : "(SHOWN)");
                                }
                            }
                            if (rows.empty()) rows = " NO-ROW-MATCH(slot absent in g_tile_slot_to_row)";
                            // in.suffix_slot is the slot parsed from the MSB name; `slot` is the
                            // one derived from the engine's geom_idx. Printing BOTH is the live
                            // cross-check of that formula the struct comment promises - it was
                            // computed every refresh and left out of this line.
                            spdlog::info("[GEOFDBG] m{:02d}_{:02d}_{:02d} {} slot={} name-slot={} "
                                         "model={} gidx={} f263=0x{:02X} f26B=0x{:02X} alive {}->{}{}",
                                         a, gx, gz, prefix, slot, in.suffix_slot, in.model_id,
                                         in.gidx, (unsigned)in.f263, (unsigned)in.f26B,
                                         it->second, cur, rows);
                        }
                        if (dbg_prev_alive.size() < kDbgAliveCap ||
                            dbg_prev_alive.find(key) != dbg_prev_alive.end())
                            dbg_prev_alive[key] = cur;
                    }
        }
    }

    if (new_collected == g_collected_rows)
        return 0;
    // Log which rows were added/removed (for small deltas only)
    {
        std::vector<uint64_t> added, removed;
        for (auto r : new_collected)
            if (!g_collected_rows.count(r)) added.push_back(r);
        for (auto r : g_collected_rows)
            if (!new_collected.count(r)) removed.push_back(r);
        spdlog::info("[COLLECTED] Set changed: {} -> {} (added {}, removed {})",
                     g_collected_rows.size(), new_collected.size(), added.size(), removed.size());
        if (added.size() <= 10)
            for (auto r : added) spdlog::info("[COLLECTED]   +row {}", r);
        if (removed.size() <= 10)
            for (auto r : removed) spdlog::info("[COLLECTED]   -row {}", r);
    }

    // Restore all to visible, then re-hide collected.
    // Each write is SEH-guarded individually so a single stale pointer
    // (e.g. from another mod or game reloading the param file) gets
    // evicted instead of tripping the top-level SEH and skipping the
    // entire refresh cycle until the game is restarted.
    std::vector<uint64_t> stale;
    for (auto &[row_id, ref] : g_param_ptrs)
    {
        if (!safe_write_byte(ref.ptr + 0x20, ref.original_areaNo))
            stale.push_back(row_id);
    }

    int applied = 0, missed = 0;
    for (uint64_t row_id : new_collected)
    {
        auto pit = g_param_ptrs.find(row_id);
        if (pit != g_param_ptrs.end())
        {
            if (safe_write_byte(pit->second.ptr + 0x20, 99))
                applied++;
            else
                stale.push_back(row_id);
        }
        else
            missed++;
    }

    if (!stale.empty())
    {
        // Dedup and evict - these pointers are no longer writable.
        std::sort(stale.begin(), stale.end());
        stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
        for (auto id : stale)
            g_param_ptrs.erase(id);
        spdlog::warn("[COLLECTED] Dropped {} stale entries; {} remain",
                     stale.size(), g_param_ptrs.size());
    }
    // Publish it as well as logging it: skipped_count() is a public accessor that returned a
    // hard 0 for as long as this counter existed, because nothing ever assigned it.
    g_unmatched_count = missed;
    if (missed > 0)
        spdlog::warn("[COLLECTED] {} row IDs NOT in param_ptrs (out of {} collected)", missed, new_collected.size());

    int delta;
    {
        std::lock_guard<std::mutex> lk(g_collected_mutex);
        delta = (int)new_collected.size() - (int)g_collected_rows.size();
        g_collected_rows = std::move(new_collected);
        g_collected_count = (int)g_collected_rows.size();
    }

    spdlog::info("[COLLECTED] Refresh: {} hidden (delta {:+d}), {} GEOF entries, {} WGM tiles",
                 g_collected_count, delta, geof.size(), wgm_tiles.size());

    return delta;
}

void goblin::collected::forget_session_state()
{
    // Same thread as refresh() (the dllmain poll loop runs both, the slot sync after the refresh),
    // so the unlocked carry-forward walk in refresh() never runs against this clear; the lock is
    // for the readers on other threads. The next refresh() rebuilds the set from the NEW
    // character's GEOF/WGM alone, with nothing of the previous one to carry forward.
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(g_collected_mutex);
        dropped = g_collected_rows.size();
        g_collected_rows.clear();
        g_collected_count = 0;
    }
    g_unmatched_count = 0;
    if (dropped > 0)
        spdlog::info("[COLLECTED] character switch: {} row(s) of the previous character released", dropped);
}

// ─── queries ────────────────────────────────────────────────────────

bool goblin::collected::is_row_collected(uint64_t row_id)
{
    std::lock_guard<std::mutex> lk(g_collected_mutex);
    return g_collected_rows.count(row_id) > 0;
}

bool goblin::collected::is_original_row_collected(uint64_t original_row_id)
{
    auto it = g_original_to_dynamic.find(original_row_id);
    uint64_t id = (it != g_original_to_dynamic.end()) ? it->second : original_row_id;
    std::lock_guard<std::mutex> lk(g_collected_mutex);
    return g_collected_rows.count(id) > 0;
}

int goblin::collected::collected_count()
{
    return g_collected_count;
}

int goblin::collected::skipped_count()
{
    return g_unmatched_count;
}

// ─── field diagnostic for the marker dump ───────────────────────────

const goblin::generated::MapEntry *goblin::collected::entry_for_live_row(uint64_t live_row_id)
{
    auto it = g_dynamic_to_original.find(live_row_id);
    const uint64_t orig = (it != g_dynamic_to_original.end()) ? it->second : live_row_id;
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
        if (generated::MAP_ENTRIES[i].row_id == orig)
            return &generated::MAP_ENTRIES[i];
    return nullptr;
}

std::string goblin::collected::diagnose_rows(const std::vector<uint64_t> &live_row_ids)
{
    std::ostringstream o;
    if (!g_initialized)
    {
        o << "      collected: tracker not initialized\n";
        return o.str();
    }
    const auto wgm = read_wgm_snapshot();
    const auto geof = read_geof_from_memory();
    char buf[256];
    for (uint64_t id : live_row_ids)
    {
        const auto *e = entry_for_live_row(id);
        if (!e)
            continue;
        if (!e->object_name || !e->object_name[0])
        {
            o << "      row " << id << ": no MSB object bound (drop/scripted source) - collection tracking "
                 "cannot hide it\n";
            continue;
        }
        const std::string prefix = prefix_from_object_name(e->object_name);
        std::string geof_prefix = prefix;
        {
            auto *end = generated::GEOF_MODEL_OVERRIDES + generated::GEOF_MODEL_OVERRIDE_COUNT;
            auto *ov = std::lower_bound(
                generated::GEOF_MODEL_OVERRIDES, end, e->row_id,
                [](const generated::GeofModelOverride &ovr, uint64_t rid) { return ovr.row_id < rid; });
            if (ov != end && ov->row_id == e->row_id)
                geof_prefix = prefix_from_model_id(ov->model_id);
        }
        const uint32_t tile = encode_tile(e->data.areaNo, e->data.gridXNo, e->data.gridZNo);
        const bool hidden = is_row_collected(id);
        snprintf(buf, sizeof(buf),
                 "      row %llu obj=%s slot=%d tile=m%02u_%02u_%02u real=(%.2f,%.2f) model=%s -> %s\n",
                 (unsigned long long)id, e->object_name, (int)e->geom_slot, (unsigned)e->data.areaNo,
                 (unsigned)e->data.gridXNo, (unsigned)e->data.gridZNo, e->real_posX, e->real_posZ,
                 geof_prefix.c_str(), hidden ? "HIDDEN (in the collected set)" : "SHOWN (not collected)");
        o << buf;

        // WGM: the live instances of this model on the tile, nearest first.
        auto wt = wgm.find(tile);
        const bool loaded = wt != wgm.end();
        if (!loaded)
            o << "        WGM: tile not loaded (no live instances) -> GEOF decides\n";
        else
        {
            size_t total = 0;
            for (const auto &[p, sm] : wt->second.slot_insts)
                for (const auto &[s, v] : sm) total += v.size();
            auto pi = wt->second.slot_insts.find(geof_prefix);
            if (pi == wt->second.slot_insts.end())
            {
                snprintf(buf, sizeof(buf),
                         "        WGM: tile loaded, %zu tracked instance(s) on it, NONE of model %s -> "
                         "no WGM outcome, and GEOF is skipped for a loaded tile\n",
                         total, geof_prefix.c_str());
                o << buf;
            }
            else
            {
                struct Row { int slot; const WGMSnapshot::SlotInst *in; float d; };
                std::vector<Row> rows;
                for (const auto &[slot, v] : pi->second)
                    for (const auto &in : v)
                    {
                        const float dx = in.px - e->real_posX, dz = in.pz - e->real_posZ;
                        rows.push_back({slot, &in, std::sqrt(dx * dx + dz * dz)});
                    }
                std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.d < b.d; });
                snprintf(buf, sizeof(buf), "        WGM: tile loaded, %zu instance(s) of %s (%zu tracked total):\n",
                         rows.size(), geof_prefix.c_str(), total);
                o << buf;
                // Same rule as refresh(): the nearest instance whose MSB name carries this
                // row's slot if it lies within 4u; else the nearest instance within 4u; else
                // the same-named instance at any distance.
                const Row *byname = nullptr;
                for (const auto &r : rows)
                    if (r.in->suffix_slot == e->geom_slot && (!byname || r.d < byname->d)) byname = &r;
                const bool named_near = byname && byname->d <= 4.0f;
                const bool pos_match = !named_near && !rows.empty() && rows.front().d <= 4.0f;
                for (size_t i = 0; i < rows.size() && i < 8; ++i)
                {
                    const auto &r = rows[i];
                    const char *outcome = "";
                    if (named_near && byname == &r)
                        outcome = r.in->alive ? "  <- match (same name-slot, within 4u): ALIVE"
                                              : "  <- match (same name-slot, within 4u): dead -> collected";
                    else if (named_near && i == 0)
                        outcome = "  <- nearest, but a same-named twin within 4u takes precedence";
                    else if (pos_match && i == 0)
                        outcome = r.in->alive ? "  <- match: ALIVE" : "  <- match: dead -> collected";
                    else if (i == 0 && !byname)
                        outcome = "  <- nearest, but > 4u, and no instance is named with this row's slot: NO MATCH";
                    else if (i == 0)
                        outcome = "  <- nearest, but > 4u: position match fails, name-slot fallback below";
                    if (!named_near && !pos_match && byname == &r)
                        outcome = byname->in->alive ? "  <- name-slot match: ALIVE"
                                                    : "  <- name-slot match: dead -> collected";
                    snprintf(buf, sizeof(buf),
                             "          slot=%d name-slot=%d pos=(%.2f,%.2f) dist=%.1f alive=%d f263=0x%02X "
                             "f26B=0x%02X model=%u gidx=%u%s\n",
                             r.slot, r.in->suffix_slot, r.in->px, r.in->pz, r.d, r.in->alive ? 1 : 0,
                             (unsigned)r.in->f263, (unsigned)r.in->f26B, r.in->model_id, r.in->gidx, outcome);
                    o << buf;
                }
            }
        }

        // GEOF: the flag-save records for this model on the tile.
        int n = 0;
        for (const auto &g : geof)
        {
            if (g.tile_id != tile) continue;
            if (prefix_from_model_id(g.model_hash) != geof_prefix) continue;
            const int slot = aeg099_index_from_geof(g.geom_idx, g.flags);
            snprintf(buf, sizeof(buf), "        GEOF: slot=%d geom_idx=0x%04X flags=0x%02X%s\n", slot,
                     (unsigned)g.geom_idx, (unsigned)g.flags,
                     slot == e->geom_slot ? "  <- this row's slot" : "");
            o << buf;
            ++n;
        }
        if (n == 0)
        {
            snprintf(buf, sizeof(buf), "        GEOF: no record for %s on this tile\n", geof_prefix.c_str());
            o << buf;
        }
        if (loaded)
            o << "        rule: tile LOADED -> WGM decides: nearest instance within 4u, else the instance "
                 "whose MSB name carries this row's slot; a dead match hides. GEOF is not consulted "
                 "while loaded\n";
    }
    return o.str();
}
