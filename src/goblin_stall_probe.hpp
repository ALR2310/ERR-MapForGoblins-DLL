#pragma once
#include <cstdint>
// Debug-only stall sampler. When debug_logging is on, capture() records the
// calling game thread and samples its instruction pointer + stack from a helper
// thread for a short window, then logs exe+RVA histograms. Used to attribute
// the map-open build cost and the deferred post-close heap-release stall to
// their exe-side call sites (data for the next optimization round).
// No-op (one flag check) when debug_logging is off or a capture is running.
namespace goblin::stall_probe
{
    // Arm the pin-registration cost counters: pass-through wraps (AOB-resolved) on
    // the three per-marker map-widget virtual methods + the typed-find walk they
    // drive - the residual per-open pin-construction cost. The wraps only count
    // (calls / time / real return addresses) while a capture window is active;
    // otherwise they are a flag check + tail call. Call once before enable_hooks().
    void setup();

    // Sample the CALLING thread for duration_ms. tag names the capture in the log.
    // Also opens the counter window above; both are logged when the window closes.
    void capture(const char *tag, unsigned duration_ms);

    // Debug V3 visual experiment: compensate the transplanted child's local scale
    // once per map frame so it keeps the same on-screen size while the map zooms.
    void on_map_frame();

    // Called synchronously from a live RemoveObject2::Execute callback. Consumes
    // a bounded part of the native-marker queue while the supplied timeline ctx
    // is guaranteed alive; never caches ctx beyond this call.
    void v3_native_factory_pulse(void *ctx, unsigned frame);

    // Does a world-map screen exist right now? Read from the ENGINE's own per-menu lifecycle
    // byte (sampled in CSMenuMan::updateTask), so it is true from the moment the dialog is
    // created - unlike maphover::map_dialog(), which only becomes non-null once the engine has
    // called our hover hook at least once, i.e. AFTER the marker build burst has already run.
    bool map_screen_alive();

    // Called from the DLL's background watcher: when the map-frame heartbeat has been
    // silent >700 ms while the map phase says a dialog exists, capture and log the map
    // thread's stack (module+offset) so a multi-second open freeze names its culprit.
    void sample_map_stall();

    // Debug V3 correlation: bracket the game's native buildMarkers call. Any
    // Scaleform attachMovie operations observed inside the bracket are grouped
    // by their actual display-list parent and logged at the end of the build.
    void v3_pin_build_begin(void *owner, void *ctx);
    void v3_pin_build_end();

    // Forget map-owned pointers and re-arm the one-shot attach for the next map.
    // Called before the WorldMapDialog's normal teardown destroys its display tree.
    void on_map_close();

    // Lever C: bulk-detach every native marker child we attached, using the engine's
    // own remove-from-container primitive, BEFORE the WorldMapDialog dtor runs its
    // blocking close-teardown. Must be called on the map UI thread while the display
    // tree is still alive (i.e. from the WMD dtor detour before the original dtor).
    // Returns the number of children detached. No-op unless variants::kSelfDetach.
    uint32_t v3_detach_all_children();
}
