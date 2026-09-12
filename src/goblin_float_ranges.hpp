#pragma once

#include <cstring>

namespace goblin
{
    // The editable range of every Float setting a menu offers, in ONE table read by both menus (the
    // overlay's sliders and the in-game menu's), so the two can never disagree about the limits of
    // one setting. A Float key absent from here is shown read-only (its value, no control) - the
    // overlay window geometry, for one, is saved on close and not meant to be dragged.
    //
    // The location-emphasis limits are exactly the clamps the marker code applies to these values
    // (goblin_stall_probe.cpp v3_emphasis / v3_fade / v3_cool), so every slider stop still changes
    // what is drawn. They showed as key-rebind rows on the overlay until 2026-09-11: its row code had
    // no Float branch at all and everything unknown fell into the key/pad-combo branch.
    struct FloatRange
    {
        const char *key;
        float min;
        float max;
        float step;           // one stop of the in-game menu's slider
        const char *format;   // the overlay slider's printf format
        const wchar_t *suffix; // printed after the number in the in-game menu's value column
    };

    inline constexpr FloatRange kFloatRanges[] = {
        {"overlay_font_scale", 0.80f, 3.00f, 0.10f, "%.2fx", L""},
        {"overlay_opacity", 0.30f, 1.00f, 0.05f, "%.2f", L""},
        // Map panel position, in percent: 0 = centre of the map area, 100 = the corner the panels
        // were authored for, above 100 = further left. Step 10 gives 26 stops over -50..200, which
        // is about 69 stage units each - fine enough to place it, and a held arrow sweeps the whole
        // range in ~2.5 s at the repeat rate.
        {"map_panel_offset_percent", -50.0f, 200.0f, 10.0f, "%.0f%%", L"%"},
        {"location_emphasis_own_scale", 0.35f, 2.50f, 0.05f, "%.2fx", L""},
        {"location_emphasis_other_scale", 0.35f, 2.50f, 0.05f, "%.2fx", L""},
        {"location_emphasis_other_fade", 0.20f, 1.00f, 0.05f, "%.2f", L""},
        {"location_emphasis_other_cool", 0.00f, 1.00f, 0.05f, "%.2f", L""},
    };

    inline const FloatRange *float_range(const char *key)
    {
        if (!key)
            return nullptr;
        for (const auto &r : kFloatRanges)
            if (std::strcmp(r.key, key) == 0)
                return &r;
        return nullptr;
    }
}
