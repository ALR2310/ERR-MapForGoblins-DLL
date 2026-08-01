// sc2 backend logging shim implementation: route the ported core's cte::flog /
// cte::log_line into MapForGoblins' spdlog so there is ONE log, and provide the
// cte:: globals the core references. config_path/state_path are only used by the
// upstream frontend (not ported); they return empty and are harmless here.

#include "log.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

namespace cte {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

std::wstring config_path() { return std::wstring(); }
std::wstring state_path() { return std::wstring(); }

void log_line(const std::string& msg, bool /*truncate*/)
{
    spdlog::info("[SC2] {}", msg);
}

void note_swallowed(const char* file, int line)
{
    const char* what = "non-std exception";
    try
    {
        throw; // rethrow the exception the caller's catch block is holding
    }
    catch (const std::exception& e)
    {
        what = e.what();
    }
    catch (...)
    {
        // Deliberately silent: this IS the reporter, and it must never report itself.
    }
    // Keep only the file name, and rate-limit per site: a fault on a per-frame path would
    // otherwise fill the log and hide everything else.
    const char* slash = std::strrchr(file, '\\');
    if (!slash)
        slash = std::strrchr(file, '/');
    const char* name = slash ? slash + 1 : file;
    static std::atomic<int> counts[128]{};
    const int slot = ((line * 31) ^ static_cast<int>(name[0])) & 127;
    const int n = counts[slot].fetch_add(1, std::memory_order_relaxed);
    if (n < 3 || (n % 200) == 0)
        spdlog::warn("[SC2] swallowed exception at {}:{} (occurrence {}): {}", name, line, n + 1,
                     what);
}

void flog(const char* fmt, ...)
{
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    va_list probe;
    va_copy(probe, args);
    const int need = std::vsnprintf(nullptr, 0, fmt, probe);
    va_end(probe);
    if (need <= 0) { va_end(args); return; }
    std::vector<char> buf(static_cast<size_t>(need) + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);
    spdlog::info("[SC2] {}", buf.data());
}

} // namespace cte
