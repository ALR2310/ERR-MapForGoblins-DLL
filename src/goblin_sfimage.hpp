#pragma once

// Drawing OUR OWN pixels into ANY menu clip, without touching a single byte of the game's
// (or another mod's) movies.
//
// The engine is stock Scaleform GFx, so the path is the SDK's own:
//   Render::RawImage::Create  - an image that lives in system memory, pixels writable
//   CS::ScaleformImageResource - the engine's resource wrapper around any Render::Image
//   drawImageInto              - fills a clip's drawing context with that resource
// Nothing here registers anything in a movie's character dictionary, so it works in every
// movie and survives overhaul mods and game updates alike (only our own addresses can move,
// and those are covered by tools/rva_anchors.py).
//
// Threading: menu/UI thread only - the repository and drawing contexts have no locks.
//
// NOT COMPILED as of 2026-07-31 (goblin_sfimage.cpp is commented out of CMakeLists.txt): the last
// external caller of create_resource() went with the own-draw icon routes. To build it back in,
// restore the two source lines in CMakeLists.txt and re-add `#include "goblin_sfimage.hpp"` to
// whichever file starts calling it.

#include <cstddef>
#include <cstdint>

namespace goblin::sfimage
{
    // Is the machinery usable (addresses resolved, heap reachable)?
    bool available();

    // Build a resource from straight (non-premultiplied is fine) RGBA8 pixels. `name` is
    // only used for the resource's own label and our logs. Returns an opaque handle that
    // stays alive for the process, or nullptr on failure. Cache the result - creating one
    // per frame would leak.
    void *create_resource(const wchar_t *name, int width, int height, const uint8_t *rgba);

    // REMOVED 2026-07-30, with the drawing half of the .cpp: draw_into(), ensure_child_clip(),
    // the IconState enum and its accessors (draw_failure / last_value_type / icon_state /
    // set_icon_state / icon_state_name). They served the own-icon path, which was permanently
    // disabled (kEnableOwnIconPath = false) when the row icons moved to being spliced into the
    // menu movie, and they reported their state to an overlay Tools page retired on 2026-07-28.
    // The hard-won details about the engine's addref, the CreateEmptyMovieClip out-buffer and
    // the vtable-slot lookup are kept as comments at their old sites in the .cpp.
}
