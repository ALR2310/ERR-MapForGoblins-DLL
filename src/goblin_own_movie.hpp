#pragma once

// Serving OUR OWN version of the menu movie our screen runs on, at LOAD time.
//
// Why at load time: the screen we host our settings on (02_160) is a stock Scaleform movie,
// and everything we still cannot do by poking the live display list - icons as real bitmap
// characters, progress bars as scaling clips, a right-hand panel without the list's button
// plate - is trivial if the movie itself already contains those pieces. Editing the movie
// AFTER the engine parsed it is the route that crashed twice (Scaleform frees buffers it
// believes it owns), so instead we hand the loader a buffer we built and let it parse that
// once, normally.
//
// Where the bytes come from: the build's OWN file, read through the engine's own opener and
// rebuilt in memory. Nothing of the game ships inside the DLL, and whatever an overhaul
// replaced 02_160 with is what we transform. If the rebuild does not come out exactly as
// intended the original bytes are served unchanged, so the worst case is the screen we have
// today rather than a broken one.

#include <cstddef>
#include <cstdint>

namespace goblin::own_movie
{
    // Install the load-time interception. Call once at startup; on failure the feature just
    // stays off and the screen loads the stock movie.
    void install();

    // Is the interception armed and usable? NO CONSUMER at present, like status() below. It reports
    // whether install() found the parse route - NOT whether a transform is armed for some particular
    // open, because there is no such thing; see the next paragraph.
    bool available();

    // THERE IS NO PER-OPEN BRACKET, and one cannot exist at this layer. An arm()/disarm() pair
    // was added on 2026-07-30 to keep the player's own key-binding screen on the stock movie; in
    // game it turned the transform off entirely, because the engine parses 02_160 ONCE during the
    // startup preload and every screen - ours and the player's - instances that single parse. The
    // transform is therefore all-or-nothing per launch. Telling the two screens apart is a
    // per-INSTANCE decision (the live display list), not a per-parse one.

    // THE SEPARATE MOVIE DEFINITION (2026-09-06). The paragraph above still holds for the game's
    // own parse - and that is exactly why our screens no longer use it. The engine keys movie
    // definitions by NAME (CSMenuMan's map, filled by the job's load step) and by URL (Scaleform's
    // SwfCache); a job that asks for a name nobody preloaded misses both caches and goes to the file
    // opener with `menu:/Win/<name>.gfx`. We hook that opener: for menu_movie_name() it hands back
    // the game's own 02_160 file, so the bytes are the stock movie, but the definition the parser
    // builds from them is a NEW one, cached under OUR name and URL. The transform below then runs
    // for that parse only, and the player's Key Assignments screen - and any other mod's key-binding
    // page - instances the untouched preload parse. The job is re-pointed at our name by the opener
    // (goblin_stall_probe open_screen) right after the game's own builder made it: the descriptor
    // is copied into the job at +0x58 {u32 8, u8 kind, wchar_t *name @+0x60}.
    //
    // The name our job asks the engine for. Never the game's own "02_160_KeyConfiguration".
    const wchar_t *menu_movie_name();
    // True once the file-opener route is hooked. Without it a job re-pointed at our name would fail
    // to load, so the caller leaves the game's name in place and the old shared-parse transform
    // applies (bleeding into the player's screen, but working).
    bool separate_movie_armed();

    // One-line state: what happened on the last load. NO CONSUMER at present - the overlay
    // Tools page that read it was retired with the rest of the on-map overlay UI (2026-07-28),
    // and the native menu never picked it up. Kept because g_status is maintained anyway and
    // this is the only way to read it; wire it to a Debug row rather than re-deriving it.
    const char *status();

    // How many row clips the LEFT column of the row pool has once we are done with the movie.
    // The screen ships eleven, spaced 63.75 px apart; we tighten the pitch and add placements, so
    // the host's own slot table has to be sized from the same number rather than a copy of it.
    // Whether the engine actually asks for the added slots is reported by the row-path hook.
    constexpr int kRowSlots = 15;
}
