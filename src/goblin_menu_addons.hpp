#pragma once

// Add-on pages: lets OTHER mod DLLs contribute their own page of rows to our in-game
// menu (see sdk/mfg_menu_api.h for the client-side contract).
//
// Discovery is host-pull: the client DLL exports `MfgMenuAddonInit`, and we walk the
// loaded modules looking for it. That keeps OUR module export-free (its clean PE
// profile matters for antivirus heuristics) and removes any load-order race - we simply
// re-scan before the menu opens, so a mod that loaded after us is still picked up.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace goblin::addons
{
    // One row as contributed by an add-on, already copied into our own storage.
    struct Row
    {
        int32_t kind = 0; // mfg_row_kind
        std::wstring label;
        std::wstring value;
        int32_t done = 0;
        int32_t total = 0;
        uint32_t row_id = 0;
    };

    struct Page
    {
        std::wstring title;
        std::wstring owner; // module file name, for logs
        std::vector<Row> rows;
    };

    // Scan the process's modules for add-ons we have not talked to yet and let them
    // register. Cheap; safe to call every time the menu opens.
    void scan();

    // Pages currently registered, in registration order.
    size_t page_count();
    const Page *page(size_t index);

    // Ask the owning add-on to refresh a page's rows (its on_build callback), then
    // return the page. Returns nullptr if the index is out of range.
    const Page *build_page(size_t index);

    // Confirm was pressed on a row of a page. Returns true if the add-on reports it
    // changed something (the menu then rebuilds).
    bool activate(size_t page_index, uint32_t row_id);

    // Set when an add-on asked for a redraw outside of a callback.
    //
    // NOT POLLED BY ANYTHING (verified 2026-07-30): the flag is set and never taken, so
    // api_request_redraw() in the published API table is a call the host accepts and ignores. That
    // stopped being a live problem the same day - this whole host is out of the map mod's build
    // (MFG_MENU_ADDON_HOST = 0), so the DLL no longer offers the table to anyone - but it is the
    // first thing to settle in the separate SDK mod, BEFORE any add-on author sees the header.
    //
    // Wiring it up is not a one-line change. The poll has to sit somewhere that may rebuild the
    // row pool, and the obvious candidate - the per-frame form_update_detour - runs inside the
    // engine's own dialog update, where rebuilding pools underneath it is exactly the shape of
    // crash this codebase has hit before. An attempt on 2026-07-30 was reverted for that reason.
    bool take_redraw_request();
}
