// Expanding the packed i18n bundles - the bundle() that goblin_i18n.cpp reads every string through.
//
// The overlay, native-menu and ini strings used to be compiled in as literals (about 150 KB of
// readable text in every DLL). src/generated_shared/goblin_i18n_strings.cpp now holds them packed
// and deflated, the way the map table ships (goblin_map_blob.hpp), inflated with the same miniz.
//
// THE LAYOUT LIVES IN TWO PLACES and they have to agree: this file and tools/textblob.py (the
// 'MFGI' table). The version word is the guard.
//
//   magic      'MFGI'                      4
//   version    u16                         2
//   locales    u16                         2
//   locales x:
//     language u8                                the Language value it answers for; locale 0 also
//                                                answers for every value without one of its own
//     6 x table, in LocaleBundle order (texts, toasts, sect_labels, sect_comments, entry_labels,
//     entry_comments):
//       count  u32                               0 = no table, nullptr in the bundle
//       texts:   count x id u16, then count x NUL-terminated UTF-8
//       toasts:  count x id u16, then count x UTF-16LE up to a 0 unit
//       names:   count x key, then count x value, each NUL-terminated UTF-8
//
// ON FIRST USE, not as a setup_mod step, because the first reader comes before setup_mod exists:
// load_config() runs in DllMain and rewrites the ini with localized comments and tr(IniHeader), and
// mfg_inigen - the build-time ini tool, which links this file - has no setup of any kind. After
// that the overlay reads on the game's render thread and the native menu on its own. bundle() keeps
// the tables in a function-local static, which the C++ runtime initialises exactly once and makes
// any other thread wait for, so whichever reader is first expands them and the rest only read.

#include "goblin_i18n_bundle.hpp"

#include "miniz.h"

#ifdef MFG_INIGEN
#include <cstdio>
#else
#include <spdlog/spdlog.h>
#endif

#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace goblin::i18n::detail
{

// Supplied by the generated translation unit (src/generated_shared/goblin_i18n_strings.cpp).
extern const unsigned char I18N_BLOB[];
extern const unsigned int I18N_BLOB_SIZE;
extern const unsigned int I18N_BLOB_RAW_SIZE;

namespace
{

constexpr uint16_t kBlobVersion = 1;
constexpr int kTables = 6;

// Everything the bundles point at. Never freed: a string handed out stays valid to the end of the
// process, as the literals did, whatever still runs while statics are destroyed.
struct Tables
{
    std::vector<unsigned char> raw; // the inflated blob; every narrow string points into it
    std::vector<wchar_t> wide;      // the toasts, widened
    std::vector<TextKV> texts;
    std::vector<ToastKV> toasts;
    std::vector<NameKV> names;
    std::vector<LocaleBundle> bundles; // [0] is the fallback
    uint16_t locale_of[256] = {};      // Language value -> index into bundles
};

// What every reader gets if the table would not expand: no rows anywhere, so tr() answers "",
// labels their key and comments the caller's fallback - the same as for any missing string.
const LocaleBundle kEmpty{};

struct Reader
{
    const unsigned char *p;
    const unsigned char *end;
    bool ok = true;

    template <typename T> T take()
    {
        T v{};
        if (static_cast<size_t>(end - p) < sizeof(T))
        {
            ok = false;
            p = end;
            return v;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }

    // A NUL-terminated string, pointed at where it lies in the blob.
    const char *narrow()
    {
        const void *nul = std::memchr(p, 0, static_cast<size_t>(end - p));
        if (!nul)
        {
            ok = false;
            p = end;
            return nullptr;
        }
        const char *s = reinterpret_cast<const char *>(p);
        p = static_cast<const unsigned char *>(nul) + 1;
        return s;
    }
};

// Inflate and walk the blob into `t`. nullptr on success, otherwise what went wrong, for the log.
const char *fill(Tables &t)
{
    if (I18N_BLOB_SIZE == 0 || I18N_BLOB_RAW_SIZE < 8)
        return "is empty";
    t.raw.resize(I18N_BLOB_RAW_SIZE);
    mz_ulong out_len = I18N_BLOB_RAW_SIZE;
    if (mz_uncompress(t.raw.data(), &out_len, I18N_BLOB, I18N_BLOB_SIZE) != MZ_OK ||
        out_len != I18N_BLOB_RAW_SIZE)
        return "would not expand";

    Reader r{t.raw.data(), t.raw.data() + t.raw.size()};
    if (std::memcmp(r.p, "MFGI", 4) != 0)
        return "has the wrong magic";
    r.p += 4;
    if (r.take<uint16_t>() != kBlobVersion)
        return "is another version";
    const uint16_t locale_count = r.take<uint16_t>();
    if (locale_count == 0)
        return "has no locales";

    // Where each locale's tables start in the row vectors. Indices, not pointers: the vectors are
    // still growing while this walks, so the bundles get their pointers once every row is in.
    struct Span
    {
        uint8_t language;
        size_t first[kTables];
        uint32_t count[kTables];
    };
    std::vector<Span> spans(locale_count);
    std::vector<size_t> toast_at; // where each toast starts in t.wide
    for (Span &s : spans)
    {
        s.language = r.take<uint8_t>();
        for (int k = 0; k < kTables && r.ok; ++k)
        {
            const uint32_t n = r.take<uint32_t>();
            s.count[k] = n;
            s.first[k] = k == 0 ? t.texts.size() : k == 1 ? t.toasts.size() : t.names.size();
            // The ids (or keys) of the whole table first, then its strings in the same order.
            for (uint32_t i = 0; i < n && r.ok; ++i)
            {
                if (k == 0)
                    t.texts.push_back(TextKV{static_cast<TextId>(r.take<uint16_t>()), nullptr});
                else if (k == 1)
                    t.toasts.push_back(ToastKV{static_cast<ToastId>(r.take<uint16_t>()), nullptr});
                else
                    t.names.push_back(NameKV{r.narrow(), nullptr});
            }
            for (uint32_t i = 0; i < n && r.ok; ++i)
            {
                if (k == 0)
                    t.texts[s.first[k] + i].s = r.narrow();
                else if (k == 1)
                {
                    toast_at.push_back(t.wide.size());
                    for (uint16_t u = 1; u != 0 && r.ok;)
                    {
                        u = r.take<uint16_t>();
                        t.wide.push_back(static_cast<wchar_t>(u));
                    }
                }
                else
                    t.names[s.first[k] + i].val = r.narrow();
            }
        }
        if (!r.ok)
            return "ends early";
    }
    if (r.p != r.end)
        return "has bytes past its last locale";

    for (size_t i = 0; i < t.toasts.size(); ++i)
        t.toasts[i].s = t.wide.data() + toast_at[i];
    t.bundles.reserve(spans.size());
    for (size_t li = 0; li < spans.size(); ++li)
    {
        const Span &s = spans[li];
        LocaleBundle b{};
        b.texts = s.count[0] ? &t.texts[s.first[0]] : nullptr;
        b.n_texts = s.count[0];
        b.toasts = s.count[1] ? &t.toasts[s.first[1]] : nullptr;
        b.n_toasts = s.count[1];
        b.sect_labels = s.count[2] ? &t.names[s.first[2]] : nullptr;
        b.n_sect_labels = s.count[2];
        b.sect_comments = s.count[3] ? &t.names[s.first[3]] : nullptr;
        b.n_sect_comments = s.count[3];
        b.entry_labels = s.count[4] ? &t.names[s.first[4]] : nullptr;
        b.n_entry_labels = s.count[4];
        b.entry_comments = s.count[5] ? &t.names[s.first[5]] : nullptr;
        b.n_entry_comments = s.count[5];
        t.bundles.push_back(b);
        t.locale_of[s.language] = static_cast<uint16_t>(li);
    }
    return nullptr;
}

const Tables *expand()
{
    const char *why = nullptr;
    Tables *t = new (std::nothrow) Tables();
    if (!t)
        why = "could not be allocated";
    else
    {
        try
        {
            why = fill(*t);
        }
        catch (...)
        {
            why = "ran out of memory";
        }
    }
    if (why)
    {
        delete t;
        t = nullptr;
    }
#ifdef MFG_INIGEN
    if (why)
        std::fprintf(stderr, "mfg_inigen: the packed i18n strings %s\n", why);
#else
    // No logger exists until DllMain's setup_logger() has run. Nothing reads text before that (no
    // static initialiser does), but a read that ever did must not fault on the missing logger.
    if (auto *log = spdlog::default_logger_raw())
    {
        if (why)
            log->error("[i18n] the packed text table {}; menus fall back to keys", why);
        else
            log->info("[i18n] {} locales expanded from {} KB packed", t->bundles.size(),
                      I18N_BLOB_SIZE / 1024);
    }
#endif
    return t;
}

} // namespace

const LocaleBundle &bundle(Language language)
{
    static const Tables *const tables = expand();
    if (!tables)
        return kEmpty;
    const auto v = static_cast<unsigned>(language);
    return tables->bundles[v < 256 ? tables->locale_of[v] : 0];
}

} // namespace goblin::i18n::detail
