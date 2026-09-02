#pragma once

#include <cstdint>

// In-game config overlay (Dear ImGui in a SEPARATE transparent top-most window
// with its own D3D11 + DirectComposition device, rendered on its own thread). It
// does NOT touch the game's swapchain, so it is compatible with tools that wrap
// the game's swapchain (Special K, NVIDIA Smooth Motion / frame-gen, ReShade).
// Lets the player open a settings panel from inside the running game and flip the
// show_* / other options; changes auto-save to MapForGoblins.ini on close and
// auto-reload on open. Open/close: the configured toggle key (default F10) or ESC.
namespace goblin::overlay
{
    // Spawn the dedicated overlay thread (creates the window + D3D11 + DComp +
    // ImGui and runs the render loop). Returns immediately. Safe no-op (logs) if
    // menu_enabled is false or window/D3D creation fails. Call from setup_mod.
    void setup();

    // True while Win32 virtual-key `vk` is currently held. Reads GetAsyncKeyState
    // directly (foreground-only). Used by the marker-dump + icon master-toggle
    // hotkey loops in goblin_markers / goblin_inject.
    bool key_down(int vk);

    // True while ALL buttons in `mask` (an XINPUT_GAMEPAD_* bitmask) are held on the
    // active pad. Reads the overlay's polled pad state (updated each render frame), so it
    // needs the pad poll running, which setup() starts in every mode. Used by the manual
    // marker-hide loop.
    bool gamepad_mask_down(uint16_t mask);
    // The whole button set currently held on the real pad (0 when none/no pad). For callers that
    // must discover a combo rather than test a known one - the menu's rebind page.
    uint16_t gamepad_buttons();

    // Live WorldMapPointParam row of the V3 native marker projecting nearest the
    // map reticle (screen centre), or nullptr. Markers migrated off the engine
    // pin pipeline have no pin, so the game's hover routine can never report
    // them - this is the equivalent focus test. Thread-safe (no ImGui state);
    // also used by the manual-hide hotkey thread.
    void *native_hover_row();

    // Text capture for the NATIVE menu's search page. While on, the raw-input hook that mutes
    // the game's keyboard during the overlay menu mutes only the TYPEABLE keys (letters, digits,
    // space, punctuation, Backspace), so the typed letters stop being the game's own menu keys
    // (E, Q, WASD...) while Enter, Escape and the arrows still drive the native screen. The
    // native-menu host sets it while the cursor sits on the search page's text row.
    void set_text_capture(bool on);
    // Is this virtual key one the text capture claims? Shared by both text feeds.
    bool text_key(int vk);

    // The keyboard layout the text feeds translate with. The game's window never handles the
    // OS layout-switch request, so its thread stays on whatever layout it started with (seen
    // 2026-09-02: 0x04090409 on both threads after Alt+Shift). So the switch is ours: poll()
    // edge-detects Alt+Shift / Ctrl+Shift / Win+Space and cycles through the installed layouts;
    // layout() is the current pick (seeded from the game window's thread); tag() is its
    // two-letter language code ("EN", "RU") for the text row. Call poll() only while typing.
    bool text_layout_poll();  // true when this call switched the layout
    void *text_layout();  // an HKL (void* keeps <windows.h> out of this header)
    const wchar_t *text_layout_tag();
}
