#pragma once

// The on-screen status line: the game's own small text at the bottom of the screen (the one that
// says "Cannot use this now"), driven from any thread with arbitrary text. ONE slot, so a new
// message REPLACES the one showing (a fast toggle never queues up), no sound, gone after 3 s.
//
// How the engine runs that line (front-end update, 1.17): every frame it takes the message id at
// the read cursor of a six-entry ring in CSFeMan (+0x59A4, cursor +0x59BC); -1 means nothing,
// otherwise it advances the timer at +0x3758 (past 3 s the entry is cleared and the cursor moves
// on) and then REBUILDS the MenuString slot from the id through a builder. Writing the slot
// directly therefore shows nothing - it is overwritten within the frame. What works is putting
// an id of our own into the entry being read, restarting the timer, and answering the builder
// for that id with our text. Field-proven in ReachProbe (2026-09-03) before it moved here.
//
// This replaced the codex toast (a TutorialParam row + TutorialBody.fmg entry per message and
// the popup routine that only runs on the UI thread): that one queued engine-side and needed
// param + FMG expansion for four static strings.
namespace goblin::status_line
{
    // Resolve the front-end pieces and set the builder answer up. Call from init, before the
    // hooks are applied. A miss logs once and leaves show() a no-op.
    void setup();

    // Show `text` now (copied; any thread). No-op until setup() succeeded.
    void show(const wchar_t *text);
}
