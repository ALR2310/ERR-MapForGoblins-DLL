#pragma once

#include <cstdint>
#include <string>

namespace goblin::map_timing
{
    // Open-latency probe, read by stall_probe when it prints [opentime] at seed READY.
    // t0 = GetTickCount64() of the first map-site relayout call after >2 s of silence
    // (a map (re)open or in-place rebuild); 0 when no burst has been seen yet.
    uint64_t open_burst_t0();
    // Counters since that t0: relayout calls arriving from the map's own call site
    // (and how many Patch D skipped) versus calls from any OTHER return address - a
    // non-zero "other" during an open is a relayout path the skip does not cover.
    void open_counters(uint64_t &site, uint64_t &skipped, uint64_t &other);
    // The other-site calls broken down by return address, formatted for the [opentime]
    // line ("exe+0xNNN xCOUNT, ..."), most frequent first; "none" when the burst saw none.
    std::string open_other_sites();

    // World-map open optimization (variants::kFastMapOpen). Hooks the per-marker
    // relayout at the map's dispatcher call site (AOB-resolved) and skips it there
    // unconditionally (field-proven "Patch D"; markers render correctly without it).
    // The defer-and-replay variant crashed and is permanently removed - see the
    // history note in goblin_map_timing.cpp. No-op when the fast-open variant is off.
    void setup();

    // Called once per frame on the game UI thread while the world map is open
    // (from maphover's per-frame hook). Currently no-op; kept as a stable entry
    // point for future frame-driven logic.
    void on_map_frame();
}
