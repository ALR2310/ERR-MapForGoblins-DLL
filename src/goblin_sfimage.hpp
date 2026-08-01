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

    // Draw a previously created resource into a resolved clip proxy, scaled to fit
    // width x height pixels at (x, y) in the clip's OWN coordinates. Returns false if the
    // clip cannot host a drawing. A zero width or height clears the clip's drawing context
    // without putting anything in it, which is how a row with no icon is blanked.
    bool draw_into(void *clip_proxy, void *resource, float x, float y, float width,
                   float height);

    // How far the row-icon path got the last time a row with an icon was drawn. Reported on
    // the Tools page so a broken step is visible in game without reading the log.
    enum class IconState
    {
        Untried,  // no icon row drawn yet
        NoImage,  // the pixels could not be turned into a resource
        NoClip,   // no clip to draw into (child creation refused and no spare clip)
        NoDraw,   // the clip exists but the engine refused the drawing
        Drawn,    // pixels are on screen
    };
    // Which step refused the last drawing, and the value type word the clip reported.
    const char *draw_failure();
    uint32_t last_value_type();

    IconState icon_state();
    void set_icon_state(IconState state);
    const char *icon_state_name();

    // Create an empty child clip (a flash.display.Sprite) named `name` at `depth` under the
    // given parent clip proxy, so we have something of our own to draw into. Safe to call
    // repeatedly - it does nothing if the child already exists. Returns false if the engine
    // refused (e.g. the parent is not a display-object container).
    bool ensure_child_clip(void *parent_proxy, const char *name, int32_t depth);
}
