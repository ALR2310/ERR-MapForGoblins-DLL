#include "goblin_search.hpp"

#include "goblin_config.hpp"
#include "goblin_i18n.hpp"
#include "goblin_inject.hpp"
#include "goblin_map_data.hpp"
#include "goblin_markers.hpp"
#include "goblin_messages.hpp"
#include "goblin_progress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <set>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace
{
    struct Entry
    {
        uint64_t key;
        int32_t textId;
        int32_t region;
        uint8_t cat;
        std::wstring loc_lower;  // name in the player's language (or the English fallback)
        std::wstring en_lower;   // English name from the baked table; empty when unknown/same
        std::string display;     // UTF-8, what the row shows
    };

    std::mutex g_mtx;
    std::vector<Entry> g_index;
    uint32_t g_index_epoch = 0;
    goblin::i18n::Language g_index_lang = goblin::i18n::Language::English;
    // Per-category searchable text, indexed by Category: localized label + English name.
    std::vector<std::wstring> g_cat_lower;

    std::string g_last_query;
    bool g_last_only_uncollected = false;
    uint64_t g_last_run_ms = 0; // with the filter on, results age: collected markers drop out
    std::shared_ptr<const goblin::search::Results> g_results; // null = nothing cached

    std::set<uint64_t> g_picks;
    // apply_picks() is in flight: the pick set is ahead of the map for a moment, and the
    // consistency sweep in query() must not read that as "the filter was lifted elsewhere".
    std::atomic<bool> g_applying{false};

    std::wstring to_lower(const wchar_t *w)
    {
        if (!w || !*w) return {};
        std::wstring s(w);
        // CharLowerBuffW folds every Unicode letter, not just ASCII - MSVC towlower in the
        // default C locale leaves Cyrillic and accented Latin alone.
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
        return s;
    }

    std::wstring utf8_to_wide(std::string_view s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::string wide_to_utf8(const std::wstring &w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    void build_index()
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto lang = goblin::i18n::current_language();
        const uint32_t epoch = goblin::label_epoch();

        g_cat_lower.assign(static_cast<size_t>(goblin::progress::kCategoryCount), std::wstring());
        for (int c = 0; c < goblin::progress::kCategoryCount; ++c)
        {
            const auto cat = static_cast<goblin::generated::Category>(c);
            std::wstring w;
            if (const char *key = goblin::category_config_key(cat))
                w = utf8_to_wide(goblin::i18n::entry_label(key, lang));
            const char *en = goblin::markers::category_name(cat);
            if (en && *en)
            {
                if (!w.empty()) w += L'\n';
                w += utf8_to_wide(en);
            }
            CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
            g_cat_lower[static_cast<size_t>(c)] = std::move(w);
        }

        std::vector<Entry> idx;
        const auto rows = goblin::search_row_snapshot();
        idx.reserve(rows.size());
        size_t no_name = 0, with_en = 0;
        for (const auto &r : rows)
        {
            Entry e{};
            e.key = r.key;
            e.textId = r.textId;
            e.region = r.region;
            e.cat = r.cat;
            const wchar_t *loc = r.textId > 0 ? goblin::lookup_text_any(r.textId) : nullptr;
            const wchar_t *en = r.textId > 0 ? goblin::lookup_text_english(goblin::unremap_textid(r.textId)) : nullptr;
            if (loc && *loc)
            {
                e.loc_lower = to_lower(loc);
                e.display = wide_to_utf8(loc);
                if (en && *en && wcscmp(loc, en) != 0)
                {
                    e.en_lower = to_lower(en);
                    e.display += " (";
                    e.display += wide_to_utf8(en);
                    e.display += ")";
                    ++with_en;
                }
            }
            else if (en && *en)
            {
                e.loc_lower = to_lower(en);
                e.display = wide_to_utf8(en);
            }
            else
            {
                // Textless marker (a grace, a stake, a summoning pool...): searchable and shown
                // by its category label only.
                ++no_name;
                const auto cat = static_cast<goblin::generated::Category>(r.cat);
                const char *key = goblin::category_config_key(cat);
                e.display = key ? goblin::i18n::entry_label(key, lang) : goblin::markers::category_name(cat);
            }
            idx.push_back(std::move(e));
        }
        g_index = std::move(idx);
        g_index_epoch = epoch;
        g_index_lang = lang;
        g_results.reset();  // any cached result set was built from the old index
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        spdlog::info("[search] index built: {} markers ({} with a distinct English name, {} textless), lang={}, epoch={}, {:.1f} ms",
                     g_index.size(), with_en, no_name, goblin::i18n::language_code(lang), epoch, ms);
    }

    bool token_in(const std::wstring &hay, const std::wstring &needle)
    {
        return !hay.empty() && hay.find(needle) != std::wstring::npos;
    }

    void run_query(const std::string &q)
    {
        // A NEW object per query, published at the end: readers on the other thread keep the
        // snapshot they already hold.
        auto out = std::make_shared<goblin::search::Results>();
        goblin::search::Results &res = *out;
        g_results = out;
        // Tokens: whitespace-separated, lowered. Every token must occur in one of the
        // three searchable strings (localized name / English name / category label).
        std::wstring wq = utf8_to_wide(q);
        CharLowerBuffW(wq.data(), static_cast<DWORD>(wq.size()));
        std::vector<std::wstring> tokens;
        {
            size_t i = 0;
            while (i < wq.size())
            {
                while (i < wq.size() && iswspace(wq[i])) ++i;
                size_t j = i;
                while (j < wq.size() && !iswspace(wq[j])) ++j;
                if (j > i) tokens.push_back(wq.substr(i, j - i));
                i = j;
            }
        }
        if (tokens.empty()) return;

        // search_hide_collected: drop the markers already collected / hidden (the same set the
        // Progress tab counts as done). Read once per query, not per row.
        const bool only_uncollected = goblin::config::searchHideCollected;
        const std::unordered_set<uint64_t> done =
            only_uncollected ? goblin::hidden_marker_original_ids() : std::unordered_set<uint64_t>{};
        std::unordered_map<int32_t, size_t> group_of;
        size_t total = 0;
        for (const auto &e : g_index)
        {
            if (only_uncollected && done.count(e.key)) continue;
            const std::wstring &cat = g_cat_lower[e.cat < g_cat_lower.size() ? e.cat : 0];
            bool ok = true;
            for (const auto &t : tokens)
                if (!token_in(e.loc_lower, t) && !token_in(e.en_lower, t) && !token_in(cat, t))
                { ok = false; break; }
            if (!ok) continue;
            if (total >= goblin::search::kMaxHits) { res.truncated = true; break; }
            ++total;
            auto it = group_of.find(e.region);
            if (it == group_of.end())
            {
                goblin::search::Group g;
                g.region = e.region;
                if (!goblin::progress::region_name(e.region, g.region_name))
                {
                    const wchar_t *w = e.region > 0 ? goblin::lookup_text_any(e.region) : nullptr;
                    g.region_name = (w && *w) ? wide_to_utf8(w) : "?";
                }
                it = group_of.emplace(e.region, res.groups.size()).first;
                res.groups.push_back(std::move(g));
            }
            res.groups[it->second].hits.push_back(
                goblin::search::Hit{e.key, e.textId, e.region, e.cat, e.display});
        }
        res.total = total;
        std::sort(res.groups.begin(), res.groups.end(),
                  [](const goblin::search::Group &a, const goblin::search::Group &b) {
                      const bool ao = a.region < 0, bo = b.region < 0;
                      if (ao != bo) return !ao;  // "Other" last
                      return a.region_name < b.region_name;
                  });
        for (auto &g : res.groups)
            std::sort(g.hits.begin(), g.hits.end(),
                      [](const goblin::search::Hit &a, const goblin::search::Hit &b) {
                          const int c = a.name.compare(b.name);
                          return c != 0 ? c < 0 : a.key < b.key;
                      });
    }
}

std::shared_ptr<const goblin::search::Results> goblin::search::query(std::string_view utf8)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const bool stale = g_index.empty() || g_index_epoch != goblin::label_epoch() ||
                       g_index_lang != goblin::i18n::current_language();
    if (stale) build_index();
    // Picks exist iff the pick focus is on the map. The filter can be lifted elsewhere (the
    // Progress "reset" row, a category focus replacing it): then the picks are stale and go.
    if (!g_picks.empty() && !goblin::focus_rows_active() && !g_applying.load())
        g_picks.clear();
    const bool only_uncollected = goblin::config::searchHideCollected;
    const uint64_t now = GetTickCount64();
    // Re-run on a new query, a flipped filter, or (filter on) every 2 s so a marker collected
    // since then leaves the list.
    if (!g_results || g_last_query != utf8 || g_last_only_uncollected != only_uncollected ||
        (only_uncollected && now - g_last_run_ms > 2000))
    {
        g_last_query.assign(utf8.data(), utf8.size());
        g_last_only_uncollected = only_uncollected;
        g_last_run_ms = now;
        run_query(g_last_query);
    }
    return g_results;
}

bool goblin::search::is_picked(uint64_t key)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_picks.count(key) != 0;
}

namespace
{
    // Push the pick set to the map. Outside the module lock: the focus machinery is inject's.
    void apply_picks()
    {
        std::vector<uint64_t> p;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            p.assign(g_picks.begin(), g_picks.end());
            g_applying.store(true);
        }
        struct Done { ~Done() { g_applying.store(false); } } done;
        if (p.empty())
        {
            if (!goblin::focus_rows_active()) return; // nothing of ours to lift
            goblin::set_focus_category(-1);
        }
        else
            goblin::set_focus_rows(p);
        goblin::reapply_live_settings();
        goblin::apply_focus_highlight();
    }
}

void goblin::search::set_picked(uint64_t key, bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (on) g_picks.insert(key); else g_picks.erase(key);
    }
    apply_picks();
}

void goblin::search::pick_many(const std::vector<uint64_t> &keys, bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (uint64_t k : keys)
            if (on) g_picks.insert(k); else g_picks.erase(k);
    }
    apply_picks();
}

void goblin::search::clear_picks()
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_picks.clear();
    }
    apply_picks();
}

size_t goblin::search::pick_count()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_picks.size();
}

std::vector<uint64_t> goblin::search::picks()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return std::vector<uint64_t>(g_picks.begin(), g_picks.end());
}

size_t goblin::search::index_size()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_index.size();
}
