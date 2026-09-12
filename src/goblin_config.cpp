#include "goblin_config.hpp"
#include "goblin_config_schema.hpp"
#include "goblin_i18n.hpp"

#include <spdlog/spdlog.h>
#include <mini/ini.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// NOTE: the goblin::config:: variables and the ini schema/emitter live in
// goblin_config_schema.cpp (kept dependency-free so tools/mfg_inigen links it).
// This file holds the runtime: ensure/migrate the ini, then load values.

namespace
{
    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    template <typename T>
    void store_if_changed(void *target, const T &value)
    {
        T &slot = *static_cast<T *>(target);
        if (!(slot == value))
            slot = value;
    }

    // A pad combo as written in the ini: true with `out` set when it parses, including the explicit
    // unbind words ("none"/"off"/empty/"0" -> 0), false when the text is not a combo at all.
    // "none"/"off"/empty must be able to UNBIND a pad button, not fall through to the default.
    // Reported 2026-08-01: hide_marker_gamepad defaults to RB, which is also the map's own
    // tab-switch button, so the player hid their markers by accident all session and could not turn
    // it off - clearing the key left RB in place, because an unparsed value was indistinguishable
    // from an empty one and both were ignored.
    bool parse_mask_setting(const std::string &v, uint16_t &out)
    {
        std::string t = v;
        t.erase(0, t.find_first_not_of(" \t"));
        const size_t last = t.find_last_not_of(" \t");
        t.erase(last == std::string::npos ? 0 : last + 1);
        std::string up = t;
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);
        if (t.empty() || up == "NONE" || up == "OFF" || up == "0")
        {
            out = 0;
            return true;
        }
        out = goblin::parse_gamepad_combo(t);
        return out != 0;
    }

    // Parse the value an entry resolves to and store it into the entry's typed target - once, and
    // only when it differs from what the target holds. A key/combo that does not parse takes the
    // schema default (what the old defaults-first load left in place).
    //
    // Why not "reset everything to defaults, then read the file" (the old apply_defaults): the
    // overlay reloads the ini on every open while the map, marker and menu threads keep reading these
    // globals, so for the length of the file read every setting sat at its default in plain view.
    // Measured 2026-09-11: an imgui-mode F10 over the map met menu_render_mode = "native" in that
    // window and opened the in-game settings screen with the overlay. Storing only changed values
    // also means an unchanged string is never rewritten under a reader.
    void assign_from_string(const goblin::IniEntry &e, const std::string &v)
    {
        using goblin::IniType;
        switch (e.type)
        {
        case IniType::Bool:
            store_if_changed(e.target, v != "false");
            break;
        case IniType::U8:
        {
            int val;
            try { val = std::stoi(v); }
            catch (...) { val = 15; }
            if (val < 0 || val > 255) val = 15;
            store_if_changed(e.target, static_cast<uint8_t>(val));
            break;
        }
        case IniType::VkKey:
        {
            uint32_t vk = goblin::parse_vk_code(v);
            if (!vk)
                vk = goblin::parse_vk_code(e.def);
            if (vk)
                store_if_changed(e.target, vk);
            break;
        }
        case IniType::GamepadMask:
        {
            uint16_t m = 0;
            if (parse_mask_setting(v, m) || parse_mask_setting(e.def, m))
                store_if_changed(e.target, m);
            break;
        }
        case IniType::Language:
            store_if_changed(e.target, goblin::i18n::normalize_language_config(v));
            break;
        case IniType::Text:
            store_if_changed(e.target, v);
            break;
        case IniType::Float:
        {
            // No generic range clamp here: different Float keys have different valid
            // ranges (font_scale 0.8-3.0, overlay_opacity 0.3-1.0, overlay_window_x/y are
            // 0..1 fractions, overlay_window_w/h are pixels). Each consumer clamps its own
            // value. A hard [0.5,3.0] clamp here mangled everything else (e.g. it turned
            // overlay_window_y=0.03 into 0.5). stof-failure falls back to a neutral 1.0.
            float val;
            try { val = std::stof(v); }
            catch (...) { val = 1.0f; }
            store_if_changed(e.target, val);
            break;
        }
        }
    }
}

// A key's former names, newest first. rename_from holds them comma-separated because a key can
// outlive more than one rename, and the value has to follow it every time.
static std::vector<std::string> former_names(const char *rename_from)
{
    std::vector<std::string> out;
    if (!rename_from)
        return out;
    std::string cur;
    for (const char *p = rename_from;; ++p)
    {
        if (*p == ',' || *p == 0)
        {
            while (!cur.empty() && cur.front() == ' ')
                cur.erase(cur.begin());
            while (!cur.empty() && cur.back() == ' ')
                cur.pop_back();
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
            if (*p == 0)
                break;
            continue;
        }
        cur.push_back(*p);
    }
    return out;
}

// The first former name actually present in this section, or an empty string.
static std::string first_present(const mINI::INIMap<std::string> &cfg, const char *rename_from)
{
    for (const std::string &was : former_names(rename_from))
        if (cfg.has(was))
            return was;
    return {};
}

void goblin::ensure_ini(const std::filesystem::path &ini_path)
{
    namespace fs = std::filesystem;
    const bool include_err = !profile_is_vanilla();

    mINI::INIStructure existing;
    bool had = fs::exists(ini_path);
    if (had)
    {
        mINI::INIFile f(ini_path.string());
        if (!f.read(existing)) had = false;
    }

    std::set<std::pair<std::string, std::string>> consumed; // lowercased (section,key)

    IniValueResolver resolve =
        [&](const std::string &section, const IniEntry &e, std::string &out) -> bool
    {
        if (!had) return false;
        std::string lsec = to_lower(section);
        if (existing.has(section) && existing[section].has(e.key))
        {
            out = existing[section].get(e.key);
            consumed.insert({lsec, to_lower(e.key)});
            return true;
        }
        for (const std::string &was : former_names(e.rename_from))
            if (existing.has(section) && existing[section].has(was))
            {
                out = existing[section].get(was);
                consumed.insert({lsec, to_lower(was)});
                return true;
            }
        // Cross-section fallback: a key (or its rename_from) that moved to a
        // different section in a newer schema still keeps its user value. Schema
        // keys are globally unique, so matching by key name in any section is safe.
        for (auto const &sp : existing)
        {
            if (sp.second.has(e.key))
            {
                out = sp.second.get(e.key);
                consumed.insert({to_lower(sp.first), to_lower(e.key)});
                return true;
            }
            for (const std::string &was : former_names(e.rename_from))
                if (sp.second.has(was))
                {
                    out = sp.second.get(was);
                    consumed.insert({to_lower(sp.first), to_lower(was)});
                    return true;
                }
        }
        return false;
    };

    // The comments in the file we are about to write are localized, so the language has to be read
    // BEFORE the schema pass. Both spellings are accepted: a file from an older build still says
    // ui_language, and it is renamed by the same pass that writes the new one.
    std::string language_value = goblin::config::overlayUiLanguage;
    if (had)
    {
        for (auto const &sp : existing)
        {
            for (const char *key : {"overlay_ui_language", "ui_language"})
                if (sp.second.has(key))
                {
                    language_value = sp.second.get(key);
                    break;
                }
        }
    }
    auto emit_language = goblin::i18n::language_from_config(language_value);

    // Before writing: say what this rewrite is about to CHANGE. `existing` is still the file as the
    // player left it, so this is the only place that can tell a preserved value from a lost one -
    // a flag that comes back at its default after an upgrade has to be visible in the log, not just
    // in the file.
    if (had)
    {
        int changed = 0;
        for (auto const &sec : ini_schema())
        {
            if (sec.err_only && !include_err)
                continue;
            for (auto const &e : sec.entries)
            {
                if (e.err_only && !include_err)
                    continue;
                std::string before_val;
                bool found = false;
                for (auto const &sp : existing)
                    if (sp.second.has(e.key))
                    {
                        before_val = sp.second.get(e.key);
                        found = true;
                        break;
                    }
                if (!found)
                    continue; // a key the file never had - nothing to lose
                std::string after_val;
                if (!resolve(sec.name, e, after_val))
                    after_val = e.def;
                if (after_val == before_val)
                    continue;
                spdlog::warn("Config: rewrite changes {} : {} -> {}", e.key, before_val, after_val);
                ++changed;
            }
        }
        if (changed)
            spdlog::warn("Config: the ini rewrite changed {} value(s) - if any of them was yours, "
                         "that is a bug in the migration, not a setting", changed);
    }

    std::ostringstream ss;
    emit_ini(ss, include_err, resolve, emit_language);

    // Keys in the old file that the schema (for this build) didn't claim:
    // renamed-away, removed, or ERR-only in a vanilla build. Comment them out,
    // preserving the value, rather than silently dropping them.
    if (had)
    {
        std::vector<std::string> dead;
        std::vector<std::string> retired;
        for (auto const &sec_pair : existing)
        {
            const std::string &sname = sec_pair.first;
            for (auto const &kv : sec_pair.second)
            {
                if (consumed.count({to_lower(sname), to_lower(kv.first)}))
                    continue;
                // A key WE retired is dropped outright: keeping it commented would leave the file
                // accumulating a graveyard across versions. Anything else unclaimed is preserved as a
                // comment, because it may be a value from a newer build or a key we are about to rename.
                const std::string lkey = to_lower(kv.first);
                bool is_retired = false;
                for (const char *r : goblin::ini_retired_keys())
                    if (lkey == to_lower(r)) { is_retired = true; break; }
                if (is_retired)
                    retired.push_back(kv.first);
                else
                    dead.push_back("[" + sname + "] " + kv.first + " = " + kv.second);
            }
        }
        if (!retired.empty())
        {
            std::string names;
            for (size_t i = 0; i < retired.size(); ++i)
                names += (i ? ", " : "") + retired[i];
            spdlog::info("Config: dropped {} retired setting(s) from the ini: {}", retired.size(), names);
        }
        if (!dead.empty())
        {
            ss << "\n; ---------------------------------------------------------------\n"
               << "; No longer used by this version (renamed, removed, or not applicable\n"
               << "; to this build). Kept commented for reference; safe to delete.\n";
            for (auto const &d : dead) ss << "; " << d << "\n";
        }
    }

    std::string desired = ss.str();
    std::string current;
    if (had)
    {
        std::ifstream in(ini_path, std::ios::binary);
        std::ostringstream b;
        b << in.rdbuf();
        current = b.str();
    }
    if (!had || current != desired)
    {
        std::error_code ec;
        if (ini_path.has_parent_path()) fs::create_directories(ini_path.parent_path(), ec);
        std::ofstream out(ini_path, std::ios::binary | std::ios::trunc);
        out << desired;
        spdlog::info("Config: ini {}", had ? "re-synced" : "created with defaults");
    }
}

std::filesystem::path goblin::g_ini_path;

void goblin::save_config(const std::filesystem::path &ini_path)
{
    const bool include_err = !profile_is_vanilla();

    IniValueResolver resolve =
        [&](const std::string &section, const IniEntry &e, std::string &out) -> bool
    {
        switch (e.type)
        {
        case IniType::Bool:
            out = *static_cast<bool *>(e.target) ? "true" : "false";
            return true;
        case IniType::U8:
            out = std::to_string(static_cast<int>(*static_cast<uint8_t *>(e.target)));
            return true;
        case IniType::VkKey: // live value (the overlay can rebind these now)
            out = format_vk_code(*static_cast<uint32_t *>(e.target));
            return true;
        case IniType::GamepadMask:
            out = format_gamepad_combo(*static_cast<uint16_t *>(e.target));
            return true;
        case IniType::Language:
            out = goblin::i18n::normalize_language_config(*static_cast<std::string *>(e.target));
            return true;
        case IniType::Text:
            out = *static_cast<std::string *>(e.target);
            return true;
        case IniType::Float:
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%.2f", *static_cast<float *>(e.target));
            out = buf;
            return true;
        }
        default:
            return false;
        }
    };

    std::ostringstream ss;
    // Audit: name every value this write CHANGES. A settings file that quietly comes back with
    // different flags is the one bug report nobody can act on, so each change is logged with its old
    // and new value - whoever wrote it (a menu row, a migration, a reset) is then visible in the log.
    {
        mINI::INIFile before_file(ini_path.string());
        mINI::INIStructure before;
        if (before_file.read(before))
        {
            int changed = 0;
            for (auto const &sec : ini_schema())
            {
                if (!before.has(sec.name))
                    continue;
                auto &old_sec = before[sec.name];
                for (auto const &e : sec.entries)
                {
                    if (!old_sec.has(e.key))
                        continue;
                    std::string now;
                    if (!resolve(sec.name, e, now))
                        continue;
                    if (old_sec.get(e.key) == now)
                        continue;
                    spdlog::info("Config: {} = {} (was {})", e.key, now, old_sec.get(e.key));
                    ++changed;
                }
            }
            if (changed)
                spdlog::info("Config: {} value(s) changed by this save", changed);
        }
    }
    emit_ini(ss, include_err, resolve, goblin::i18n::current_language());

    std::error_code ec;
    if (ini_path.has_parent_path())
        std::filesystem::create_directories(ini_path.parent_path(), ec);
    std::ofstream out(ini_path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        spdlog::warn("save_config: cannot open {}", ini_path.string());
        return;
    }
    out << ss.str();
    spdlog::info("Config: saved to {}", ini_path.string());
}

void goblin::load_config(const std::filesystem::path &ini_path)
{
    spdlog::info("Config: {}", ini_path.string());
    g_ini_path = ini_path;

    ensure_ini(ini_path);

    mINI::INIFile file(ini_path.string());
    mINI::INIStructure ini;
    const bool have_file = file.read(ini);
    if (!have_file)
        spdlog::warn("Failed to read INI file, using defaults");

    // Every entry resolves to ONE final value - the file's, a former key's, or the schema default -
    // and is stored once (assign_from_string explains why there is no defaults-first pass). In the
    // vanilla build ERR-only entries are force-disabled, so their features stay off even if an ERR
    // ini is present.
    const bool include_err = !profile_is_vanilla();
    for (auto const &sec : ini_schema())
    {
        const bool section_present = have_file && ini.has(sec.name);
        for (auto const &e : sec.entries)
        {
            if ((e.err_only || sec.err_only) && !include_err)
            {
                if (e.type == IniType::Bool)
                    store_if_changed(e.target, false);
                continue;
            }
            std::string v = e.def;
            if (section_present)
            {
                auto &cfg = ini[sec.name];
                if (cfg.has(e.key))
                    v = cfg.get(e.key);
                else if (const std::string was = first_present(cfg, e.rename_from); !was.empty())
                    v = cfg.get(was);
            }
            assign_from_string(e, v);
        }
    }

    // This key used to name a DRAWING mode (surface / layered / swapchain / swapchain_2).
    // It now names WHICH MENU is on the hotkey, so a file written by an older build carries a value
    // that no longer means anything. Rewrite it to the default and say so: leaving the old word in
    // the file would have it read as "native" while looking like a setting that does something else.
    {
        const std::string &m = goblin::config::menuRenderMode;
        if (m != "native" && m != "imgui" && m != "dev")
        {
            spdlog::info("Config: menu_render_mode '{}' is from an older build (it named a drawing "
                         "mode); migrating to 'native'.", m);
            goblin::config::menuRenderMode = "native";
            save_config(ini_path);
        }
    }
}

// Parse an XInput button combo string like "Y+R3" or "LB+RB+START" into a
// bitmask. Buttons separated by '+'. Returns 0 if any token is unknown.
uint16_t goblin::parse_gamepad_combo(std::string s)
{
    static const std::unordered_map<std::string, uint16_t> name_to_mask = {
        {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
        {"LB", 0x0100}, {"RB", 0x0200},
        {"L3", 0x0040}, {"LSTICK", 0x0040},
        {"R3", 0x0080}, {"RSTICK", 0x0080},
        {"BACK", 0x0020}, {"SELECT", 0x0020}, {"VIEW", 0x0020},
        {"START", 0x0010}, {"MENU", 0x0010},
        {"DPAD_UP", 0x0001}, {"UP", 0x0001},
        {"DPAD_DOWN", 0x0002}, {"DOWN", 0x0002},
        {"DPAD_LEFT", 0x0004}, {"LEFT", 0x0004},
        {"DPAD_RIGHT", 0x0008}, {"RIGHT", 0x0008},
    };
    uint16_t mask = 0;
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    size_t pos = 0;
    while (pos < s.size())
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '+'))
            pos++;
        size_t end = pos;
        while (end < s.size() && s[end] != '+' && s[end] != ' ' && s[end] != '\t')
            end++;
        if (end == pos) break;
        std::string tok = s.substr(pos, end - pos);
        auto it = name_to_mask.find(tok);
        if (it == name_to_mask.end())
        {
            spdlog::warn("Unknown gamepad button: '{}'", tok);
            return 0;
        }
        mask |= it->second;
        pos = end;
    }
    return mask;
}

// Parse a human key name ("F9", "A", "Home", "0", "Space") into Win32 VK_* code.
// Returns 0 on unknown name.
uint32_t goblin::parse_vk_code(std::string name)
{
    for (auto &c : name)
        if (c >= 'a' && c <= 'z') c -= 32; // uppercase

    if (name.empty()) return 0;

    if (name.size() > 2 && name[0] == '0' && name[1] == 'X') // raw "0xNN" code
    {
        try { return static_cast<uint32_t>(std::stoul(name.substr(2), nullptr, 16)); }
        catch (...) {}
    }

    if (name.size() >= 2 && name[0] == 'F')
    {
        try
        {
            int n = std::stoi(name.substr(1));
            if (n >= 1 && n <= 24) return 0x6F + n; // VK_F1=0x70
        }
        catch (...) {}
    }

    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
        return static_cast<uint32_t>(name[0]);
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
        return static_cast<uint32_t>(name[0]);

    static const std::pair<const char *, uint32_t> named[] = {
        {"SPACE", 0x20}, {"ESCAPE", 0x1B}, {"ESC", 0x1B},
        {"TAB", 0x09}, {"ENTER", 0x0D}, {"RETURN", 0x0D}, {"BACKSPACE", 0x08},
        {"HOME", 0x24}, {"END", 0x23},
        {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"INSERT", 0x2D}, {"DELETE", 0x2E},
        {"UP", 0x26}, {"DOWN", 0x28}, {"LEFT", 0x25}, {"RIGHT", 0x27},
    };
    for (auto &p : named)
        if (name == p.first) return p.second;
    return 0;
}

// Inverse of parse_vk_code: a name parse_vk_code accepts (or "0xNN" fallback).
std::string goblin::format_vk_code(uint32_t vk)
{
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x6F); // F1..F24
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, static_cast<char>(vk));
    switch (vk)
    {
    case 0x20: return "Space";
    case 0x1B: return "Escape";
    case 0x09: return "Tab";
    case 0x0D: return "Enter";
    case 0x08: return "Backspace";
    case 0x24: return "Home";
    case 0x23: return "End";
    case 0x21: return "PageUp";
    case 0x22: return "PageDown";
    case 0x2D: return "Insert";
    case 0x2E: return "Delete";
    case 0x26: return "Up";
    case 0x28: return "Down";
    case 0x25: return "Left";
    case 0x27: return "Right";
    default: { char b[8]; std::snprintf(b, sizeof b, "0x%02X", vk); return b; }
    }
}

// Inverse of parse_gamepad_combo: tokens joined with '+', in a stable order.
std::string goblin::format_gamepad_combo(uint16_t mask)
{
    static const std::pair<uint16_t, const char *> order[] = {
        {0x8000, "Y"}, {0x4000, "X"}, {0x1000, "A"}, {0x2000, "B"},
        {0x0100, "LB"}, {0x0200, "RB"}, {0x0040, "L3"}, {0x0080, "R3"},
        {0x0020, "BACK"}, {0x0010, "START"},
        {0x0001, "UP"}, {0x0002, "DOWN"}, {0x0004, "LEFT"}, {0x0008, "RIGHT"},
    };
    std::string out;
    for (auto &p : order)
        if (mask & p.first) { if (!out.empty()) out += '+'; out += p.second; }
    // Spell an unbound pad button rather than writing a blank, which reads as a value someone
    // forgot to fill in. The parser accepts this back as "no button".
    return out.empty() ? std::string("none") : out;
}
