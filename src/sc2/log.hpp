#pragma once

// sc2 backend (ported from CustomTalismanEffects / QuestPath backend v2) logging
// shim. The portable core calls cte::flog / cte::log_line; here we back them by
// MapForGoblins' existing spdlog sink instead of a second log file, and expose
// the handful of cte:: globals the core expects. Keep the API identical to the
// upstream log.hpp so the ported files compile unchanged.

#define WIN32_LEAN_AND_MEAN
// NOMINMAX matters here, not just tidiness: the backend's own translation units define it before
// including Windows.h, and this header is now pulled into them too. Without it the min/max macros
// leak in and break every std::numeric_limits<...>::max() in the frame ring.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>

namespace cte {

extern HINSTANCE g_hinst; // set in sc2 bootstrap; used to locate the DLL's folder
extern bool      g_debug; // unused here (kept for API parity)

std::wstring config_path();
std::wstring state_path();

void log_line(const std::string& msg, bool truncate = false);
void flog(const char* fmt, ...);

// Report an exception that a `catch (...)` is about to swallow. MUST be called from inside the
// catch block: it rethrows internally to read the in-flight exception's type. Every silent handler
// in this backend now calls it, because the alternative is what a player report looked like - a
// health flag flapping with no cause anywhere in the log. Rate-limited per call site.
void note_swallowed(const char* file, int line);

} // namespace cte
