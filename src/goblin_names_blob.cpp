// Expanding the packed enemy names and item fallback. See goblin_names_blob.hpp for why they are
// packed at all.
//
// THE LAYOUT LIVES IN TWO PLACES and they have to agree: this file and tools/textblob.py (the
// 'MFGN' table). The version word is the guard.
//
//   magic      'MFGN'                      4
//   version    u16                         2
//   columns    u16                         2     ENEMY_NAME_LANG_COUNT for the enemy names, 1 for the fallback
//   count      u32                         4
//   count x id delta u32                         id[i] - id[i-1] (id[-1] = 0), modulo 2^32
//   columns x count strings                      column by column, each NUL-terminated UTF-8; in a
//                                                column after the first, "\x01" = column 0's text
//
// At startup, not on first use: the readers are setup_messages() (an init step) and
// lookup_text_english() (the search index, built from the menus), and setup_mod() runs this right
// after the map table - before either, before any hook is enabled and before any other thread of
// ours exists - so nothing can read a table while it is being filled.

#include "goblin_names_blob.hpp"

#include "miniz.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace goblin::generated
{

// Supplied by the generated translation units (src/generated*/goblin_enemy_names.cpp and
// goblin_item_fallback.cpp).
extern const unsigned char ENEMY_NAMES_BLOB[];
extern const unsigned int ENEMY_NAMES_BLOB_SIZE;
extern const unsigned int ENEMY_NAMES_BLOB_RAW_SIZE;
extern const unsigned char ITEM_FALLBACK_BLOB[];
extern const unsigned int ITEM_FALLBACK_BLOB_SIZE;
extern const unsigned int ITEM_FALLBACK_BLOB_RAW_SIZE;

const EnemyName *ENEMY_NAMES = nullptr;
size_t ENEMY_NAME_COUNT = 0;
const ItemNameFallback *ITEM_NAME_FALLBACK = nullptr;
size_t ITEM_NAME_FALLBACK_COUNT = 0;

namespace
{

constexpr uint16_t kBlobVersion = 1;

// The expanded tables own their storage: every name pointer points into one of the two text
// buffers, which are filled once and never resized. They are namespace-scope vectors, so they go
// at DLL unload with the other statics - the same lifetime the literals had in practice (the i18n
// tables are leaked on purpose instead: DllMain reads those, see goblin_i18n_blob.cpp).
std::vector<wchar_t> g_enemy_text, g_fallback_text;
std::vector<EnemyName> g_enemy_rows;
std::vector<ItemNameFallback> g_fallback_rows;
bool g_loaded = false;
bool g_ok = false;

// Inflate one table into `text` and split it: ids[i], and cells[i * columns + c] pointing into
// `text`. nullptr on success, otherwise what went wrong, for the log line.
const char *expand(const unsigned char *blob, unsigned size, unsigned raw_size, uint16_t columns,
                   std::vector<wchar_t> &text, std::vector<int32_t> &ids,
                   std::vector<const wchar_t *> &cells)
{
    if (size == 0 || raw_size < 12)
        return "is empty";
    std::vector<unsigned char> raw(raw_size);
    mz_ulong out_len = raw_size;
    if (mz_uncompress(raw.data(), &out_len, blob, size) != MZ_OK || out_len != raw_size)
        return "would not expand";
    if (std::memcmp(raw.data(), "MFGN", 4) != 0)
        return "has the wrong magic";
    uint16_t version = 0, have_columns = 0;
    uint32_t count = 0;
    std::memcpy(&version, raw.data() + 4, 2);
    std::memcpy(&have_columns, raw.data() + 6, 2);
    std::memcpy(&count, raw.data() + 8, 4);
    if (version != kBlobVersion)
        return "is another version";
    if (have_columns != columns)
        return "has another language count";
    const size_t ids_end = 12 + static_cast<size_t>(count) * 4;
    if (ids_end > raw.size())
        return "ends inside its ids";

    ids.resize(count);
    uint32_t id = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t delta = 0;
        std::memcpy(&delta, raw.data() + 12 + static_cast<size_t>(i) * 4, 4);
        id += delta;
        ids[i] = static_cast<int32_t>(id);
    }

    // One conversion for the whole string block: its NULs come through as L'\0', so the result is
    // every string widened and still NUL-separated, in the same order.
    text.clear();
    const char *utf8 = reinterpret_cast<const char *>(raw.data() + ids_end);
    const int utf8_len = static_cast<int>(raw.size() - ids_end);
    if (utf8_len > 0)
    {
        const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, utf8_len, nullptr, 0);
        if (wide_len <= 0)
            return "holds text that is not UTF-8";
        text.resize(static_cast<size_t>(wide_len));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, utf8_len, text.data(), wide_len) != wide_len)
            return "holds text that is not UTF-8";
    }

    cells.assign(static_cast<size_t>(count) * columns, nullptr);
    const wchar_t *p = text.data();
    const wchar_t *const end = p + text.size();
    for (uint16_t c = 0; c < columns; ++c)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const wchar_t *s = p;
            while (p < end && *p)
                ++p;
            if (p == end)
                return "ends inside its text";
            ++p;
            // "\x01" after the first column: the same text as column 0, an untranslated name.
            const size_t row = static_cast<size_t>(i) * columns;
            cells[row + c] = (c != 0 && s[0] == 1 && s[1] == 0) ? cells[row] : s;
        }
    }
    if (p != end)
        return "has text past its last row";
    return nullptr;
}

} // namespace

bool load_name_tables()
{
    if (g_loaded)
        return g_ok;
    g_loaded = true;

    std::vector<int32_t> ids;
    std::vector<const wchar_t *> cells;
    bool ok = true;

    constexpr uint16_t kLangs = static_cast<uint16_t>(ENEMY_NAME_LANG_COUNT);
    if (const char *why = expand(ENEMY_NAMES_BLOB, ENEMY_NAMES_BLOB_SIZE, ENEMY_NAMES_BLOB_RAW_SIZE,
                                 kLangs, g_enemy_text, ids, cells))
    {
        spdlog::error("[names] the enemy-name table {}; enemy markers keep their generic labels", why);
        g_enemy_text.clear();
        ok = false;
    }
    else
    {
        g_enemy_rows.resize(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            g_enemy_rows[i].id = ids[i];
            for (uint16_t c = 0; c < kLangs; ++c)
                g_enemy_rows[i].names[c] = cells[i * kLangs + c];
        }
        ENEMY_NAMES = g_enemy_rows.empty() ? nullptr : g_enemy_rows.data();
        ENEMY_NAME_COUNT = g_enemy_rows.size();
    }

    if (const char *why = expand(ITEM_FALLBACK_BLOB, ITEM_FALLBACK_BLOB_SIZE, ITEM_FALLBACK_BLOB_RAW_SIZE,
                                 1, g_fallback_text, ids, cells))
    {
        spdlog::error("[names] the item-name fallback {}; no English fallback names", why);
        g_fallback_text.clear();
        ok = false;
    }
    else
    {
        g_fallback_rows.resize(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
            g_fallback_rows[i] = ItemNameFallback{ids[i], cells[i]};
        ITEM_NAME_FALLBACK = g_fallback_rows.empty() ? nullptr : g_fallback_rows.data();
        ITEM_NAME_FALLBACK_COUNT = g_fallback_rows.size();
    }

    if (ok) // a table that failed has said so above
        spdlog::info("[names] {} enemy names, {} fallback names expanded from {} KB packed",
                     ENEMY_NAME_COUNT, ITEM_NAME_FALLBACK_COUNT,
                     (ENEMY_NAMES_BLOB_SIZE + ITEM_FALLBACK_BLOB_SIZE) / 1024);
    g_ok = ok;
    return ok;
}

} // namespace goblin::generated
