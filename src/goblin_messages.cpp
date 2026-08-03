#include "goblin_messages.hpp"
#include "goblin_map_data.hpp"
#include "goblin_enemy_names.hpp"
#include "goblin_item_fallback.hpp"
#include "goblin_location_alt.hpp"
#include "goblin_config.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_i18n.hpp"
#include "goblin_inject.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "modutils.hpp"

#include <cstring>
#include <functional>
#include <set>
#include <spdlog/spdlog.h>
#include <deque>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace from::CS
{
class MsgRepositoryImp;
}

static from::CS::MsgRepositoryImp *msg_repository = nullptr;
static void *fmg_allocation = nullptr;

// Every PlaceName id that resolves to a real string after the FMG patch.
// Populated in patch_fmg_in_memory; read by sanitize_injected_textids().
static std::unordered_set<int32_t> g_placename_valid_ids;

// Collision-proof textId remap (see goblin_messages.hpp). Built in
// setup_messages(): offset-ENCODED key -> FRESH PlaceName id allocated
// contiguously above the runtime max. Empty until built -> identity.
static std::unordered_map<int32_t, int32_t> g_textid_remap;

int32_t goblin::remap_textid(int32_t encoded)
{
    auto it = g_textid_remap.find(encoded);
    return it != g_textid_remap.end() ? it->second : encoded;
}

// (Toggle state for the PlaceName FMG slot lived here - g_placename_slot_ptr and
//  g_vanilla_placename_fmg. Only slot 19 was ever a pointer swap; the other slots get surgical
//  in-place additions that are not undone. Both went with the toggle API that never had a caller.)

// Every MsgRepository slot we overwrote, with what it held before. See check_patched_slots().
struct PatchedSlot
{
    uint8_t **slot = nullptr;
    uint8_t *original = nullptr;
    uint8_t *ours = nullptr;
    bool reported = false;
};
static std::vector<PatchedSlot> g_patched_slots;
static uint8_t *g_expanded_placename_fmg = nullptr;
// PlaceName DLC layer FMGs (slots 329, 429). The game resolves a marker's
// PlaceName textId through these layers first, then the base slot; ids that live
// only in a DLC layer aren't in our expanded base buffer. Captured at setup so
// the marker dump can resolve location names the way the game does.
static uint8_t *g_placename_dlc_slots[2] = {nullptr, nullptr};

// ── SEH-guarded slot access ──
// Some runtimes keep STALE pointers in MsgRepository slots the game never
// initialized - observed under ERR's loader with the DLC-layer slots
// (dereferencing one access-violates; pre-1.0.15 code carried the same
// warning for ActionButtonText 365/465). A bad slot must degrade to a
// skipped layer, not kill the whole setup_messages init step (that is the
// "?PlaceName? everywhere" failure: the PlaceName patch never commits).
// seh_call has no C++ objects (MSVC C2712), the job body lives outside it.
static int seh_run_job_thunk(void *ctx)
{
    return (*static_cast<std::function<int()> *>(ctx))();
}

static int seh_call(int (*fn)(void *), void *ctx)
{
    __try { return fn(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static std::string detect_language()
{
    std::string lang = goblin::i18n::steam_game_language();
    return lang.empty() ? "english" : lang;
}

// Map a Steam game-language string (from detect_language) to an index into the
// embedded enemy-name table's ENEMY_NAME_LANGS (msgbnd codes). Falls back to 0
// (engus) for anything unmapped.
static int enemy_name_lang_index(const std::string &steam_lang)
{
    static const std::pair<const char *, const char *> STEAM_TO_MSGBND[] = {
        {"english", "engus"}, {"japanese", "jpnjp"}, {"german", "deude"},
        {"french", "frafr"}, {"italian", "itait"}, {"koreana", "korkr"},
        {"polish", "polpl"}, {"brazilian", "porbr"}, {"portuguese", "porbr"},
        {"russian", "rusru"}, {"spanish", "spaes"}, {"latam", "spaar"},
        {"thai", "thath"}, {"schinese", "zhocn"}, {"tchinese", "zhotw"},
        {"arabic", "araae"},
    };
    const char *code = "engus";
    for (auto &m : STEAM_TO_MSGBND)
        if (steam_lang == m.first) { code = m.second; break; }
    for (int i = 0; i < goblin::generated::ENEMY_NAME_LANG_COUNT; i++)
        if (std::strcmp(code, goblin::generated::ENEMY_NAME_LANGS[i]) == 0)
            return i;
    return 0;
}

// In-memory FMG binary layout (Elden Ring, version 2):
//   0x00: uint32 header    (0x00020000 = version 2 packed)
//   0x04: uint32 fileSize
//   0x08: uint32 unk08     (1)
//   0x0C: uint32 groupCount
//   0x10: uint32 stringCount
//   0x14: uint32 unk14     (0xFF)
//   0x18: uint64 stringOffsetsOffset (relative to FMG start)
//   0x20: 8 bytes zeros
//   0x28: groups[groupCount], each 16 bytes:
//         int32 stringIndex, int32 firstId, int32 lastId, int32 pad(0)
//   stringOffsetsOffset: uint64[stringCount] - each is offset from FMG start to UTF-16LE string
//   after offsets: UTF-16LE null-terminated string data

struct FmgGroup
{
    int32_t string_index;
    int32_t first_id;
    int32_t last_id;
    int32_t pad;
};

// Highest entry id in an FMG (walks its group table). -1 if empty/implausible.
// Used to allocate our injected ids ABOVE the live max so they never collide.
static int32_t fmg_max_id(uint8_t *fmg)
{
    if (!fmg) return -1;
    uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x0C);
    if (grp_cnt == 0 || grp_cnt > 0x100000) return -1;
    auto *groups = reinterpret_cast<FmgGroup *>(fmg + 0x28);
    int32_t mx = -1;
    // Ignore implausible group ids. Measured on ERR 2026-08-01: one PlaceName group carries
    // last_id = INT32_MAX, which made this return 2147483647, so the caller's "allocate fresh ids
    // above the max" started at INT32_MAX+1, tripped its own headroom guard on the FIRST entry and
    // allocated NOTHING - the log read "allocated 0 fresh ids ... for 20841 queued strings" on
    // every ERR startup, and the ids fell back to the original encoded ones with no collision
    // protection at all. Real FMG ids are in the millions; a billion is already far outside
    // anything the game or an overhaul ships, so a value at or above it is padding or a sentinel,
    // never content.
    constexpr int32_t kImplausibleId = 1'000'000'000;
    uint32_t skipped = 0;
    for (uint32_t g = 0; g < grp_cnt; g++)
    {
        const int32_t last = groups[g].last_id;
        if (last >= kImplausibleId)
        {
            ++skipped;
            continue;
        }
        if (last > mx) mx = last;
    }
    if (skipped)
        spdlog::info("[FMG] max-id scan skipped {} group(s) with a sentinel last_id (>= {}); "
                     "real max = {}", skipped, kImplausibleId, mx);
    return mx;
}

struct NewEntry
{
    int32_t id;
    const wchar_t *text;
};

// Defined below (after setup_messages); used by the GR_MenuText patch too.
static const wchar_t *fmg_lookup_in(uint8_t *fmg, int32_t id);

static bool patch_fmg_in_memory(uint8_t *fmg_ptr, uint8_t **slot_ptr,
                                const std::vector<NewEntry> &new_entries_in,
                                bool capture_valid_ids = false)
{
    // De-duplicate injected entries by id (first occurrence wins). Source FMG
    // group tables can cover the same id twice (observed in modded msgbnds),
    // and the two passes below (string-data append, then offset rebuild) MUST
    // agree on exactly which entries carry new strings: a single skipped
    // duplicate desynchronized every later string offset - labels after it
    // showed truncated/foreign text (reported in-game under The Convergence).
    std::vector<NewEntry> new_entries;
    {
        std::unordered_set<int32_t> seen;
        new_entries.reserve(new_entries_in.size());
        for (auto &ne : new_entries_in)
            if (seen.insert(ne.id).second)
                new_entries.push_back(ne);
        if (new_entries.size() != new_entries_in.size())
            spdlog::info("[FMG] {} duplicate added id(s) dropped",
                         new_entries_in.size() - new_entries.size());
    }
    uint32_t orig_file_size  = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x04);
    uint32_t orig_group_cnt  = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x0C);
    uint32_t orig_string_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x10);
    uint64_t raw_str_off     = *reinterpret_cast<uint64_t *>(fmg_ptr + 0x18);

    // Detect fixup: game converts relative offset to absolute pointer at runtime
    uint64_t orig_str_off_rel;
    uint8_t *orig_offsets_ptr;
    if (raw_str_off > 0x1000000)
    {
        orig_offsets_ptr = reinterpret_cast<uint8_t *>(raw_str_off);
        orig_str_off_rel = (uint64_t)(orig_offsets_ptr - fmg_ptr);
    }
    else
    {
        orig_str_off_rel = raw_str_off;
        orig_offsets_ptr = fmg_ptr + orig_str_off_rel;
    }


    auto *orig_groups = reinterpret_cast<FmgGroup *>(fmg_ptr + 0x28);
    auto *orig_offsets = reinterpret_cast<uint64_t *>(orig_offsets_ptr);

    struct ExistingEntry
    {
        int32_t id;
        uint64_t str_offset; // offset from FMG start
    };
    std::vector<ExistingEntry> all_entries;
    all_entries.reserve(orig_string_cnt + new_entries.size());

    for (uint32_t g = 0; g < orig_group_cnt; g++)
    {
        int32_t idx = orig_groups[g].string_index;
        int32_t fid = orig_groups[g].first_id;
        int32_t lid = orig_groups[g].last_id;
        for (int32_t id = fid; id <= lid; id++)
        {
            int32_t si = idx + (id - fid);
            if (si >= 0 && si < (int32_t)orig_string_cnt)
                all_entries.push_back({id, orig_offsets[si]});
        }
    }


    // Injected entries OVERRIDE pre-existing ids. Overhaul mods can pre-seed
    // rows at ids our offset encoding also uses (The Convergence ships
    // boss-text PlaceName rows in the 9xxM band, some with EMPTY text -
    // observed shadowing our "Summoning Pools" label at 900301690). Keeping
    // the old row would make the merge below skip ours, so drop it first.
    if (!new_entries.empty())
    {
        std::unordered_set<int32_t> override_ids;
        for (auto &ne : new_entries)
            override_ids.insert(ne.id);
        size_t before = all_entries.size();
        all_entries.erase(std::remove_if(all_entries.begin(), all_entries.end(),
                                         [&](const ExistingEntry &e)
                                         { return override_ids.count(e.id) != 0; }),
                          all_entries.end());
        if (before != all_entries.size())
            spdlog::info("[FMG] {} existing FMG ids overridden by added entries",
                         before - all_entries.size());
    }

    uint64_t orig_str_data_start = orig_str_off_rel + orig_string_cnt * 8;

    std::vector<uint8_t> new_str_data;
    size_t orig_str_data_len = orig_file_size - orig_str_data_start;
    new_str_data.resize(orig_str_data_len);
    memcpy(new_str_data.data(), fmg_ptr + orig_str_data_start, orig_str_data_len);

    struct AllEntry
    {
        int32_t id;
        uint64_t str_offset;
    };

    std::vector<AllEntry> merged;
    merged.reserve(all_entries.size() + new_entries.size());

    for (auto &e : all_entries)
        merged.push_back({e.id, e.str_offset});

    // A `pending` vector of (merged_idx, data_offset) was built here and never read: merged_idx is
    // stale the moment the entries are sorted, which is exactly why the offsets are re-derived from
    // the ids further down. Only the re-derivation was ever used.
    //
    // EVERY added entry gets a string appended, unconditionally. That is the invariant this loop and
    // the re-derivation at "Rebuild pending map" below must SHARE: they walk `new_entries` in the
    // same order, and the second one advances `data_pos` for exactly the entries the first one
    // appended. When the two disagreed by even one entry, every later string offset shifted and
    // labels after it drew truncated or foreign text (the duplicate-id case at the top of this
    // function, reported in game under The Convergence).
    //
    // There used to be a guard here that SKIPPED an added entry whose id was already in `merged`,
    // with a note saying it was kept on purpose to catch an overhaul shipping a row at an id our
    // offset encoding also uses. Two things were wrong with that (2026-07-31). It cannot fire:
    // `merged` is built only from `all_entries`, and the block above has just erased from
    // `all_entries` every id that appears in `new_entries`, so the override is already done by the
    // time we get here. And if it ever HAD fired it would have done the opposite of what it claimed:
    // `continue` skipped both the string append and the push, dropping OUR row and leaving the
    // pre-existing one - possibly the empty Convergence row that the erase above exists to displace.
    //
    // It is not replaced by a "defensive" version of itself either. A scan of `merged` per added
    // entry is O(added x merged) - tens of millions of comparisons at PlaceName scale - to test a
    // condition that is provably false, and it would have to carry a log line, which is dead text in
    // a binary that is judged on its readable strings. The override rule is enforced ONCE, where it
    // belongs: in the erase above. If that block is ever changed, this loop's precondition changes
    // with it, and that is the place to look.
    for (auto &ne : new_entries)
    {
        size_t str_start = new_str_data.size();
        size_t wlen = wcslen(ne.text);
        size_t byte_len = (wlen + 1) * sizeof(wchar_t);
        new_str_data.resize(new_str_data.size() + byte_len);
        memcpy(new_str_data.data() + str_start, ne.text, byte_len);

        merged.push_back({ne.id, 0}); // offset TBD, filled by the id-keyed re-derivation below
    }

    std::sort(merged.begin(), merged.end(),
              [](const AllEntry &a, const AllEntry &b) { return a.id < b.id; });

    // Record every PlaceName id that now resolves to a real string, so
    // sanitize_injected_textids() can strip marker textIds that point at a
    // missing entry (game's GetMessage returns null → wstring(null) → crash).
    // ONLY for the PlaceName (slot 19) patch - this function is also used for
    // the TutorialBody (codex toast) patch, whose id set is unrelated; capturing
    // that would wrongly flag every marker textId as missing and clear them all.
    if (capture_valid_ids)
    {
        g_placename_valid_ids.clear();
        for (auto &m : merged)
            g_placename_valid_ids.insert(m.id);
    }

    uint32_t total_strings = (uint32_t)merged.size();


    // One group per entry (sparse IDs)
    uint32_t total_groups = total_strings;

    constexpr size_t HEADER_SIZE = 0x28;
    size_t groups_size = total_groups * sizeof(FmgGroup);
    size_t new_str_off_pos = HEADER_SIZE + groups_size;
    size_t offsets_size = total_strings * sizeof(uint64_t);
    size_t new_str_data_start = new_str_off_pos + offsets_size;
    size_t new_file_size = new_str_data_start + new_str_data.size();


    // Allocate the expanded FMG buffer with the allocator the ENGINE will free it with.
    //
    // The engine does release these: at world unload it clears the MsgRepository slots we patch.
    // The release path was traced in full on 2026-08-04:
    //   ~MsgbndFileCap -> MsgRepositoryImp::ReleaseMsg -> the DL heap lookup, which finds the
    //   pointer in NO registered DL arena and falls back to
    //   DLKRD::HeapAllocator<DLKR::Win32RuntimeHeapImpl>::Free -> _aligned_free -> _free_base.
    //
    // _aligned_free does NOT free the pointer it is handed. It reads a back-pointer that the
    // matching _aligned_malloc stored at (p & ~7) - 8 and frees that instead. A buffer from plain
    // _malloc_base has no back-pointer there, so the engine read the XOR-encoded _HEAP_ENTRY that
    // precedes our block and passed it to RtlFreeHeap, which rejected it for not being 16-byte
    // aligned and terminated the process. That is the 0xC0000374 recorded on ten of ten quits.
    //
    // The old comment here claimed HeapAlloc(GetProcessHeap()) was "compatible". It was not: the
    // process has exactly one heap and it IS the CRT heap, so both paths were identically broken.
    // The v2.0.5 switch to the game's malloc neither caused this nor made it worse - the defect was
    // never about WHICH heap, it was about the missing _aligned_malloc back-pointer.
    //
    // There is no fallback any more, deliberately. Any other allocator here is a guaranteed crash at
    // shutdown; not expanding the FMG costs some marker names and nothing else.
    fmg_allocation = goblin::gfx_probe::game_aligned_alloc(new_file_size);
    if (!fmg_allocation)
    {
        spdlog::error("[FMG] aligned alloc unavailable ({} bytes) - expansion skipped so the "
                      "engine cannot free a buffer it did not align",
                      new_file_size);
        return false;
    }

    auto *nfmg = reinterpret_cast<uint8_t *>(fmg_allocation);

    *reinterpret_cast<uint32_t *>(nfmg + 0x00) = 0x00020000;
    *reinterpret_cast<uint32_t *>(nfmg + 0x04) = (uint32_t)new_file_size;
    *reinterpret_cast<uint32_t *>(nfmg + 0x08) = 1;
    *reinterpret_cast<uint32_t *>(nfmg + 0x0C) = total_groups;
    *reinterpret_cast<uint32_t *>(nfmg + 0x10) = total_strings;
    *reinterpret_cast<uint32_t *>(nfmg + 0x14) = 0xFF;
    // Game expects fixed-up absolute pointer, not relative offset
    *reinterpret_cast<uint64_t *>(nfmg + 0x18) = (uint64_t)(nfmg + new_str_off_pos);

    // Where each added string landed in new_str_data, keyed by id. The append loop's own indices
    // into `merged` are stale after the sort above, so this walks `new_entries` again and re-derives
    // the positions from the ids.
    //
    // UNCONDITIONAL, and it has to be: this loop advances `data_pos` by one string per entry, and
    // the append loop wrote one string per entry, so the two only agree if neither skips. It used to
    // carry its own skip test (`exists_in_orig`, over `all_entries`) mirroring the append loop's
    // skip test (over `merged`). Two separately-written predicates for one shared invariant is how a
    // desync gets introduced silently - and the two were NOT the same test. Both are gone; the
    // invariant is now "one string per added entry" on both sides, with nothing to disagree about.
    std::unordered_map<int32_t, size_t> new_entry_data_offset;
    {
        size_t data_pos = orig_str_data_len; // where new strings start in new_str_data
        for (auto &ne : new_entries)
        {
            new_entry_data_offset[ne.id] = new_str_data_start + data_pos;
            size_t wlen = wcslen(ne.text);
            data_pos += (wlen + 1) * sizeof(wchar_t);
        }
    }

    auto *new_groups = reinterpret_cast<FmgGroup *>(nfmg + HEADER_SIZE);
    auto *new_offsets = reinterpret_cast<uint64_t *>(nfmg + new_str_off_pos);

    for (uint32_t i = 0; i < total_strings; i++)
    {
        new_groups[i].string_index = (int32_t)i;
        new_groups[i].first_id = merged[i].id;
        new_groups[i].last_id = merged[i].id;
        new_groups[i].pad = 0;

        if (merged[i].str_offset != 0)
        {
            // Remap old relative offset into new layout
            uint64_t old_off = merged[i].str_offset;
            if (old_off >= orig_str_data_start)
            {
                uint64_t within = old_off - orig_str_data_start;
                new_offsets[i] = new_str_data_start + within;
            }
            else
            {
                new_offsets[i] = 0;
            }
        }
        else
        {
            auto it = new_entry_data_offset.find(merged[i].id);
            if (it != new_entry_data_offset.end())
                new_offsets[i] = it->second;
            else
                new_offsets[i] = 0;
        }
    }

    memcpy(nfmg + new_str_data_start, new_str_data.data(), new_str_data.size());

    // Remember what the engine had here. Overwriting a MsgRepository slot means the engine may one day
    // free OUR buffer with the allocator its own msgbnd loader used - the open question from the
    // 2026-07-28 audit. Keeping the original lets us hand it back, and the watch below answers whether
    // the engine ever swaps the slot on its own (a language change or a msgbnd reload would).
    g_patched_slots.push_back({slot_ptr, fmg_ptr, nfmg});
    *slot_ptr = nfmg;

    return true;
}

// VERIFY (2026-07-28 audit): does the engine ever replace a slot we patched? Called from the periodic
// refresh; logs the first time any slot stops pointing at our buffer.
void goblin::check_patched_slots()
{
    if (!goblin::config::debugLogging)
        return;
    for (auto &ps : g_patched_slots)
    {
        if (ps.reported || !ps.slot)
            continue;
        uint8_t *now = nullptr;
        __try
        {
            now = *ps.slot;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            continue;
        }
        if (now != ps.ours)
        {
            ps.reported = true;
            spdlog::warn("[verify] a patched MsgRepository slot changed under us: ours=0x{:X} now=0x{:X} "
                         "(original was 0x{:X}) - the engine DOES replace these, so the restore-on-detach "
                         "path is required",
                         (uint64_t)ps.ours, (uint64_t)now, (uint64_t)ps.original);
        }
    }
}


void goblin::setup_messages()
{
    using namespace goblin::generated;

    auto lang = detect_language();
    spdlog::debug("Detected language: {}", lang);

    std::vector<NewEntry> new_entries;


    // Collect item IDs used as textId - need to copy from various FMGs to PlaceName
    // FmgId slots: GoodsName=10, WeaponName=11, ProtectorName=12, AccessoryName=13,
    //              MagicName=14, NpcName=18, PlaceName=19, GemName=35, ArtsName=42,
    //              TutorialTitle=207
    // textId encoding: real_id + category_offset
    //   100000000+id:      WeaponName (cat=2, weapons)
    //   200000000+id:      ProtectorName (cat=3, armour)
    //   300000000+id:      AccessoryName (cat=4, talismans)
    //   400000000+id:      GemName (cat=5, ashes of war)
    //   500000000+id:      GoodsName (cat=1, goods)
    //   (600M band = EventTextForMap is RESERVED but unused - no marker emits it in
    //    any profile; resolving it would need the menu msgbnd bank. See
    //    docs/messages_offline_research.md.)
    //   700000000+id:      NpcName (named-NPC and 9MMMMVVV "Characters" boss names)
    //   800000000+id:      ActionButtonText (in-game interact prompts, e.g. "Examine statue")
    //   900000000+id:      TutorialTitle (enemy names)
    //   950000000+id:      BloodMsg (message-builder vocabulary words - generic
    //                      enemy-type labels for the vanilla build: "skeleton",
    //                      "demi-human", ... localized in all languages)
    // NpcName has two ID ranges in vanilla+ERR: small ids (Patches=130900,
    // Garris=137600) and 9MMMMVVV "Characters" (Stray Mimic Tear=903320300).
    // With +700M offset, small ids land in [700M..800M); the large 9MMMMVVV ids
    // land in [1600M..1700M). Both ranges classify here.
    // Legacy (pre-offset): raw PlaceName IDs < 100M for location names
    std::set<int32_t> goods_ids_needed;      // GoodsName FMG, slot 10
    std::set<int32_t> weapon_ids_needed;     // WeaponName FMG, slot 11
    std::set<int32_t> protector_ids_needed;  // ProtectorName FMG, slot 12
    std::set<int32_t> accessory_ids_needed;  // AccessoryName FMG, slot 13
    std::set<int32_t> gem_ids_needed;        // GemName FMG, slot 14
    std::set<int32_t> npc_name_ids_needed;   // NpcName FMG, slot 18 (+ DLC 328, 428)
    std::set<int32_t> action_btn_ids_needed; // ActionButtonText FMG, slot 32 (+ DLC 365, 465)
    std::set<int32_t> tutorial_ids_needed;   // TutorialTitle FMG, slot 207 (enemy names from ERR Codex)
    std::set<int32_t> bloodmsg_ids_needed;   // BloodMsg FMG, slot 2 (vanilla enemy-type words, offset 950M)
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        const int32_t *text_ids[] = {
            &e.data.textId1, &e.data.textId2, &e.data.textId3, &e.data.textId4,
            &e.data.textId5, &e.data.textId6, &e.data.textId7, &e.data.textId8,
        };
        for (auto *tidp : text_ids)
        {
            int32_t tid = *tidp;
            if (tid <= 0) continue;
            if (tid >= 1600000000 && tid < 1700000000)   // NpcName "Characters" hi-range (9MMMMVVV+700M)
                npc_name_ids_needed.insert(tid);
            else if (tid >= 950000000 && tid < 960000000) // BloodMsg vocabulary (offset 950M)
                bloodmsg_ids_needed.insert(tid);
            else if (tid >= 900000000)          // TutorialTitle (enemy names)
                tutorial_ids_needed.insert(tid);
            else if (tid >= 800000000 && tid < 900000000) // ActionButtonText (offset 800M)
                action_btn_ids_needed.insert(tid);
            else if (tid >= 700000000 && tid < 800000000) // NpcName low-range (small id+700M)
                npc_name_ids_needed.insert(tid);
            else if (tid >= 500000000 && tid < 600000000) // GoodsName (goods, offset 500M)
                goods_ids_needed.insert(tid);
            else if (tid >= 400000000)          // GemName (ashes of war, offset 400M)
                gem_ids_needed.insert(tid);
            else if (tid >= 300000000)          // AccessoryName (talismans, offset 300M)
                accessory_ids_needed.insert(tid);
            else if (tid >= 200000000)          // ProtectorName (armour, offset 200M)
                protector_ids_needed.insert(tid);
            else if (tid >= 100000000)          // WeaponName (weapons, offset 100M)
                weapon_ids_needed.insert(tid);
            // IDs < 100M are raw PlaceName IDs (location names, no copy needed)
        }
    }

    auto msg_repository_address = modutils::scan<from::CS::MsgRepositoryImp *>({
        .aob = "48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75",
        .relative_offsets = {{3, 7}},
    });
    if (!msg_repository_address) { spdlog::error("MsgRepositoryImp not found"); return; }

    while (!(msg_repository = *msg_repository_address))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));


    // PlaceName = bnd index 19 in MsgRepositoryImp
    auto *repo = reinterpret_cast<uint8_t *>(msg_repository);
    auto base_array = *reinterpret_cast<uint8_t ***>(repo + 0x08);
    int32_t count2 = *reinterpret_cast<int32_t *>(repo + 0x14);

    if (!base_array || !base_array[0] || 19 >= count2)
    {
        spdlog::error("Cannot navigate to PlaceName FMG slot");
        return;
    }

    auto **sub = reinterpret_cast<uint8_t **>(base_array[0]);

    // Helper: copy entries from an FMG to PlaceName, with offset remapping.
    // needed_ids contains offset-encoded IDs; real FMG id = offset_id - offset_base.
    // The PlaceName entry is written at the offset-encoded ID. Satisfied ids
    // are ERASED from needed_ids so layered calls only fill what is missing.
    auto copy_fmg_entries = [&](uint8_t *fmg_ptr, std::set<int32_t> &needed_ids,
                                int32_t offset_base, const char *label, int slot) -> int
    {
        if (needed_ids.empty() || !fmg_ptr) return 0;

        uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x0C);
        uint32_t str_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x10);
        uint64_t raw_off = *reinterpret_cast<uint64_t *>(fmg_ptr + 0x18);

        // Sanity-guard the header before walking: a stale/foreign slot pointer
        // would otherwise send us through garbage group tables.
        if (grp_cnt > 0x100000 || str_cnt > 0x100000 || raw_off == 0)
        {
            spdlog::debug("{}: slot {} header implausible (groups={}, strings={}) - skipped",
                          label, slot, grp_cnt, str_cnt);
            return 0;
        }

        uint8_t *off_ptr = (raw_off > 0x1000000)
            ? reinterpret_cast<uint8_t *>(raw_off)
            : fmg_ptr + raw_off;

        auto *groups = reinterpret_cast<FmgGroup *>(fmg_ptr + 0x28);
        auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);

        // Build reverse lookup: real_id -> offset_id (for ammo: offset_base=0, id stored as-is)
        std::unordered_map<int32_t, int32_t> real_to_offset;
        for (int32_t oid : needed_ids)
            real_to_offset[oid - offset_base] = oid;

        int copied = 0;
        for (uint32_t g = 0; g < grp_cnt; g++)
        {
            int32_t first = groups[g].first_id;
            int32_t last = groups[g].last_id;
            int32_t si = groups[g].string_index;
            for (int32_t id = first; id <= last; id++, si++)
            {
                if (si < 0 || si >= (int32_t)str_cnt) continue;
                auto it = real_to_offset.find(id);
                if (it == real_to_offset.end()) continue;

                uint64_t s_off = str_offs[si];
                if (s_off == 0) continue;
                const wchar_t *text = (s_off > 0x1000000)
                    ? reinterpret_cast<const wchar_t *>(s_off)
                    : reinterpret_cast<const wchar_t *>(fmg_ptr + s_off);
                if (text && text[0])
                {
                    new_entries.push_back({it->second, text});  // write at offset-encoded ID
                    needed_ids.erase(it->second);
                    // also drop from the reverse map: FMG group tables can
                    // cover the same id twice - never push it twice
                    real_to_offset.erase(it);
                    copied++;
                }
            }
        }
        if (copied)
            spdlog::info("Copied {} {} entries to PlaceName (slot {})", copied, label, slot);
        return copied;
    };

    // The runtime keeps every msgbnd FMG layer in its OWN MsgRepository slot
    // (base / _dlc01 / _dlc02) and merges them at LOOKUP time, highest layer
    // first - verified live: WeaponName base slot 11 does NOT contain ids that
    // ship in WeaponName_dlc01 (slot 310). Overhaul mods (The Convergence) put
    // their added strings in the DLC layers, so the base slot alone misses
    // them. Walk the layers in the game's lookup priority; copy_fmg_entries
    // erases satisfied ids, so later (lower-priority) layers only fill gaps.
    auto copy_fmg_layered = [&](std::initializer_list<int> slots,
                                std::set<int32_t> &needed_ids,
                                int32_t offset_base, const char *label) -> int
    {
        int total = 0;
        for (int slot : slots)
        {
            if (needed_ids.empty()) break;
            if (slot >= count2 || !sub[slot]) continue;
            // SEH-guard each slot: stale slot pointers (ERR loader, or a
            // vanilla install without the DLC) must not kill the init step.
            // Roll both outputs back on failure so a partial walk over a
            // garbage FMG never leaks bogus entries into the patch.
            size_t entries_mark = new_entries.size();
            std::set<int32_t> ids_snapshot = needed_ids;
            std::function<int()> job = [&, slot]() -> int
            { return copy_fmg_entries(sub[slot], needed_ids, offset_base, label, slot); };
            int copied = seh_call(&seh_run_job_thunk, &job);
            if (copied < 0)
            {
                new_entries.resize(entries_mark);
                needed_ids = std::move(ids_snapshot);
                spdlog::warn("{}: slot {} not readable in this runtime - layer skipped", label, slot);
                continue;
            }
            total += copied;
        }
        return total;
    };

    // Live-loot labels (config::liveLootLabels): the randomizer can place ANY
    // item at a loot light-point, so we can't know at bake time which item id
    // a marker will need. Copy EVERY entry from a source FMG family into
    // PlaceName at its offset-encoded id, so refresh_loot_from_itemlot() can
    // point a marker's textId1 at whatever item the live ItemLotParam now gives.
    // first-hit-wins across layers (dlc02 → dlc01 → base), same as the targeted
    // walk. ammo_as_is: WeaponName ammo ids (>=50M) are stored unshifted.
    auto copy_fmg_all_layered = [&](std::initializer_list<int> slots, int32_t offset_base,
                                    const char *label, bool ammo_as_is) -> int
    {
        std::unordered_set<int32_t> seen;
        int total = 0;
        for (int slot : slots)
        {
            if (slot >= count2 || !sub[slot]) continue;
            size_t entries_mark = new_entries.size();
            std::function<int()> job = [&, slot]() -> int
            {
                int copied = 0;
                uint8_t *f = sub[slot];
                uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(f + 0x0C);
                uint32_t str_cnt = *reinterpret_cast<uint32_t *>(f + 0x10);
                uint64_t raw_off = *reinterpret_cast<uint64_t *>(f + 0x18);
                if (grp_cnt > 0x100000 || str_cnt > 0x100000 || raw_off == 0) return 0;
                uint8_t *off_ptr = (raw_off > 0x1000000)
                    ? reinterpret_cast<uint8_t *>(raw_off) : f + raw_off;
                auto *groups = reinterpret_cast<FmgGroup *>(f + 0x28);
                auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);
                for (uint32_t g = 0; g < grp_cnt; g++)
                {
                    int32_t si = groups[g].string_index;
                    for (int32_t id = groups[g].first_id; id <= groups[g].last_id; id++, si++)
                    {
                        if (si < 0 || si >= (int32_t)str_cnt) continue;
                        if (!seen.insert(id).second) continue; // higher layer already won
                        uint64_t s_off = str_offs[si];
                        if (s_off == 0) continue;
                        const wchar_t *text = (s_off > 0x1000000)
                            ? reinterpret_cast<const wchar_t *>(s_off)
                            : reinterpret_cast<const wchar_t *>(f + s_off);
                        if (!text || !text[0]) continue;
                        int32_t enc = (ammo_as_is && id >= 50000000) ? id : id + offset_base;
                        new_entries.push_back({enc, text});
                        copied++;
                    }
                }
                return copied;
            };
            int copied = seh_call(&seh_run_job_thunk, &job);
            if (copied < 0)
            {
                new_entries.resize(entries_mark);
                spdlog::warn("{}(all): slot {} not readable in this runtime - layer skipped", label, slot);
                continue;
            }
            total += copied;
        }
        if (total)
            spdlog::info("Copied ALL {} {} entries to PlaceName (live-loot labels)", total, label);
        return total;
    };

    // Slot lists per FMG family, in the game's lookup-priority order
    // (dlc02 → dlc01 → base). The ERR build walks ONLY the base slots:
    // everything its bake references lives there (proven through v1.0.14),
    // and the DLC-layer slots were observed to hold stale pointers under
    // ERR's loader - touching them caused the "?PlaceName?" incident in
    // v1.0.15 (SEH killed setup_messages before the PlaceName patch).
#ifdef MFG_VANILLA
    const std::initializer_list<int>
        goods_slots{419, 319, 10}, weapon_slots{410, 310, 11},
        protector_slots{413, 313, 12}, accessory_slots{416, 316, 13},
        gem_slots{422, 322, 35, 42}, npc_slots{428, 328, 18},
        bloodmsg_slots{461, 361, 2}, tutorial_slots{475, 375, 207};
#else
    const std::initializer_list<int>
        goods_slots{10}, weapon_slots{11},
        protector_slots{12}, accessory_slots{13},
        gem_slots{35, 42}, npc_slots{18},
        bloodmsg_slots{2}, tutorial_slots{207};
#endif

    // GoodsName. Offset 500M.
    copy_fmg_layered(goods_slots, goods_ids_needed, 500000000, "GoodsName");

    // WeaponName. Ammo IDs are stored as-is (>=50M), weapon IDs are offset by 100M.
    {
        std::set<int32_t> ammo_ids, weapon_offset_ids;
        for (int32_t tid : weapon_ids_needed)
        {
            if (tid >= 100000000)
                weapon_offset_ids.insert(tid);
            else
                ammo_ids.insert(tid);
        }
        copy_fmg_layered(weapon_slots, ammo_ids, 0, "WeaponName(ammo)");
        copy_fmg_layered(weapon_slots, weapon_offset_ids, 100000000, "WeaponName(weapon)");
    }

    // ProtectorName. Offset 200M.
    copy_fmg_layered(protector_slots, protector_ids_needed, 200000000, "ProtectorName");

    // AccessoryName. Offset 300M.
    copy_fmg_layered(accessory_slots, accessory_ids_needed, 300000000, "AccessoryName");

    // GemName (ArtsName 42 as fallback). Offset 400M.
    copy_fmg_layered(gem_slots, gem_ids_needed, 400000000, "GemName");

    // Live-loot labels: pull the WHOLE item-name space into PlaceName so a
    // marker can be relabeled to any randomized item at runtime. Offsets match
    // the encoding above; the targeted copies stay (they're a subset, deduped
    // first-wins by patch_fmg_in_memory). Gated - off by default it adds nothing.
    if (goblin::config::liveLootLabels)
    {
        copy_fmg_all_layered(goods_slots, 500000000, "GoodsName", false);
        copy_fmg_all_layered(weapon_slots, 100000000, "WeaponName", true);
        copy_fmg_all_layered(protector_slots, 200000000, "ProtectorName", false);
        copy_fmg_all_layered(accessory_slots, 300000000, "AccessoryName", false);
        copy_fmg_all_layered(gem_slots, 400000000, "GemName", false);
    }

    // NpcName. Offset 700M.
    // (Was break-on-first-success from base 18 - that missed every id living
    // only in a DLC layer, e.g. The Convergence's added bosses in slot 328.)
    copy_fmg_layered(npc_slots, npc_name_ids_needed, 700000000, "NpcName");

    // Read ActionButtonText FMG (slots 32 + DLC 365, 465): in-game interact
    // prompts ("Examine statue", "Light bonfire", ...). Offset 800M.
    // ActionButtonText lives in menu.msgbnd but MsgRepositoryImp's FMG slot
    // array is indexed by global BND file ID - slot 32 returns the menu
    // ActionButtonText FMG directly, no separate bank navigation needed.
    // DLC slots merge their own entries on top (e.g. DLC2 7041 = "Examine statue").
    // Slot 32 = ActionButtonText.fmg. ER runtime already merges DLC entries
    // into the base FMG at this slot - we observed 7041 ("Examine statue",
    // added in DLC02) being readable from slot 32 in-game. Don't poke DLC
    // slots 365/465 directly: in past testing accessing those past the
    // valid count2 range derailed downstream FMG copies (TutorialTitle
    // never ran and PlaceName patch never committed → "?PlaceName?" text).
    if (!action_btn_ids_needed.empty() && 32 < count2 && sub[32])
        copy_fmg_entries(sub[32], action_btn_ids_needed, 800000000, "ActionButtonText", 32);

    // Read BloodMsg FMG (slot 2, menu.msgbnd): message-builder vocabulary.
    // The vanilla build labels generic enemy drops with the closest vocabulary
    // word ("skeleton", "demi-human", ...) - localized for free, same as the
    // in-game message composer. Offset 950M. Same direct-slot access as
    // ActionButtonText above (slot array is indexed by global BND file ID).
    // BloodMsg layers (vocabulary is base-game, but overhauls may extend it).
    // Spoiler-free mode: pull the generic localized label ("something",
    // BloodMsg word 32004) into PlaceName so anonymous loot markers can point at
    // it. Offset 950M, same band as the vanilla enemy-type vocabulary.
    // ALWAYS inject it (not gated on anonymousLoot): the marker re-point to this
    // id happens live when the user toggles anonymous_loot in the overlay, but FMG
    // text injection is init-only - so without this, a live toggle shows
    // "?PlaceName?" (the id has no PlaceName entry). One harmless extra entry.
    bloodmsg_ids_needed.insert(950000000 + 32004);
    copy_fmg_layered(bloodmsg_slots, bloodmsg_ids_needed, 950000000, "BloodMsg");

    // TutorialTitle (ERR Codex enemy names + category labels like "Summoning
    // Pools"). textId in MASSEDIT = real TutorialTitle ID + 900000000 (to
    // avoid collision with GoodsName).
    copy_fmg_layered(tutorial_slots, tutorial_ids_needed, 900000000, "TutorialTitle");

    // Non-ERR builds carry their own localized enemy-name table (the runtime
    // FMG has only generic tutorial entries, no enemy names). Inject the names
    // for the detected language into PlaceName at the same +900M encoding the
    // markers use, so enemy-drop labels read properly instead of falling back to
    // the generic vocabulary words. The ERR build ships an empty table and uses
    // its runtime names, so this loop is a no-op there.
    if (generated::ENEMY_NAME_COUNT > 0)
    {
        int li = enemy_name_lang_index(lang);
        int added = 0;
        for (size_t i = 0; i < generated::ENEMY_NAME_COUNT; i++)
        {
            const auto &en = generated::ENEMY_NAMES[i];
            const wchar_t *nm = en.names[li] ? en.names[li] : en.names[0];
            if (nm && nm[0])
            {
                new_entries.push_back({en.id + 900000000, nm});
                added++;
            }
        }
        spdlog::info("Added {} enemy names into PlaceName (lang index {})", added, li);
    }

    auto *fmg_ptr = sub[19];

    if (!fmg_ptr)
    {
        spdlog::error("PlaceName FMG (bnd=19) is null");
        return;
    }


    // Composed labels for duplicate-named sub-zones (e.g. the two Hallowhorn Grounds):
    // synthesize "<sub> (<super>)" from the game's OWN PlaceName strings so the text is
    // correct in any language. Storage is a function-local static deque - pointer-stable.
    {
        auto fmg_find = [&](uint8_t *fmg, int32_t id) -> const wchar_t *
        {
            if (!fmg) return nullptr;
            uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x0C);
            uint32_t str_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x10);
            uint64_t raw_off = *reinterpret_cast<uint64_t *>(fmg + 0x18);
            // Sanity-bound a possibly-stale slot (see fmg_lookup_in) before walking it.
            if (grp_cnt == 0 || grp_cnt > 0x100000 || str_cnt > 0x200000 || raw_off == 0)
                return nullptr;
            uint8_t *off_ptr = (raw_off > 0x1000000) ? reinterpret_cast<uint8_t *>(raw_off) : fmg + raw_off;
            auto *groups = reinterpret_cast<FmgGroup *>(fmg + 0x28);
            auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);
            for (uint32_t g = 0; g < grp_cnt; g++)
            {
                if (id < groups[g].first_id || id > groups[g].last_id) continue;
                int32_t si = groups[g].string_index + (id - groups[g].first_id);
                if (si < 0 || si >= (int32_t)str_cnt) return nullptr;
                uint64_t s_off = str_offs[si];
                if (s_off == 0) return nullptr;
                return (s_off > 0x1000000) ? reinterpret_cast<const wchar_t *>(s_off)
                                           : reinterpret_cast<const wchar_t *>(fmg + s_off);
            }
            return nullptr;
        };
        // Look the parts up the way the game does: DLC PlaceName layers first
        // (429, 329), then the base slot - overhauls keep reworked zone names
        // in the DLC layers only. ERR build: base slot only (its DLC slot
        // pointers can be stale - see the slot-list comment above).
        auto find_layered = [&](int32_t id) -> const wchar_t *
        {
#ifdef MFG_VANILLA
            for (int slot : {429, 329})
                if (slot < count2 && sub[slot])
                    if (const wchar_t *t = fmg_find(sub[slot], id); t && t[0])
                        return t;
#endif
            return fmg_find(fmg_ptr, id);
        };
        static std::deque<std::wstring> compose_storage;
        int composed = 0;
        for (size_t i = 0; i < generated::LOCATION_COMPOSE_COUNT; i++)
        {
            const auto &c = generated::LOCATION_COMPOSE[i];
            const wchar_t *sub_txt = find_layered(c.subId);
            const wchar_t *sup_txt = find_layered(c.superId);
            if (!sub_txt || !sup_txt)
            {
                spdlog::warn("Compose label {}: PlaceName {} or {} not found in FMG", c.id, c.subId, c.superId);
                continue;
            }
            compose_storage.emplace_back(std::wstring(sub_txt) + L" (" + sup_txt + L")");
            new_entries.push_back({c.id, compose_storage.back().c_str()});
            composed++;
        }
        if (composed)
            spdlog::info("Composed {} duplicate-zone labels into PlaceName", composed);
    }

    // English item-name fallback (LOWEST priority). For every loot item a marker can
    // reference, add its English name at the same offset-encoded id. patch_fmg_in_memory
    // dedups first-occurrence-wins, and the localized copies above were pushed first, so this
    // only fills ids the player's language left empty - a marker whose item has no string in
    // the active language then shows the English name instead of "?PlaceName?". Overhauls
    // (e.g. The Convergence) ship many items localized only in English; this catches those.
    //
    // CRITICAL gap-fill gate for RAW PlaceName location ids (< 100M). Unlike the
    // offset-encoded item/boss bands (which the game's PlaceName never carries, so
    // queuing them is always safe), a raw location id usually DOES resolve natively
    // in the player's language - and queuing it here would create a g_textid_remap
    // entry that hijacks the marker's location textId onto our English copy, showing
    // English locations/graces on a fully-localized build (e.g. ERR in Russian). So
    // for a raw location id, only queue the English fallback when the native FMG (the
    // current language's PlaceName, incl. the DLC layers the game falls back to) has
    // NO string for it - a true gap. Offset-encoded ids (>= 100M) are queued as before.
    {
        size_t before = new_entries.size();
        auto native_has = [&](int32_t id) -> bool
        {
#ifdef MFG_VANILLA
            for (int slot : {429, 329})
                if (slot < count2 && sub[slot] && fmg_lookup_in(sub[slot], id))
                    return true;
#endif
            return fmg_lookup_in(fmg_ptr, id) != nullptr;
        };
        for (size_t i = 0; i < generated::ITEM_NAME_FALLBACK_COUNT; ++i)
        {
            const auto &fe = generated::ITEM_NAME_FALLBACK[i];
            if (!fe.name || !fe.name[0])
                continue;
            // raw PlaceName location id that the player's language already has -> let the
            // marker pass through to the native localized string (no remap, no override)
            if (fe.id > 0 && fe.id < 100000000 && native_has(fe.id))
                continue;
            new_entries.push_back({fe.id, fe.name});
        }
        if (new_entries.size() != before)
            spdlog::info("Queued {} English item-name fallback entries (fill gaps only)",
                         new_entries.size() - before);
    }

    // --- Collision-proof remap ---------------------------------------------
    // Every queued string is RELOCATED from its offset-encoded id to a FRESH id
    // allocated contiguously ABOVE the runtime max PlaceName id, in new_entries
    // push order (so each producer's strings form a contiguous, dynamically-sized
    // run - never a fixed-width band, so they can't overflow into each other no
    // matter how many strings a profile has). g_textid_remap then drives
    // remap_textid() so live marker textIds point at the fresh ids. Result: our
    // ids sit strictly above anything the game / overhaul / another mod put in
    // PlaceName, so they can never collide. Raw PlaceName location ids (never
    // queued here) are untouched and pass through remap_textid() unchanged.
    {
        int32_t max_id = 0;
        for (int slot : {19, 329, 429})   // PlaceName base + DLC layers (game merges these)
            if (slot < count2 && sub[slot])
                max_id = std::max(max_id, fmg_max_id(sub[slot]));

        int64_t next = (int64_t)max_id + 1;
        g_textid_remap.clear();
        g_textid_remap.reserve(new_entries.size());
        bool overflow = false;
        for (auto &e : new_entries)
        {
            auto it = g_textid_remap.find(e.id);
            if (it != g_textid_remap.end()) { e.id = it->second; continue; }  // dup original -> same fresh
            if (next > 2'100'000'000)  // int32 headroom guard (never hit in practice)
            { overflow = true; break; }
            int32_t fresh = (int32_t)next++;
            g_textid_remap[e.id] = fresh;   // key = ORIGINAL encoded id
            e.id = fresh;
        }
        if (overflow)
            spdlog::error("[FMG] remap: id space exhausted above max={} - some labels may be unresolved", max_id);
        spdlog::info("[FMG] remap: PlaceName max={}, allocated {} fresh ids [{}..{}) for {} queued strings (contiguous, above max)",
                     max_id, (uint32_t)g_textid_remap.size(), max_id + 1, (int32_t)next, new_entries.size());
    }

    if (patch_fmg_in_memory(fmg_ptr, &sub[19], new_entries, /*capture_valid_ids=*/true))
    {
        spdlog::info("PlaceName FMG merged ({} entries)", new_entries.size());
        // sub[19] now points at our expanded buffer (set inside patch_fmg_in_memory). The DLC
        // layers are captured because the game resolves PlaceName through them FIRST.
        // (Two more captures stood here, plus an "injection is active" flag, for the runtime
        //  vanilla/expanded toggle that never had a caller.)
        g_expanded_placename_fmg = sub[19];
        g_placename_dlc_slots[0] = (329 < count2) ? sub[329] : nullptr;
        g_placename_dlc_slots[1] = (429 < count2) ? sub[429] : nullptr;

#ifdef MFG_VANILLA
        // The game resolves PlaceName through the DLC layers FIRST (slots 429,
        // 329) and falls back to base slot 19. Ids that live only in a DLC
        // layer (vanilla DLC zones; The Convergence's reworked zones and boss
        // texts) are perfectly valid marker textIds even though our expanded
        // base FMG doesn't carry them - whitelist them so the sanitizer
        // doesn't clear those labels (observed: 1330 false-cleared under The
        // Convergence, whose layers hold 520+ dlc01-only zone ids).
        // Not compiled for ERR: its bake never relies on DLC-layer-only ids,
        // and its DLC slot pointers can be stale (see the slot-list comment).
        size_t dlc_valid = 0;
        for (int slot : {329, 429})
        {
            if (slot >= count2 || !sub[slot]) continue;
            std::function<int()> job = [&]() -> int
            {
                int found = 0;
                uint8_t *f = sub[slot];
                uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(f + 0x0C);
                uint32_t str_cnt = *reinterpret_cast<uint32_t *>(f + 0x10);
                uint64_t raw_off = *reinterpret_cast<uint64_t *>(f + 0x18);
                if (grp_cnt > 0x100000 || str_cnt > 0x100000 || raw_off == 0) return 0;
                uint8_t *off_ptr = (raw_off > 0x1000000)
                    ? reinterpret_cast<uint8_t *>(raw_off) : f + raw_off;
                auto *groups = reinterpret_cast<FmgGroup *>(f + 0x28);
                auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);
                for (uint32_t g = 0; g < grp_cnt; g++)
                {
                    int32_t si = groups[g].string_index;
                    for (int32_t id = groups[g].first_id; id <= groups[g].last_id; id++, si++)
                    {
                        if (si < 0 || si >= (int32_t)str_cnt) continue;
                        uint64_t s_off = str_offs[si];
                        if (s_off == 0) continue;
                        const wchar_t *text = (s_off > 0x1000000)
                            ? reinterpret_cast<const wchar_t *>(s_off)
                            : reinterpret_cast<const wchar_t *>(f + s_off);
                        if (text && text[0] && g_placename_valid_ids.insert(id).second)
                            found++;
                    }
                }
                return found;
            };
            int found = seh_call(&seh_run_job_thunk, &job);
            if (found < 0)
            {
                spdlog::warn("PlaceName layer slot {} not readable - skipped", slot);
                continue;
            }
            dlc_valid += found;
        }
        if (dlc_valid)
            spdlog::info("PlaceName DLC layers contribute {} additional valid textIds", dlc_valid);
#endif
    }
    else
        spdlog::error("PlaceName FMG merge failed");

    // Inject NEW TutorialBody entries (slot 208 = 0xD0) for the codex toasts.
    // CSPopupMenu::ShowTutorialPopup (trampoline, AOB-resolved at runtime - its
    // RVA shifts on game updates) looks up TutorialParam[id].textId then
    // TutorialBody.fmg[textId]. Matching TutorialParam rows are injected by
    // goblin::inject_tutorial_popup_rows. Using fresh ids leaves all vanilla/ERR
    // codex text untouched. All four entries (ON/OFF/DUMP_OK/DUMP_FAIL) are
    // STATIC strings written below - there is no runtime text rewriting.
    if (count2 > 208 && sub[208])
    {
        // Allocate the 4 toast text ids ABOVE the live TutorialBody max (dynamic -
        // never collides with an overhaul's codex text). inject_tutorial_popup_rows
        // (runs after this) points its param rows' textId at these.
        int32_t tb_base = fmg_max_id(sub[208]) + 1;
        for (int s = 0; s < goblin::TOAST_COUNT; ++s)
            goblin::g_toast_fmg_id[s] = tb_base + s;
        const auto toast_lang = goblin::i18n::current_language();
        std::vector<NewEntry> tb_entries = {
            {goblin::g_toast_fmg_id[goblin::TOAST_ON],
             goblin::i18n::wtr(goblin::i18n::ToastId::MapIconsOn, toast_lang)},
            {goblin::g_toast_fmg_id[goblin::TOAST_OFF],
             goblin::i18n::wtr(goblin::i18n::ToastId::MapIconsOff, toast_lang)},
            {goblin::g_toast_fmg_id[goblin::TOAST_DUMP_OK],
             goblin::i18n::wtr(goblin::i18n::ToastId::MarkersDumped, toast_lang)},
            {goblin::g_toast_fmg_id[goblin::TOAST_DUMP_FAIL],
             goblin::i18n::wtr(goblin::i18n::ToastId::MarkerDumpFailed, toast_lang)},
        };
        if (patch_fmg_in_memory(sub[208], &sub[208], tb_entries))
            spdlog::info("[TOAST] TutorialBody.fmg expanded (ids {}/{}/{}/{} above max {})",
                         goblin::g_toast_fmg_id[goblin::TOAST_ON], goblin::g_toast_fmg_id[goblin::TOAST_OFF],
                         goblin::g_toast_fmg_id[goblin::TOAST_DUMP_OK], goblin::g_toast_fmg_id[goblin::TOAST_DUMP_FAIL],
                         tb_base - 1);
        else
            spdlog::warn("[TOAST] TutorialBody.fmg merge failed - codex banners unavailable");
    }
    else
        spdlog::warn("[TOAST] TutorialBody (slot 208) unavailable - codex banners unavailable");

    // Inject the native settings-menu tab label into GR_MenuText (menu.msgbnd,
    // bank "GRMT"; MsgRepository slots are indexed by global BND file id -> 200).
    // The native menu builds its tab category label via the game's MenuTextCtor(fmgId) against this
    // bank. Fresh id above the live max never collides with an overhaul's own menu text. Sanity-check
    // the slot by resolving id 110000 (the "System" tab label) before touching it.
    //
    // Keyed on the menu mode, NOT on debug_logging: this WRITES a rebuilt FMG into the game's
    // MsgRepository and publishes the ids the menu resolves through, so a log key must not decide it -
    // and with the log key off the menu silently fell back to unlocalized labels.
    if (goblin::config::native_menu_enabled())
    {
        constexpr int kMenuTextSlot = 200;
        if (count2 > kMenuTextSlot && sub[kMenuTextSlot] &&
            fmg_lookup_in(sub[kMenuTextSlot], 110000))
        {
            // Entry texts must outlive patch_fmg_in_memory's copy pass; static
            // backing keeps the converted row labels alive for the call.
            static std::vector<std::wstring> mt_texts;
            mt_texts.clear();
            mt_texts.push_back(L"MapForGoblins"); // tab label (brand, locale-invariant)
            size_t row_count = 0;
            const auto *rows = goblin::native_menu_rows(&row_count);
            const auto mt_lang = goblin::i18n::current_language();
            for (size_t i = 0; i < row_count; ++i)
            {
                // Row label = the localized ini entry label (same text the overlay
                // shows), utf8 -> utf16 for the FMG.
                const char *u8 = goblin::i18n::entry_label(rows[i].ini_key, mt_lang);
                std::wstring w;
                if (u8 && *u8)
                {
                    int wl = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
                    if (wl > 1)
                    {
                        w.resize(static_cast<size_t>(wl) - 1);
                        MultiByteToWideChar(CP_UTF8, 0, u8, -1, w.data(), wl);
                    }
                }
                if (w.empty())
                {
                    // No/failed translation: fall back to the raw ini key.
                    w.assign(rows[i].ini_key, rows[i].ini_key + strlen(rows[i].ini_key));
                }
                mt_texts.push_back(std::move(w));
            }
            // Two more entries after the row labels: the localized "On"/"Off" value
            // texts the native keybinding form shows in its value column.
            const size_t mt_value_first = mt_texts.size();
            for (goblin::i18n::TextId vid :
                 {goblin::i18n::TextId::ValueOn, goblin::i18n::TextId::ValueOff})
            {
                const char *u8 = goblin::i18n::tr(vid, mt_lang);
                std::wstring w;
                if (u8 && *u8)
                {
                    int wl = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
                    if (wl > 1)
                    {
                        w.resize(static_cast<size_t>(wl) - 1);
                        MultiByteToWideChar(CP_UTF8, 0, u8, -1, w.data(), wl);
                    }
                }
                if (w.empty())
                    w = (vid == goblin::i18n::TextId::ValueOn) ? L"On" : L"Off";
                mt_texts.push_back(std::move(w));
            }
            // Allocate above the max of ALL GR_MenuText layers (base 200, dlc01 368,
            // dlc02 468): the runtime lookup prefers DLC layers, so a fresh id that
            // collides with a DLC-layer id would resolve to the DLC text, not ours.
            // DLC slots can hold STALE pointers under some loaders (the "?PlaceName?"
            // incident class) - SEH-guard each layer read like the copy walks do.
            // IGNORE absurd group ids while scanning: a live ERR run showed a layer
            // carrying last_id = INT32_MAX (sentinel/garbage group), which made
            // max+1 OVERFLOW to a negative base and killed every injected label.
            constexpr int32_t kSaneIdCap = 1000000000;
            auto sane_max = [](uint8_t *fmg) -> int32_t
            {
                uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x0C);
                if (grp_cnt == 0 || grp_cnt > 0x100000)
                    return 0;
                auto *groups = reinterpret_cast<FmgGroup *>(fmg + 0x28);
                int32_t mx = 0;
                for (uint32_t g = 0; g < grp_cnt; g++)
                    if (groups[g].last_id > mx && groups[g].last_id < kSaneIdCap)
                        mx = groups[g].last_id;
                return mx;
            };
            int32_t mt_max = sane_max(sub[kMenuTextSlot]);
            for (int dlc_slot : {368, 468})
            {
                if (dlc_slot >= count2 || !sub[dlc_slot])
                    continue;
                std::function<int()> job = [&, dlc_slot]() -> int
                { return sane_max(sub[dlc_slot]); };
                int layer_max = seh_call(&seh_run_job_thunk, &job);
                if (layer_max > mt_max)
                    mt_max = layer_max;
            }
            int32_t mt_base = mt_max + 1;
            // The ignored ids may legitimately exist above the cap - verify the whole
            // candidate block is FREE in every layer (SEH-guarded), slide up if not.
            {
                const int32_t need = static_cast<int32_t>(mt_texts.size());
                auto block_occupied = [&](int32_t first) -> bool
                {
                    for (int32_t k = 0; k < need; ++k)
                    {
                        for (int slot : {kMenuTextSlot, 368, 468})
                        {
                            if (slot >= count2 || !sub[slot])
                                continue;
                            std::function<int()> job = [&, slot, first, k]() -> int
                            { return fmg_lookup_in(sub[slot], first + k) ? 1 : 0; };
                            if (seh_call(&seh_run_job_thunk, &job) == 1)
                                return true;
                        }
                    }
                    return false;
                };
                int guard = 0;
                while (block_occupied(mt_base) && guard++ < 64)
                    mt_base += need;
            }
            std::vector<NewEntry> mt_entries;
            for (size_t i = 0; i < mt_texts.size(); ++i)
                mt_entries.push_back({mt_base + static_cast<int32_t>(i), mt_texts[i].c_str()});
            if (patch_fmg_in_memory(sub[kMenuTextSlot], &sub[kMenuTextSlot], mt_entries))
            {
                goblin::g_menutext_tab_id = mt_base;
                goblin::g_menutext_row_ids.assign(row_count, 0);
                for (size_t i = 0; i < row_count; ++i)
                    goblin::g_menutext_row_ids[i] = mt_base + 1 + static_cast<int32_t>(i);
                goblin::g_menutext_on_id = mt_base + static_cast<int32_t>(mt_value_first);
                goblin::g_menutext_off_id = goblin::g_menutext_on_id + 1;
                spdlog::info("[nmenu] GR_MenuText.fmg expanded: tab label id {} + {} row labels "
                             "+ on/off {}/{} (above live max {})", mt_base, row_count,
                             goblin::g_menutext_on_id, goblin::g_menutext_off_id, mt_base - 1);
            }
            else
                spdlog::warn("[nmenu] GR_MenuText merge failed - native tab keeps the duplicate label");
        }
        else
            spdlog::warn("[nmenu] GR_MenuText slot {} unavailable / id 110000 missing - "
                         "native tab keeps the duplicate label", kMenuTextSlot);
    }

    // Point every injected marker textId at its FRESH remapped id (the strings
    // now live at fresh ids in PlaceName, allocated above the runtime max). Raw
    // PlaceName location ids and any key we did not copy pass through unchanged.
    // Done here (inject_map_entries runs before setup_messages, so the rows exist
    // and still hold their offset-encoded ids); the live-loot relabel paths below
    // route their own writes through remap_textid() too. g_lot_backed_rows keep
    // their ORIGINAL encoded baked_text1 (captured at inject) for classification.
    {
        int remapped = 0, blank_high = 0;
        for (uint8_t *rp : goblin::injected_row_ptrs())
        {
            auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(rp);
            int32_t *tids[8] = {&p->textId1, &p->textId2, &p->textId3, &p->textId4,
                                &p->textId5, &p->textId6, &p->textId7, &p->textId8};
            for (auto *t : tids)
            {
                if (*t <= 0) continue;
                int32_t nv = goblin::remap_textid(*t);
                if (nv != *t) { *t = nv; remapped++; }
                else if (*t >= 50000000) blank_high++;  // a high (encoded) id we did NOT copy -> would be blank
            }
        }
        spdlog::info("[FMG] remapped {} live marker textId slots to fresh ids ({} high ids left unmapped/blank-risk)",
                     remapped, blank_high);
    }

    // Final safety pass: strip any marker textId that didn't end up with a real
    // string in the expanded PlaceName FMG. The game's GetMessage returns null
    // for a missing id and some callers build a std::wstring from it → null
    // deref in game code on load transitions (grace teleport / character swap).
    // Root cause was e.g. a double-offset enemy-name id (npc id already +900M
    // tutorial-encoded, then +700M again → 1.6B, in no FMG). Clearing it to -1
    // makes that text line simply absent instead of crashing.
    goblin::sanitize_injected_textids();
}

void goblin::sanitize_injected_textids()
{
    if (g_placename_valid_ids.empty())
    {
        spdlog::warn("[SANITIZE] PlaceName valid-id set empty - skipping (FMG merge ran?)");
        return;
    }
    auto valid = [](int32_t id) {
        // Logic sentinels the DLL/engine special-case (camp/boss markers) - leave alone.
        if (id == 5000 || id == 5100 || id == 5300 || id == 8800) return true;
        return g_placename_valid_ids.count(id) != 0;
    };
    const auto &rows = goblin::injected_row_ptrs();
    int cleared = 0, scanned = 0;
    for (uint8_t *p : rows)
    {
        auto *row = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(p);
        int32_t *tids[8]  = {&row->textId1, &row->textId2, &row->textId3, &row->textId4,
                             &row->textId5, &row->textId6, &row->textId7, &row->textId8};
        unsigned int *fl[8] = {&row->textDisableFlagId1, &row->textDisableFlagId2,
                               &row->textDisableFlagId3, &row->textDisableFlagId4,
                               &row->textDisableFlagId5, &row->textDisableFlagId6,
                               &row->textDisableFlagId7, &row->textDisableFlagId8};
        ++scanned;
        for (int i = 0; i < 8; ++i)
        {
            int32_t id = *tids[i];
            if (id >= 0 && !valid(id))
            {
                *tids[i] = -1;
                *fl[i] = 0;
                ++cleared;
            }
        }
    }
    spdlog::info("[SANITIZE] Scanned {} added rows, cleared {} dangling textId(s) "
                 "(missing from PlaceName FMG → would null-deref on load)", scanned, cleared);
}

// set_fmg_injection_active(bool) and is_fmg_injection_active() stood here. They swapped the
// PlaceName MsgRepository slot between our expanded FMG and the vanilla one, so the injection could
// be toggled at runtime - and nothing ever called either of them. With no caller, the three statics
// they were built on (g_placename_slot_ptr, g_vanilla_placename_fmg and g_fmg_injection_active)
// were write-only: filled at install time and never consulted again.
//
// They looked live because goblin_inject exports a matching pair for the PARAM injection, and that
// one IS driven (menu_auto_toggle_loop). The FMG buffer is different: it is installed once and
// stays for the session, which is also why check_patched_slots() exists to notice when the engine
// replaces the slot under us.

// Look an id up in ONE FMG-v2 buffer (same layout fixup as patch_fmg_in_memory: the game turns
// the relative string-offset table into an absolute pointer at runtime, so both forms are handled).
static const wchar_t *fmg_lookup_in(uint8_t *fmg, int32_t id)
{
    if (!fmg || id <= 0)
        return nullptr;
    uint32_t group_cnt  = *reinterpret_cast<uint32_t *>(fmg + 0x0C);
    uint32_t string_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x10);
    uint64_t raw        = *reinterpret_cast<uint64_t *>(fmg + 0x18);
    // Sanity-bound the header before walking it. A stale/garbage FMG slot pointer
    // (e.g. a DLC PlaceName layer that isn't actually loaded on some builds) reads
    // an absurd group_cnt here and the loop below derefs far out of bounds - an
    // access violation that, inside setup_messages, aborts the whole step (FMG never
    // patched -> the game later crashes in its own FMG lookup). Matches the guard the
    // DLC-whitelist block already uses. Garbage -> treat as "not found".
    if (group_cnt == 0 || group_cnt > 0x100000 || string_cnt > 0x200000 || raw == 0)
        return nullptr;
    uint64_t off_rel = (raw > 0x1000000)
                           ? static_cast<uint64_t>(reinterpret_cast<uint8_t *>(raw) - fmg)
                           : raw;
    auto *groups  = reinterpret_cast<FmgGroup *>(fmg + 0x28);
    auto *offsets = reinterpret_cast<uint64_t *>(fmg + off_rel);
    for (uint32_t g = 0; g < group_cnt; ++g)
    {
        if (id >= groups[g].first_id && id <= groups[g].last_id)
        {
            int32_t si = groups[g].string_index + (id - groups[g].first_id);
            if (si < 0 || si >= static_cast<int32_t>(string_cnt))
                return nullptr;
            uint64_t so = offsets[si];
            const wchar_t *s = (so > 0x1000000)
                                   ? reinterpret_cast<const wchar_t *>(so)
                                   : reinterpret_cast<const wchar_t *>(fmg + so);
            return (s && *s) ? s : nullptr;
        }
    }
    return nullptr;
}

const wchar_t *goblin::lookup_text(int32_t id)
{
    return fmg_lookup_in(g_expanded_placename_fmg, id);
}

const wchar_t *goblin::lookup_text_dlc(int32_t id)
{
    // Raw (un-remapped) PlaceName id, probed in the DLC layers the game falls back
    // to. Used by the marker dump for DLC-layer-only locations that aren't carried
    // in our expanded base buffer.
    for (uint8_t *fmg : g_placename_dlc_slots)
        if (const wchar_t *s = fmg_lookup_in(fmg, id))
            return s;
    return nullptr;
}
