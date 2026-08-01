#include "goblin_native_menu.hpp"

#include "goblin_own_movie.hpp"

#include "goblin_config.hpp"
#include "goblin_config_schema.hpp"
#include "goblin_i18n.hpp"
#include "goblin_inject.hpp"
#include "goblin_menu_addons.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_sfimage.hpp"
#include "goblin_markers.hpp"
#include "goblin_messages.hpp" // lookup_text() names the hidden markers
#include "goblin_progress.hpp"
#include "generated_shared/goblin_overlay_icons.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <windows.h>

namespace
{
    namespace tr = goblin::i18n;
    using goblin::nmenu::BarStyle;
    using goblin::nmenu::Row;
    using goblin::nmenu::RowKind;

    // ── model state ──────────────────────────────────────────────────────────────────
    std::vector<Row> g_rows;
    std::deque<std::wstring> g_arena; // stable backing for every label/value pointer
    std::wstring g_title;
    int32_t g_page = goblin::nmenu::kPageRoot;
    std::vector<int32_t> g_stack; // pages we came from, for Back
    // Which page is being PREVIEWED in the right-hand column (-1 = none). Confirming a
    // sub-page row previews it here first; confirming the same row again enters it. The form
    // is 11 rows x 2 columns and the right clips only carry a value field, so a preview row
    // is one line of text.
    int32_t g_preview_page = -1;
    std::vector<Row> g_preview_rows;
    std::deque<std::wstring> g_preview_arena;
    bool g_dirty = false;
    BarStyle g_bar_style = BarStyle::Ascii;

    // The entry a value page / rebind page is editing. Pointers into the ini schema, which
    // outlives every page, so holding them across a rebuild is safe.
    const goblin::IniEntry *g_edit_entry = nullptr;

    // ── screen-per-page mode ─────────────────────────────────────────────────────────
    // On: every page is a real native screen owned by the host, so the model must not move
    // g_page by itself - it only reports what the host should open or close.
    bool g_nested = false;
    int32_t g_child_page = -1; // page to open as a child screen
    bool g_want_close = false; // this screen is finished

    const wchar_t *hold(std::wstring s)
    {
        g_arena.push_back(std::move(s));
        return g_arena.back().c_str();
    }

    std::wstring wide(const char *u8)
    {
        std::wstring w;
        if (!u8 || !*u8)
            return w;
        const int wl = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
        if (wl > 1)
        {
            w.resize(static_cast<size_t>(wl) - 1);
            MultiByteToWideChar(CP_UTF8, 0, u8, -1, w.data(), wl);
        }
        return w;
    }

    const wchar_t *text(tr::TextId id) { return hold(wide(tr::tr(id))); }

    // ── numeric row metadata ─────────────────────────────────────────────────────────
    // The ini schema stores defaults as text and has no range info, so ranges for the
    // rows a player can step live here, keyed by ini key. Anything not listed is shown
    // read-only (the value is still visible, it just cannot be changed on this screen).
    struct NumRange
    {
        const char *key;
        float min;
        float max;
        float step;
    };
    // Ranges mirror the overlay's own sliders (goblin_overlay.cpp): font scale 0.8-3.0
    // and opacity 0.3-1.0. The remaining Float entries are the overlay WINDOW geometry
    // (overlay_window_x/y/w/h) - meaningless to edit from the in-game menu, so they stay
    // read-only Info rows.
    constexpr NumRange kRanges[] = {
        {"overlay_font_scale", 0.80f, 3.00f, 0.10f},
        {"overlay_opacity", 0.30f, 1.00f, 0.05f},
    };
    const NumRange *range_for(const char *key)
    {
        if (!key)
            return nullptr;
        for (const auto &r : kRanges)
            if (std::strcmp(r.key, key) == 0)
                return &r;
        return nullptr;
    }

    // The two ini values with a fixed option list (both mirror the overlay's combos).
    const char *const kLanguages[] = {"auto",    "english", "schinese", "tchinese", "korean",
                                      "russian", "german",  "french",   "spanish"};
    const char *const kRenderModes[] = {"auto", "window", "swapchain_2"};

    bool is_render_mode_key(const char *key)
    {
        return key && std::strcmp(key, "overlay_render_mode") == 0;
    }

    // ── markup ───────────────────────────────────────────────────────────────────────
    // The row's text fields are html=1 EditText, and the setter the mod already uses
    // (RVA 0x74A000 -> FUN_140D842A0) passes isHtml=1 unconditionally - so <font>, <b>
    // and friends work with no extra plumbing (recon_draw_primitives.md section 2).
    // Kept behind a switch so a single in-game run can compare markup on vs off.
    bool g_markup = true;
    bool g_graphic_bar = false;

    std::wstring colored(const std::wstring &text, uint32_t rgb)
    {
        if (!g_markup)
            return text;
        wchar_t open[32];
        _snwprintf_s(open, _TRUNCATE, L"<font color=\"#%06X\">", rgb & 0xFFFFFF);
        return std::wstring(open) + text + L"</font>";
    }

    constexpr uint32_t kColOn = 0x7FD97F;    // green - enabled
    constexpr uint32_t kColOff = 0x9A9A9A;   // grey - disabled
    constexpr uint32_t kColValue = 0xE8D9A0; // parchment - neutral value
    constexpr uint32_t kColBarOn = 0x7FD97F;
    constexpr uint32_t kColBarOff = 0x50504A;
    constexpr uint32_t kColFocus = 0xE86A6A;  // the category isolated on the map right now
    constexpr uint32_t kColHeader = 0x9ED1FF; // mega-section captions, as in the overlay

    // ── per-section marker glyph ─────────────────────────────────────────────────────
    // Real per-category bitmaps need an image clip the 02_160 row does not have (icons_*
    // recon is running). Until that lands, each section gets a coloured glyph so
    // categories stay distinguishable. Only code points VERIFIED present in the menu font
    // (font/eu_std/font.gfx, "Agmena W1G", 910 glyphs) are used: U+25A0 SQUARE,
    // U+25CF CIRCLE, U+25C6 DIAMOND, U+2605 STAR, U+25B2 TRIANGLE, U+2022 BULLET.
    struct SectionMark
    {
        const char *section;
        wchar_t glyph;
        uint32_t rgb;
    };
    // In-game truth (screenshots 2026-07-25): this client's menu font draws U+2022
    // BULLET but NOT the geometric shapes (U+25A0/25C6/25CF/25B2/2605 all rendered as
    // tofu boxes). So every section uses the bullet and is distinguished by COLOUR.
    constexpr SectionMark kMarks[] = {
        {"Goblin", L'\x2022', 0xE8D9A0},    {"Equipment", L'\x2022', 0x9FC6E8},
        {"Key Items", L'\x2022', 0xE8C86A}, {"Loot", L'\x2022', 0xC8E89A},
        {"Magic", L'\x2022', 0xC9A0E8},     {"Quest", L'\x2022', 0xE8A0A0},
        {"Reforged", L'\x2022', 0xE8B080},  {"World", L'\x2022', 0x9AD8C0},
        {"ERR Markers", L'\x2022', 0xD0A0E8},
    };
    // ini key -> source icon id, via the same atlas mapping the overlay draws from
    // (generated_shared/goblin_overlay_icons: ICON_CELLS gives the cell, CELL_SRC_ICON the
    // icon id). -1 when the key has no icon.
    int32_t icon_for_key(const char *key)
    {
        if (!key)
            return -1;
        namespace oi = goblin::overlay_icons;
        for (int i = 0; i < oi::ICON_CELL_COUNT; ++i)
        {
            if (std::strcmp(oi::ICON_CELLS[i].key, key) != 0)
                continue;
            const int cell = oi::ICON_CELLS[i].row * (oi::ATLAS_W / oi::CELL) + oi::ICON_CELLS[i].col;
            if (cell >= 0 && cell < oi::ATLAS_CELL_COUNT)
                return oi::CELL_SRC_ICON[cell];
            return -1;
        }
        return -1;
    }

    // A representative icon for a whole section: the first entry in it that has one. Makes
    // the icons visible on the very first screen instead of only inside a category page.
    const goblin::IniEntry *section_icon_entry(const goblin::IniSection &sec)
    {
        for (const auto &e : sec.entries)
            if (icon_for_key(e.key) >= 0)
                return &e;
        return nullptr;
    }

    // The glyph is a STAND-IN for a picture. Where a real icon draws, showing both put a
    // coloured dot right next to the image that replaced it; where there is no icon, the row
    // still needs its marker. So the caller passes whether this row has one.
    bool row_icons_drawn() { return goblin::config::nativeMenuIcons != 0; }

    const SectionMark *mark_for(const char *section)
    {
        if (!section)
            return nullptr;
        for (const auto &m : kMarks)
            if (std::strcmp(m.section, section) == 0)
                return &m;
        return nullptr;
    }

    // ── value formatting ─────────────────────────────────────────────────────────────
    std::wstring bar_text(int collected, int total, BarStyle style)
    {
        // The value field is only ~260px wide, so the NUMBERS come first - in-game the
        // trailing count was being clipped off the right edge. The bar itself is plain
        // ASCII: this client's font has no block or shade glyphs (they drew as tofu).
        const int width = 8;
        int filled = 0;
        if (total > 0)
            filled = std::clamp(static_cast<int>((static_cast<double>(collected) / total) * width +
                                                 0.5),
                                0, width);
        wchar_t head[32];
        _snwprintf_s(head, _TRUNCATE, L"%d/%d ", collected, total);
        std::wstring out(head);
        if (g_markup)
        {
            out += colored(std::wstring(static_cast<size_t>(filled), L'#'), kColBarOn);
            out += colored(std::wstring(static_cast<size_t>(width - filled), L'-'), kColBarOff);
        }
        else
        {
            out += L'[';
            out.append(static_cast<size_t>(filled), L'#');
            out.append(static_cast<size_t>(width - filled), L'-');
            out += L']';
        }
        (void)style;
        return out;
    }

    std::wstring value_of(const goblin::IniEntry &e)
    {
        wchar_t buf[96] = {};
        switch (e.type)
        {
        case goblin::IniType::Bool:
        {
            const bool on = e.target && *static_cast<bool *>(e.target);
            return colored(wide(tr::tr(on ? tr::TextId::ValueOn : tr::TextId::ValueOff)),
                           on ? kColOn : kColOff);
        }
        case goblin::IniType::Float:
            if (e.target)
                _snwprintf_s(buf, _TRUNCATE, L"%.2f", *static_cast<float *>(e.target));
            break;
        case goblin::IniType::U8:
            if (e.target)
                _snwprintf_s(buf, _TRUNCATE, L"%u",
                             static_cast<unsigned>(*static_cast<uint8_t *>(e.target)));
            break;
        case goblin::IniType::VkKey:
            // The same spelling the ini uses ("F10", "Home", ...) rather than a raw code -
            // config.cpp already owns that formatting for saving, so reuse it.
            if (e.target)
                return wide(goblin::format_vk_code(*static_cast<uint32_t *>(e.target)).c_str());
            break;
        case goblin::IniType::GamepadMask:
            if (e.target)
                return wide(goblin::format_gamepad_combo(*static_cast<uint16_t *>(e.target))
                                .c_str());
            break;
        case goblin::IniType::Language:
            if (e.target)
            {
                const auto *s = static_cast<std::string *>(e.target);
                return wide(tr::language_option_label(*s));
            }
            break;
        case goblin::IniType::Text:
            if (e.target)
                return wide(static_cast<std::string *>(e.target)->c_str());
            break;
        }
        return buf;
    }

    // The schema entry behind a row. Rows carry only the key (they are rebuilt constantly),
    // and the schema is a stable global, so this is the safe way back to the definition.
    const goblin::IniEntry *entry_for_key(const char *key)
    {
        if (!key)
            return nullptr;
        for (const auto &sec : goblin::ini_schema())
            for (const auto &e : sec.entries)
                if (std::strcmp(e.key, key) == 0)
                    return &e;
        return nullptr;
    }

    RowKind kind_of(const goblin::IniEntry &e)
    {
        if (e.type == goblin::IniType::Bool)
            return RowKind::Toggle;
        if (e.type == goblin::IniType::Language || is_render_mode_key(e.key))
            return RowKind::Enum;
        if (e.type == goblin::IniType::VkKey)
            return RowKind::Rebind;
        if (range_for(e.key))
            return RowKind::Number;
        return RowKind::Info;
    }

    // ── the choices behind one entry ─────────────────────────────────────────────────
    // Both value kinds boil down to "a list of options with one of them current", which is
    // what the value page renders. Numbers get their list from the range, enums from their
    // fixed table - so the page itself needs no per-type code.
    size_t option_count(const goblin::IniEntry &e)
    {
        if (e.type == goblin::IniType::Language)
            return sizeof(kLanguages) / sizeof(kLanguages[0]);
        if (is_render_mode_key(e.key))
            return sizeof(kRenderModes) / sizeof(kRenderModes[0]);
        const NumRange *r = range_for(e.key);
        if (!r || r->step <= 0.f)
            return 0;
        return static_cast<size_t>((r->max - r->min) / r->step + 0.5f) + 1;
    }

    std::wstring option_label(const goblin::IniEntry &e, size_t index)
    {
        if (e.type == goblin::IniType::Language)
            return wide(tr::language_option_label(kLanguages[index]));
        if (is_render_mode_key(e.key))
            return wide(kRenderModes[index]);
        const NumRange *r = range_for(e.key);
        if (!r)
            return L"";
        wchar_t buf[32];
        const float v = r->min + r->step * static_cast<float>(index);
        if (e.type == goblin::IniType::Float)
            _snwprintf_s(buf, _TRUNCATE, L"%.2f", v);
        else
            _snwprintf_s(buf, _TRUNCATE, L"%d", static_cast<int>(v + 0.5f));
        return buf;
    }

    // Which option is live right now (-1 when the value sits between steps).
    int current_option(const goblin::IniEntry &e)
    {
        if (!e.target)
            return -1;
        if (e.type == goblin::IniType::Language)
        {
            const std::string cur = tr::normalize_language_config(*static_cast<std::string *>(e.target));
            for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); ++i)
                if (cur == kLanguages[i])
                    return static_cast<int>(i);
            return -1;
        }
        if (is_render_mode_key(e.key))
        {
            const std::string &cur = *static_cast<std::string *>(e.target);
            for (size_t i = 0; i < sizeof(kRenderModes) / sizeof(kRenderModes[0]); ++i)
                if (cur == kRenderModes[i])
                    return static_cast<int>(i);
            return -1;
        }
        const NumRange *r = range_for(e.key);
        if (!r || r->step <= 0.f)
            return -1;
        const float v = e.type == goblin::IniType::Float
                            ? *static_cast<float *>(e.target)
                            : static_cast<float>(*static_cast<uint8_t *>(e.target));
        const int ix = static_cast<int>((v - r->min) / r->step + 0.5f);
        return ix >= 0 && static_cast<size_t>(ix) < option_count(e) ? ix : -1;
    }

    void apply_option(const goblin::IniEntry &e, size_t index)
    {
        if (!e.target || index >= option_count(e))
            return;
        if (e.type == goblin::IniType::Language)
            *static_cast<std::string *>(e.target) = kLanguages[index];
        else if (is_render_mode_key(e.key))
            *static_cast<std::string *>(e.target) = kRenderModes[index];
        else if (const NumRange *r = range_for(e.key))
        {
            const float v = r->min + r->step * static_cast<float>(index);
            if (e.type == goblin::IniType::Float)
                *static_cast<float *>(e.target) = v;
            else
                *static_cast<uint8_t *>(e.target) = static_cast<uint8_t>(v + 0.5f);
        }
        g_dirty = true;
        goblin::reapply_live_settings();
    }

    bool entry_visible(const goblin::IniEntry &e)
    {
        return !(e.err_only && goblin::profile_is_vanilla());
    }
    bool section_visible(const goblin::IniSection &s)
    {
        return !(s.err_only && goblin::profile_is_vanilla());
    }

    // ── actions ──────────────────────────────────────────────────────────────────────
    // Bulk switches operate on the section that is currently open, so one pair of
    // actions serves every section (including the ~60 icon-category toggles).
    void set_section_bools(bool on)
    {
        const auto &schema = goblin::ini_schema();
        const size_t ix = static_cast<size_t>(g_page - goblin::nmenu::kPageSectionBase);
        if (ix >= schema.size())
            return;
        size_t n = 0;
        for (const auto &e : schema[ix].entries)
            if (e.type == goblin::IniType::Bool && e.target && entry_visible(e))
            {
                *static_cast<bool *>(e.target) = on;
                ++n;
            }
        g_dirty = true;
        goblin::reapply_live_settings();
        spdlog::info("[nmenu] section '{}': {} bools -> {}", schema[ix].name, n, on ? "on" : "off");
    }
    void action_all_on() { set_section_bools(true); }
    void action_all_off() { set_section_bools(false); }

    void action_unhide_all()
    {
        const size_t n = goblin::manual_hidden_count();
        goblin::clear_manual_hidden();
        goblin::reapply_live_settings();
        spdlog::info("[nmenu] unhid {} manually hidden markers", n);
    }

    void action_save_ini()
    {
        goblin::save_config(goblin::g_ini_path);
        g_dirty = false;
        spdlog::info("[nmenu] ini saved on request");
    }

    void action_toggle_graphic_bar()
    {
        g_graphic_bar = !g_graphic_bar;
        spdlog::info("[nmenu] graphic bar {}", g_graphic_bar ? "on" : "off");
    }

    void action_toggle_markup()
    {
        g_markup = !g_markup;
        spdlog::info("[nmenu] rich text {}", g_markup ? "on" : "off");
    }

    void action_cycle_bar_style()
    {
        g_bar_style = static_cast<BarStyle>((static_cast<int>(g_bar_style) + 1) % 3);
        spdlog::info("[nmenu] bar style -> {}", static_cast<int>(g_bar_style));
    }

    // ── page builders ────────────────────────────────────────────────────────────────
    void push(Row row) { g_rows.push_back(row); }

    void add_back_row()
    {
        Row r;
        r.kind = RowKind::Back;
        // The row's text fields are html=1, so a literal '<' opens a tag and the parser eats
        // the whole label - in game this row came out completely blank. Escaped, and the
        // glyphs stay ASCII because the menu font has no arrows.
        r.value = hold(L"&lt;&lt;  " + wide(tr::tr(tr::TextId::MenuBack)));
        push(r);
    }

    void build_root()
    {
        g_title = L"MapForGoblins";
        const auto &schema = goblin::ini_schema();
        for (size_t s = 0; s < schema.size(); ++s)
        {
            if (!section_visible(schema[s]))
                continue;
            std::wstring name = wide(tr::section_label(schema[s].name));
            if (name.empty())
                name = wide(schema[s].name);
            size_t shown = 0;
            for (const auto &e : schema[s].entries)
                if (entry_visible(e))
                    ++shown;
            Row r;
            r.kind = RowKind::SubPage;
            r.page_id = goblin::nmenu::kPageSectionBase + static_cast<int32_t>(s);
            if (const goblin::IniEntry *pick = section_icon_entry(schema[s]))
            {
                r.icon_id = icon_for_key(pick->key);
                r.ini_key = pick->key; // the draw route finds the strip cell by key
            }
            if (const SectionMark *mk = mark_for(schema[s].name))
                if (!row_icons_drawn() || r.icon_id < 0)
                    name = colored(std::wstring(1, mk->glyph), mk->rgb) + L"  " + name;
            {
                std::wstring tip = wide(tr::section_comment(schema[s].name, ""));
                if (!tip.empty())
                    r.help = hold(std::move(tip));
            }
            r.label = hold(std::move(name));
            wchar_t cnt[24];
            _snwprintf_s(cnt, _TRUNCATE, L"%zu  >", shown);
            r.value = hold(cnt);
            push(r);
        }
        Row prog;
        prog.kind = RowKind::SubPage;
        prog.page_id = goblin::nmenu::kPageProgress;
        prog.label = text(tr::TextId::TabProgress);
        prog.value = hold(L">");
        push(prog);

        Row hid;
        hid.kind = RowKind::SubPage;
        hid.page_id = goblin::nmenu::kPageHidden;
        hid.label = text(tr::TextId::HiddenMarkers);
        wchar_t hc[24];
        _snwprintf_s(hc, _TRUNCATE, L"%zu  >", goblin::manual_hidden_count());
        hid.value = hold(hc);
        push(hid);

        Row act;
        act.kind = RowKind::SubPage;
        act.page_id = goblin::nmenu::kPageActions;
        act.label = text(tr::TextId::MenuTools);
        act.value = hold(L">");
        push(act);

        // Pages contributed by other mods (sdk/mfg_menu_api.h). They appear as ordinary
        // rows, so a player sees one menu for everything.
        goblin::addons::scan();
        for (size_t i = 0; i < goblin::addons::page_count(); ++i)
        {
            const auto *ap = goblin::addons::page(i);
            if (!ap)
                continue;
            Row r;
            r.kind = RowKind::SubPage;
            r.page_id = goblin::nmenu::kPageAddonBase + static_cast<int32_t>(i);
            r.label = hold(ap->title);
            r.value = hold(L">");
            push(r);
        }
    }

    void build_addon(size_t index)
    {
        const auto *ap = goblin::addons::build_page(index);
        if (!ap)
        {
            g_title = L"MapForGoblins";
            add_back_row();
            Row r;
            r.kind = RowKind::Info;
            r.label = text(tr::TextId::MenuUnavailable);
            push(r);
            return;
        }
        g_title = ap->title;
        add_back_row();
        for (const auto &ar : ap->rows)
        {
            Row r;
            switch (ar.kind)
            {
            case 1: // toggle
            case 2: // number
            case 3: // enum
                r.kind = RowKind::Info; // the add-on owns the value; decide forwards to it
                break;
            case 4:
                r.kind = RowKind::Info;
                break;
            case 5:
                r.kind = RowKind::Progress;
                break;
            default:
                r.kind = RowKind::Info;
                break;
            }
            r.label = hold(ar.label);
            r.value = ar.kind == 5 && ar.value.empty()
                          ? hold(bar_text(ar.done, ar.total, g_bar_style))
                          : hold(ar.value);
            r.collected = ar.done;
            r.total = ar.total;
            // Remember where to route a confirm: page index + the add-on's row id.
            r.page_id = static_cast<int32_t>(index);
            r.type_tag = 0xFF; // marks "belongs to an add-on"
            r.addon_row_id = ar.row_id;
            r.addon_kind = ar.kind;
            push(r);
        }
    }

    void build_section(size_t ix)
    {
        const auto &schema = goblin::ini_schema();
        if (ix >= schema.size())
            return;
        const auto &sec = schema[ix];
        std::wstring name = wide(tr::section_label(sec.name));
        if (name.empty())
            name = wide(sec.name);
        g_title = name;
        add_back_row();
        size_t bools = 0;
        for (const auto &e : sec.entries)
        {
            if (!entry_visible(e))
                continue;
            if (e.type == goblin::IniType::Bool)
                ++bools;
            std::wstring label = wide(tr::entry_label(e.key));
            if (label.empty())
                label = wide(e.key);
            // Icon-category rows (show_*) carry the section's marker, so a long list
            // still reads as grouped.
            const int32_t row_icon = icon_for_key(e.key);
            if (const SectionMark *mk = mark_for(sec.name))
                if (std::strncmp(e.key, "show_", 5) == 0 &&
                    (!row_icons_drawn() || row_icon < 0))
                    label = colored(std::wstring(1, mk->glyph), mk->rgb) + L"  " + label;
            Row r;
            r.kind = kind_of(e);
            // The same explanatory text the overlay shows as a tooltip; the form has a wide
            // multiline help line at the bottom, which is exactly where it belongs.
            {
                std::wstring tip = wide(tr::entry_comment(e.key, e.comment ? e.comment : ""));
                if (!tip.empty())
                    r.help = hold(std::move(tip));
            }
            r.label = hold(std::move(label));
            r.value = hold(value_of(e));
            r.ini_key = e.key;
            r.icon_id = row_icon;
            r.target = e.target;
            r.type_tag = static_cast<uint8_t>(e.type);
            push(r);
        }
        if (bools >= 3) // bulk switches only pay off on the long toggle lists
        {
            Row on;
            on.kind = RowKind::Action;
            on.label = text(tr::TextId::AllOn);
            on.action = &action_all_on;
            push(on);
            Row off;
            off.kind = RowKind::Action;
            off.label = text(tr::TextId::AllOff);
            off.action = &action_all_off;
            push(off);
        }
    }

    void build_progress()
    {
        g_title = wide(tr::tr(tr::TextId::TabProgress));
        add_back_row();
        goblin::progress::rebuild();
        const auto &regions = goblin::progress::snapshot();
        if (regions.empty())
        {
            Row r;
            r.kind = RowKind::Info;
            r.label = text(tr::TextId::ProgressNoMarkers);
            push(r);
            return;
        }
        int done = 0, all = 0;
        for (const auto &reg : regions)
        {
            done += reg.collected;
            all += reg.total;
        }
        Row tot;
        tot.kind = RowKind::Progress;
        tot.label = text(tr::TextId::MenuTotal);
        tot.collected = done;
        tot.total = all;
        tot.value = hold(bar_text(done, all, g_bar_style));
        push(tot);
        // Mega-section headers, exactly where the overlay's progress tab puts them: whenever the
        // group changes, and never before the trailing "Other" bucket (place id < 0). The row
        // clip has a purpose-built look for this - the PadCategory frame, one wide caption and
        // no value field - which a valueless Info row already selects.
        int last_mega = -1;
        for (size_t i = 0; i < regions.size(); ++i)
        {
            const auto &reg = regions[i];
            if (reg.total <= 0)
                continue;
            if (reg.place_name_id >= 0 && static_cast<int>(reg.mega) != last_mega)
            {
                last_mega = static_cast<int>(reg.mega);
                const tr::TextId mid =
                    reg.mega == goblin::progress::Mega::LandsBetween ? tr::TextId::MegaLandsBetween
                    : reg.mega == goblin::progress::Mega::Dungeons   ? tr::TextId::MegaDungeons
                                                                     : tr::TextId::MegaShadow;
                Row head;
                head.kind = RowKind::Info; // valueless -> drawn on the PadCategory frame
                head.label = hold(colored(wide(tr::tr(mid)), kColHeader));
                push(head);
            }
            // A region opens its own page with the per-category breakdown, the same numbers
            // the overlay's progress tab shows when a region is expanded.
            Row r;
            r.kind = RowKind::SubPage;
            r.page_id = goblin::nmenu::kPageRegionBase + static_cast<int32_t>(i);
            r.label = hold(wide(reg.name.c_str()));
            r.collected = reg.collected;
            r.total = reg.total;
            r.value = hold(bar_text(reg.collected, reg.total, g_bar_style));
            push(r);
        }
    }

    void build_region(size_t index)
    {
        const auto &regions = goblin::progress::snapshot();
        if (index >= regions.size())
        {
            g_title = wide(tr::tr(tr::TextId::TabProgress));
            add_back_row();
            return;
        }
        const auto &reg = regions[index];
        g_title = hold(wide(reg.name.c_str()));
        add_back_row();
        Row tot;
        tot.kind = RowKind::Progress;
        tot.label = text(tr::TextId::MenuTotal);
        tot.collected = reg.collected;
        tot.total = reg.total;
        tot.value = hold(bar_text(reg.collected, reg.total, g_bar_style));
        push(tot);
        for (int ci = 0; ci < goblin::progress::kCategoryCount; ++ci)
        {
            if (reg.cats[ci].total <= 0)
                continue;
            const auto cat = static_cast<goblin::generated::Category>(ci);
            // Same label source as the overlay: the category's ini key carries the localized
            // name and the icon, with the raw enum name only as a last resort.
            const char *key = goblin::category_config_key(cat);
            Row r;
            r.kind = RowKind::Progress;
            r.label = hold(wide(key ? tr::entry_label(key)
                                    : goblin::markers::category_name(cat)));
            if (key)
            {
                r.icon_id = icon_for_key(key);
                r.ini_key = key; // the icon route looks the strip cell up by key
            }
            // Confirming a category isolates it on the live map, exactly like the overlay's
            // progress tab: only its uncollected markers stay, and they get the glow icon.
            r.collected = reg.cats[ci].collected;
            r.total = reg.cats[ci].total;
            r.type_tag = static_cast<uint8_t>(ci);
            r.page_id = reg.place_name_id;
            std::wstring val = bar_text(r.collected, r.total, g_bar_style);
            // The focused category reads in red. A real background fill is not available:
            // these fields take HTML, and GFx HTML has font colour but no background - that is
            // an ActionScript property of the text field, which we do not drive.
            if (goblin::focus_category() == ci && goblin::focus_region() == reg.place_name_id)
            {
                val = colored(bar_text(r.collected, r.total, g_bar_style), kColFocus);
                r.plate = true; // the row's own red plate marks what is isolated on the map
            }
            r.value = hold(std::move(val));
            push(r);
        }
    }

    // ── hidden markers ───────────────────────────────────────────────────────────────
    // Restoring ONE marker needs its key, and a row's action is a bare function pointer, so
    // the key travels in the row (page_id holds the index into the snapshot the page was
    // built from) and the handler re-reads the snapshot. Deleting from under an index is
    // safe because the page is rebuilt immediately afterwards.
    std::vector<goblin::HiddenMarkerInfo> g_hidden_view;

    // A row action is a bare function pointer, so the row it fired on is handed over here
    // (set by activate() around the call). That keeps Row a plain aggregate - no captures,
    // no allocation - while still letting one handler serve a whole list.
    const Row *g_active_row = nullptr;

    void action_unhide_one()
    {
        if (!g_active_row)
            return;
        const size_t ix = static_cast<size_t>(g_active_row->page_id);
        if (ix >= g_hidden_view.size())
            return;
        goblin::unhide_marker(g_hidden_view[ix].key);
        goblin::persist_manual_hidden();
        goblin::reapply_live_settings();
    }

    void build_hidden()
    {
        g_title = hold(wide(tr::tr(tr::TextId::HiddenMarkers)));
        add_back_row();
        g_hidden_view = goblin::manual_hidden_snapshot();
        if (g_hidden_view.empty())
        {
            Row info;
            info.kind = RowKind::Info;
            info.label = text(tr::TextId::HiddenMarkersNone);
            push(info);
            return;
        }
        Row un;
        un.kind = RowKind::Action;
        un.label = text(tr::TextId::UnhideAll);
        wchar_t cnt[24];
        _snwprintf_s(cnt, _TRUNCATE, L"%zu", g_hidden_view.size());
        un.value = hold(cnt);
        un.action = &action_unhide_all;
        push(un);
        for (size_t i = 0; i < g_hidden_view.size(); ++i)
        {
            const auto &h = g_hidden_view[i];
            // Same three parts the overlay's Hidden tab shows: item, where it was, category.
            const wchar_t *name = goblin::lookup_text(h.textId);
            const wchar_t *where = h.region > 0 ? goblin::lookup_text(h.region) : nullptr;
            const auto cat = static_cast<goblin::generated::Category>(h.cat);
            const char *ckey = goblin::category_config_key(cat);
            std::wstring label = name && *name ? name : L"?";
            if (where && *where)
                label += std::wstring(L"  -  ") + where;
            Row r;
            r.kind = RowKind::Action;
            r.page_id = static_cast<int32_t>(i);
            r.action = &action_unhide_one;
            r.label = hold(std::move(label));
            r.value = text(tr::TextId::Unhide);
            if (ckey)
            {
                r.icon_id = icon_for_key(ckey);
                r.ini_key = ckey; // same reason as the progress rows
                r.help = hold(wide(tr::entry_label(ckey)));
            }
            if (r.icon_id < 0)
                r.label = hold(colored(L" 22", kColValue) + L"  " +
                               std::wstring(r.label ? r.label : L""));
            push(r);
        }
    }

    // ── value page: the choices behind one Number/Enum entry ─────────────────────────
    void build_value()
    {
        const goblin::IniEntry *e = g_edit_entry;
        if (!e)
        {
            g_title = L"MapForGoblins";
            add_back_row();
            return;
        }
        std::wstring name = wide(tr::entry_label(e->key));
        g_title = name.empty() ? wide(e->key) : name;
        add_back_row();
        const int cur = current_option(*e);
        const size_t n = option_count(*e);
        for (size_t i = 0; i < n; ++i)
        {
            const bool live = static_cast<int>(i) == cur;
            Row r;
            r.kind = RowKind::ValueOption;
            r.page_id = static_cast<int32_t>(i);
            // The live choice is marked in the value column, so the label column stays a
            // clean list and the marker reads the same on every row width.
            r.label = hold(option_label(*e, i));
            if (live)
                r.value = hold(colored(L"\x2022", kColOn));
            push(r);
        }
    }

    // ── rebind page ──────────────────────────────────────────────────────────────────
    bool g_rebind_waiting = false;

    void build_rebind()
    {
        const goblin::IniEntry *e = g_edit_entry;
        if (!e)
        {
            g_title = L"MapForGoblins";
            add_back_row();
            return;
        }
        std::wstring name = wide(tr::entry_label(e->key));
        g_title = name.empty() ? wide(e->key) : name;
        add_back_row();
        Row prompt;
        prompt.kind = RowKind::Info;
        prompt.label = text(tr::TextId::MenuPressKey);
        prompt.value = hold(value_of(*e));
        push(prompt);
        Row keep;
        keep.kind = RowKind::Back;
        keep.label = text(tr::TextId::MenuKeepCurrent);
        push(keep);
    }

    void build_actions()
    {
        g_title = hold(wide(tr::tr(tr::TextId::MenuTools)));
        add_back_row();
        Row save;
        save.kind = RowKind::Action;
        save.label = text(tr::TextId::MenuSaveNow);
        save.action = &action_save_ini;
        push(save);
        // Everything below is diagnostics for us, not settings for the player: the strings
        // are deliberately un-localized because they never reach a release screen.
        if (!goblin::config::nativeMenuDevRows)
            return;
        Row bar;
        bar.kind = RowKind::Action;
        bar.label = hold(L"Progress bar style");
        bar.value = hold(g_bar_style == BarStyle::Ascii   ? L"ascii"
                         : g_bar_style == BarStyle::Blocks ? L"blocks"
                                                           : L"both");
        bar.action = &action_cycle_bar_style;
        push(bar);
        Row markup;
        markup.kind = RowKind::Action;
        markup.label = hold(L"Rich text (colour)");
        markup.value = hold(g_markup ? colored(L"on", kColOn) : std::wstring(L"off"));
        markup.action = &action_toggle_markup;
        push(markup);
        Row gbar;
        gbar.kind = RowKind::Action;
        gbar.label = hold(L"Graphic bar (clip scale)");
        gbar.value = hold(g_graphic_bar ? colored(L"on", kColOn) : std::wstring(L"off"));
        gbar.action = &action_toggle_graphic_bar;
        push(gbar);
        Row icons;
        icons.kind = RowKind::Info;
        icons.label = hold(L"Row icons");
        {
            // Report the path we actually use (our own pixels into a row clip), not the
            // retired movie-splice experiment.
            const auto st = goblin::sfimage::icon_state();
            const bool ok = st == goblin::sfimage::IconState::Drawn;
            icons.value = hold(colored(wide(goblin::sfimage::icon_state_name()),
                                       ok ? kColOn : kColOff));
        }
        push(icons);
        Row movie;
        movie.kind = RowKind::Info;
        movie.label = hold(L"Own movie");
        movie.value = hold(wide(goblin::own_movie::status()));
        push(movie);
        Row demo;
        demo.kind = RowKind::Progress;
        demo.label = hold(L"Bar preview");
        demo.collected = 7;
        demo.total = 10;
        demo.value = hold(bar_text(7, 10, g_bar_style));
        push(demo);
        // A row whose value shows raw markup, so one glance tells us whether the field
        // renders HTML or prints the tags verbatim.
        Row probe;
        probe.kind = RowKind::Info;
        probe.label = hold(L"Markup check");
        probe.value = hold(L"<font color=\"#FF6060\">red</font> plain");
        push(probe);
    }

    // Build the right-hand preview for `page` (currently: the rows of an ini section as
    // "name  value" lines).
    void build_preview(int32_t page)
    {
        g_preview_rows.clear();
        g_preview_arena.clear();
        if (page < goblin::nmenu::kPageSectionBase)
            return;
        const auto &schema = goblin::ini_schema();
        const size_t ix = static_cast<size_t>(page - goblin::nmenu::kPageSectionBase);
        if (ix >= schema.size())
            return;
        auto hold_preview = [](std::wstring t) -> const wchar_t *
        {
            g_preview_arena.push_back(std::move(t));
            return g_preview_arena.back().c_str();
        };
        std::wstring head = wide(tr::section_label(schema[ix].name));
        if (head.empty())
            head = wide(schema[ix].name);
        Row title;
        title.kind = RowKind::Info;
        title.right_column = true;
        title.value = hold_preview(colored(head, kColValue));
        g_preview_rows.push_back(title);
        for (const auto &e : schema[ix].entries)
        {
            if (!entry_visible(e))
                continue;
            std::wstring label = wide(tr::entry_label(e.key));
            if (label.empty())
                label = wide(e.key);
            if (label.size() > 26)
                label = label.substr(0, 25) + L"…";
            Row r;
            r.kind = RowKind::Info;
            r.right_column = true;
            r.value = hold_preview(label + L"   " + value_of(e));
            g_preview_rows.push_back(r);
            if (g_preview_rows.size() >= 11) // the form shows 11 slots per column
                break;
        }
    }

    void build_current()
    {
        g_rows.clear();
        g_arena.clear();
        if (g_page == goblin::nmenu::kPageRoot)
            build_root();
        else if (g_page == goblin::nmenu::kPageProgress)
            build_progress();
        else if (g_page == goblin::nmenu::kPageHidden)
            build_hidden();
        else if (g_page == goblin::nmenu::kPageActions)
            build_actions();
        else if (g_page == goblin::nmenu::kPageValue)
            build_value();
        else if (g_page == goblin::nmenu::kPageRebind)
            build_rebind();
        else if (g_page >= goblin::nmenu::kPageRegionBase)
            build_region(static_cast<size_t>(g_page - goblin::nmenu::kPageRegionBase));
        else if (g_page >= goblin::nmenu::kPageAddonBase)
            build_addon(static_cast<size_t>(g_page - goblin::nmenu::kPageAddonBase));
        else if (g_page >= goblin::nmenu::kPageSectionBase)
            build_section(static_cast<size_t>(g_page - goblin::nmenu::kPageSectionBase));
        else
            build_root();
        build_preview(g_preview_page);
    }
}

void goblin::nmenu::rebuild() { build_current(); }

const goblin::nmenu::Row *goblin::nmenu::rows(size_t *count)
{
    if (count)
        *count = g_rows.size();
    return g_rows.empty() ? nullptr : g_rows.data();
}

const wchar_t *goblin::nmenu::page_title() { return g_title.c_str(); }
int32_t goblin::nmenu::current_page() { return g_page; }

void goblin::nmenu::set_page(int32_t page)
{
    // Leaving an editing page ends the edit, exactly as navigate_back() does: nothing may keep
    // polling for a key once its screen is gone, and a stale entry pointer must not be reachable
    // from the page that resumes.
    if (page != kPageValue && page != kPageRebind)
    {
        g_rebind_waiting = false;
        g_edit_entry = nullptr;
    }
    if (g_page == page)
        return;
    g_page = page;
    g_preview_page = -1;
    build_current();
}

void goblin::nmenu::set_nested(bool on)
{
    g_nested = on;
    g_child_page = -1;
    g_want_close = false;
}

bool goblin::nmenu::nested() { return g_nested; }

int32_t goblin::nmenu::take_child_page()
{
    const int32_t p = g_child_page;
    g_child_page = -1;
    return p;
}

bool goblin::nmenu::take_close_request()
{
    const bool c = g_want_close;
    g_want_close = false;
    return c;
}

int32_t goblin::nmenu::subpage_target(size_t row_index)
{
    if (row_index >= g_rows.size())
        return -1;
    const Row &r = g_rows[row_index];
    return r.kind == RowKind::SubPage ? r.page_id : -1;
}

void goblin::nmenu::reset_to_root()
{
    g_page = kPageRoot;
    g_stack.clear();
    build_current();
}

bool goblin::nmenu::activate(size_t row_index)
{
    if (row_index >= g_rows.size())
        return false;
    const Row row = g_rows[row_index]; // copy: the rebuild below invalidates the vector
    switch (row.kind)
    {
    case RowKind::Back:
        if (g_nested)
        {
            g_want_close = true; // the engine's own Back does the rest
            return false;
        }
        return navigate_back();
    case RowKind::SubPage:
        if (g_nested)
        {
            g_child_page = row.page_id;
            return false; // this screen stays as it is; the host opens the next one
        }
        // One confirm, one level down. The two-step "preview first, enter on the second
        // press" behaviour went away with the right-hand preview column: making the first
        // press do nothing visible to the list is worse than no preview at all.
        g_stack.push_back(g_page);
        g_page = row.page_id;
        g_preview_page = -1;
        build_current();
        return true;
    case RowKind::Toggle:
        if (row.target)
        {
            bool *v = static_cast<bool *>(row.target);
            *v = !*v;
            g_dirty = true;
            spdlog::info("[nmenu] {} -> {}", row.ini_key ? row.ini_key : "?", *v ? "on" : "off");
        }
        build_current();
        return true;
    case RowKind::Number:
    case RowKind::Enum:
        // A list of choices beats stepping-with-wrap: every option is visible at once, the
        // live one is marked, and it needs no left/right input the grid already owns.
        if (const goblin::IniEntry *e = entry_for_key(row.ini_key))
        {
            g_edit_entry = e;
            if (g_nested)
            {
                g_child_page = kPageValue;
                return false;
            }
            g_stack.push_back(g_page);
            g_page = kPageValue;
            g_preview_page = -1;
            build_current();
            return true;
        }
        return false;
    case RowKind::ValueOption:
        if (g_edit_entry)
        {
            apply_option(*g_edit_entry, static_cast<size_t>(row.page_id));
            spdlog::info("[nmenu] {} set from its value page", g_edit_entry->key);
        }
        if (g_nested)
        {
            g_want_close = true; // the value page has done its one job
            return false;
        }
        return navigate_back();
    case RowKind::Rebind:
        if (const goblin::IniEntry *e = entry_for_key(row.ini_key))
        {
            g_edit_entry = e;
            g_rebind_waiting = true;
            if (g_nested)
            {
                g_child_page = kPageRebind;
                return false;
            }
            g_stack.push_back(g_page);
            g_page = kPageRebind;
            g_preview_page = -1;
            build_current();
            return true;
        }
        return false;
    case RowKind::Action:
        if (row.action)
        {
            g_active_row = &row;
            row.action();
            g_active_row = nullptr;
        }
        build_current();
        return true;
    case RowKind::Progress:
        // A per-category row on a region page: same behaviour as the overlay's progress tab -
        // confirming isolates that category's uncollected markers on the live map and gives
        // them the glow icon; confirming the focused one again clears the filter.
        if (row.type_tag != 0xFF && row.ini_key && g_page >= kPageRegionBase)
        {
            const int cat = static_cast<int>(row.type_tag);
            const bool same = goblin::focus_category() == cat &&
                              goblin::focus_region() == row.page_id;
            if (same)
                goblin::set_focus_category(-1);
            else
                goblin::set_focus_category(cat, row.page_id);
            goblin::reapply_live_settings();
            goblin::apply_focus_highlight(); // swap the focused rows to the glow icon
            build_current();
            spdlog::info("[nmenu] progress focus: category {} region {} -> {}", cat, row.page_id,
                         same ? "cleared" : "isolated");
            return true;
        }
        [[fallthrough]];
    case RowKind::Info:
        // An add-on row: let the owning add-on handle it and rebuild if it changed.
        if (row.type_tag == 0xFF)
        {
            const bool changed =
                goblin::addons::activate(static_cast<size_t>(row.page_id), row.addon_row_id);
            if (changed)
                build_current();
            return changed;
        }
        return false;
    }
    return false;
}

const goblin::nmenu::Row *goblin::nmenu::right_row(size_t index)
{
    return index < g_preview_rows.size() ? &g_preview_rows[index] : nullptr;
}

bool goblin::nmenu::navigate_back()
{
    if (g_stack.empty())
        return false;
    g_page = g_stack.back();
    g_stack.pop_back();
    g_preview_page = -1;
    // Leaving an editing page ends the edit: nothing else may keep polling for a key, and a
    // stale entry pointer must not be reachable from the next page.
    g_rebind_waiting = false;
    if (g_page != kPageValue && g_page != kPageRebind)
        g_edit_entry = nullptr;
    build_current();
    return true;
}

bool goblin::nmenu::rebind_pending()
{
    return g_rebind_waiting && g_page == kPageRebind && g_edit_entry != nullptr;
}

void goblin::nmenu::rebind_apply(uint32_t vk)
{
    if (g_edit_entry && g_edit_entry->target && vk != 0)
    {
        *static_cast<uint32_t *>(g_edit_entry->target) = vk;
        g_dirty = true;
        goblin::reapply_live_settings();
        spdlog::info("[nmenu] {} rebound to 0x{:02X}", g_edit_entry->key, vk);
    }
    g_rebind_waiting = false;
    if (g_nested)
    {
        g_want_close = true; // the "press a key" screen closes itself once it has one
        return;
    }
    navigate_back();
}

size_t goblin::nmenu::depth() { return g_stack.size(); }

bool goblin::nmenu::dirty() { return g_dirty; }
void goblin::nmenu::clear_dirty() { g_dirty = false; }
void goblin::nmenu::set_bar_style(BarStyle style) { g_bar_style = style; }
goblin::nmenu::BarStyle goblin::nmenu::bar_style() { return g_bar_style; }
bool goblin::nmenu::graphic_bar() { return g_graphic_bar; }
