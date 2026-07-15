#pragma once

namespace goblin::map_timing
{
    // World-map open optimization (config::fastMapOpen). Hooks the per-marker
    // relayout at the map's dispatcher call site (AOB-resolved) and skips it there
    // unconditionally (field-proven "Patch D"; markers render correctly without it).
    // The defer-and-replay variant crashed and is permanently removed - see the
    // history note in goblin_map_timing.cpp. No-op when fastMapOpen is off.
    void setup();

    // Called once per frame on the game UI thread while the world map is open
    // (from maphover's per-frame hook). Currently no-op; kept as a stable entry
    // point for future frame-driven logic.
    void on_map_frame();
}
