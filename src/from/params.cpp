#include <chrono>
#include <spdlog/spdlog.h>
#include <thread>

#include "../modutils.hpp"
#include "params.hpp"

using namespace std;

from::params::ParamList **from::params::param_list_address = nullptr;

void from::params::initialize()
{
    param_list_address = modutils::scan<ParamList *>({
        .aob = "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA 90",
        .relative_offsets = {{3, 7}},
    });

    spdlog::info("Waiting for params...");

    constexpr int entry_count = sizeof(ParamList::entries) / sizeof(ParamList::entries[0]);
    // A startup that never finishes leaves this loop as the log's last line, which says nothing about
    // whose fault it is. Report progress instead: on a normal boot the params appear within about ten
    // seconds, so anything past that is worth a line, and the FILLED COUNT distinguishes "the game is
    // still working through them" from "the boot stopped before it created a single one".
    int seconds = 0;
    int reported = -1;

    while (true)
    {
        int filled = 0;
        auto param_list = *param_list_address;
        if (param_list != nullptr)
        {
            for (int i = 0; i < entry_count; i++)
                if (param_list->entries[i].param_res_cap != nullptr)
                    filled++;
            if (filled == entry_count)
            {
                if (seconds > 0)
                    spdlog::info("Params ready after {} s.", seconds);
                return;
            }
        }

        if (seconds >= 15 && seconds % 15 == 0 && filled != reported)
        {
            spdlog::warn("Params still not ready after {} s: {} of {} slots filled{}. The game has not "
                         "finished its own startup yet; nothing of ours runs until it does.",
                         seconds, filled, entry_count,
                         param_list == nullptr ? " (the param list itself does not exist yet)" : "");
            reported = filled;
        }

        this_thread::sleep_for(chrono::seconds(1));
        seconds++;
    }
}
