#pragma once
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
}
