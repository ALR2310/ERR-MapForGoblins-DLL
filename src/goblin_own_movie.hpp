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

    // Is the interception armed and usable?
    bool available();

    // Bracket our own screen's open, so a movie load started by anything else (the player
    // opening the real key-binding screen) is never touched.
    void arm();
    void disarm();

    // One-line state for the Tools page and the log: what happened on the last load.
    const char *status();
}
