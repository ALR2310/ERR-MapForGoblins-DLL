#pragma once
// "We asked for this fault and we handle it."
//
// Parts of the mod deliberately try things that can fail inside the ENGINE: ask a movie for a clip by
// path that may not be there, drive a primitive on an object that may not be the class we think. Those
// calls sit in an SEH frame and the failure is the answer, not a defect.
//
// The crash logger is a vectored handler, so it sees every one of those first-chance faults BEFORE the
// frame that handles them - and it used to record each as a crash. Measured 2026-07-30 on ERR: twelve
// "CRASH" records in one session with the game perfectly alive (six inside the Scaleform proxy
// destructor, six in the name resolver), which is exactly the noise a real crash then hides in.
//
// So a call of that kind raises the counter first. dllmain's handler records nothing while it is up.
// Kept as a plain int rather than a scope object on purpose: MSVC will not compile a function that has
// both __try and an object needing unwinding, and these call sites are all __try.
//
//     ++goblin::guarded::depth;
//     __try { ...engine call... } __except (EXCEPTION_EXECUTE_HANDLER) { }
//     --goblin::guarded::depth;   // reached on both paths
//
// It is per-thread and it must stay narrow: anything that faults while it is up goes unrecorded, so
// raise it around the engine call and nothing more.
namespace goblin::guarded
{
    inline thread_local int depth = 0;

    inline bool inside() { return depth > 0; }
}
