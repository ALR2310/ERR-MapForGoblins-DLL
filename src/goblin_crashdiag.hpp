#pragma once
// Crash-diagnosis instrumentation, added 2026-08-03 on top of clean 2.1.1.
//
// Why it exists: five sessions of crash reports had been argued without anyone ever MEASURING the
// thing under argument. Report 19 is the example - a whole theory ("map opens accumulate until the
// GFx allocator refuses") rested on a correlation, and could not be tested because there was no
// instrument, while the crash that actually killed the process was never recorded at all: the flat
// twelve-record budget in the crash logger had already been spent on a handled fault.
//
// Everything here reads and logs, or records state for the crash writer to print. Nothing changes
// what the mod does.
//
// `debug_logging` gates only the two sampling probes, and only because they cost time - never
// because they alter behaviour. The counters are plain stores of numbers the code already has and
// are ALWAYS on, because the crash writer needs them exactly in the runs where nobody thought to
// switch diagnostics on beforehand.
//
// What each probe decides:
//   memory()    - does anything actually accumulate per map open? A commit charge climbing by tens
//                 of MB per open says yes and gives the slope; a flat one ends the leak theory.
//   survivors() - do an abandoned generation's children still exist once the next generation is
//                 built? That settles both "is the leak real" and "would releasing them be a write
//                 into memory the engine has already taken back".
#include <cstddef>
#include <cstdint>

namespace goblin::crashdiag
{
    // ── live state, read by the crash writer ────────────────────────────────────────────────────
    // Plain atomics, so a handler on another thread can read them without a lock and without
    // trusting any of our containers to still be intact.
    void note_map_open_completed(int layer, uint32_t created, uint32_t failed, uint32_t tracked);
    void note_map_closed(uint32_t tracked);
    void note_generation(uint64_t parent, uint64_t wrapper);

    // One "[state] k=v ..." line into a caller-supplied buffer. No heap, no CRT, safe to call from
    // an exception handler. Returns bytes written.
    int format_state(char *buf, int cap);

    // ── which game build we are running in ──────────────────────────────────────────────────────
    // Report 33 cost a dump read just to learn that it was exe 2.6.2.0: our own log said nothing
    // about the game at all, and the reporter had trimmed the [SESSION] banner away, so even the
    // mod build was unknown from the log alone. Resolved ONCE on a normal thread at init and only
    // read afterwards, so the crash writer formats cached numbers and never touches the PE image
    // or a resource inside a handler.
    //
    // FileVersion is not enough on its own: a downpatched or repacked exe reports the same string
    // as the stock one. SizeOfImage and TimeDateStamp are what actually identify a build - they
    // are the pair the anchor resolver's behaviour follows.
    void resolve_game_build();
    uint64_t game_version();    // ms<<32 | ls, i.e. 2.6.2.0 -> 0x0002000600020000; 0 = unknown
    uint32_t game_image_size(); // SizeOfImage of the running exe
    uint32_t game_timestamp();  // its PE TimeDateStamp

    // One "  [env] ..." line: this mod's version/profile/commit and the game build above. No heap,
    // no CRT, safe from an exception handler. Written into EVERY crash record on purpose - reports
    // arrive hand-trimmed to the interesting record, and a record that cannot say which two
    // binaries produced it is not actionable. Returns bytes written.
    int format_env(char *buf, int cap);

    // ── probe 1: does anything accumulate? ──────────────────────────────────────────────────────
    // Commit charge and working set for this process; `tag` names the moment ("open" / "close").
    // Resolved through K32GetProcessMemoryInfo so the import table does not change. debug_logging.
    void memory(const char *tag, uint32_t tracked);

    // ── probe 3: the Scaleform arena, the thing that actually runs out ──────────────────────────
    // Measured in the 11.84 GB full-memory dump: every Scaleform allocation in the process comes
    // out of ONE pre-committed region of 167,770,368 bytes (160.00 MiB) at 0x7FF38C0E0000. That is
    // why probe 1 was the wrong instrument - the region is committed up front, so filling it never
    // moves the process's commit charge, and a flat commit graph was read as "nothing accumulates"
    // when the arena was in fact 82.15% full with seven abandoned marker generations in it.
    //
    // When the arena cannot satisfy a request it returns 0, and the engine stores that 0 without
    // checking: two independent sites did exactly that within one second. The Scaleform "limit" is
    // not involved - its handler overwrites its own answer and lets the allocation through.
    //
    // So this is the number that decides whether the close-time release actually WORKS: `free` must
    // stop falling across opens. Read-only, resolved once, and silently disabled if the layout does
    // not validate. debug_logging.
    void arena(const char *tag);

    // ── probe 2: do abandoned children survive their generation? ────────────────────────────────
    // Called as a generation is thrown away: keeps a spread sample of its child addresses and the
    // vtable they are supposed to carry. A copy of numbers we already hold. debug_logging.
    // `total` is the generation's real size (the sample is at most 16 of it) and `site` names the
    // reset path that abandoned it - the graveyard's "0 destroyed, 7136 refused" says the children
    // are not always in the state the 2.1.1 sample found, and the caller is the obvious suspect.
    void sample_generation(uint64_t child_vtable, const uintptr_t *children, size_t count,
                           size_t total, const char *site);

    // Called once the NEXT generation is fully built, i.e. after the old movie has certainly been
    // torn down: re-reads the sampled addresses and reports per address whether the page is still
    // committed, whether the first qword is still that vtable, and what the reference count reads.
    // Read-only and SEH-guarded throughout. debug_logging.
    void probe_survivors();
}
