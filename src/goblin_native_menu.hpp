#pragma once

// The MODEL behind the in-game (native) mod menu: pages, rows, navigation and value
// formatting. Deliberately free of any game-engine detail - the hook layer in
// goblin_stall_probe.cpp turns these rows into native list items, draws them and feeds
// input back in. Keeping the two apart is what lets the same model serve our own
// settings, extra screens (progress / hidden markers / actions) and - later - pages
// registered by OTHER mods through a stable C ABI.
//
// Row text is owned by the model (a string arena that is stable for as long as the
// current page lives), so the draw hook only ever reads `const wchar_t *`.

#include <cstddef>
#include <cstdint>

namespace goblin::nmenu
{
    enum class RowKind : uint8_t
    {
        Back,        // "< Back" - go up one level
        SubPage,     // opens another page in place
        Toggle,      // bool config value, decide flips it
        Number,      // numeric config value, decide opens its value page
        Enum,        // value from a fixed list, decide opens its value page
        ValueOption, // one choice ON a value page; decide applies it and returns
        Rebind,      // hotkey, decide opens the "press a key" page
        Action,      // runs something once
        Info,        // read-only text
        Progress,    // read-only text + a bar drawn from collected/total
    };

    struct Row
    {
        RowKind kind = RowKind::Info;
        const wchar_t *label = L"";
        const wchar_t *value = L"";
        int32_t page_id = -1;     // SubPage target
        const char *ini_key = nullptr;
        void *target = nullptr;   // Toggle: bool*, Number: float*/uint8_t*, Enum: std::string*
        uint8_t type_tag = 0;     // goblin::IniType as u8, for Number/Enum
        int32_t collected = 0;    // Progress
        int32_t total = 0;        // Progress
        void (*action)() = nullptr;
        uint32_t addon_row_id = 0; // rows contributed by another mod (type_tag 0xFF)
        int32_t addon_kind = 0;
        // Source icon id for this row (-1 = none). Resolved from the ini key through the
        // shared icon atlas mapping, so it is ready for whichever draw path lands
        // (native image clip or a spliced icon sprite).
        int32_t icon_id = -1;
        // Long description shown in the screen's help line (the overlay's tooltip text).
        const wchar_t *help = nullptr;
        // Show the row's red plate. It is the screen's key-CONFLICT marker (the Conflict clip,
        // normally hidden by us), which makes it the one real background fill a row has - HTML
        // in the text fields cannot paint one.
        bool plate = false;
        // Rows destined for the form's RIGHT column (a preview of the highlighted section).
        // Those clips only have a value field, so their text goes in `value`.
        bool right_column = false;
    };

    // Page ids: the root plus the fixed extra screens. Sections of the ini schema get
    // ids kPageSectionBase + <section index>, so a new ini section needs no code here.
    constexpr int32_t kPageRoot = 0;
    constexpr int32_t kPageProgress = 1;
    constexpr int32_t kPageHidden = 2;
    constexpr int32_t kPageActions = 3;
    // The list of choices for one Number/Enum entry, and the "press a key" screen for one
    // hotkey entry. Both edit a single entry, remembered by the model while the page is up,
    // so one page id each is enough.
    constexpr int32_t kPageValue = 4;
    constexpr int32_t kPageRebind = 5;
    constexpr int32_t kPageSectionBase = 100;
    // Pages contributed by other mods through the SDK occupy [kPageAddonBase, kPageRegionBase).
    constexpr int32_t kPageAddonBase = 1000;
    // One page per progress region (kPageRegionBase + index into progress::snapshot()),
    // showing that region's per-category breakdown. MUST stay above the add-on range: the
    // dispatch below tests the add-on range last, so anything inside it is claimed by it.
    constexpr int32_t kPageRegionBase = 4000;

    // Rebuild the rows of the current page from live config/state. Call after anything
    // that changes a value, and once when the screen opens.
    void rebuild();

    // Current page contents. The pointers stay valid until the next rebuild().
    const Row *rows(size_t *count);
    const wchar_t *page_title();
    int32_t current_page();

    // NESTED-SCREEN mode: one page per real screen, so Back/Esc/scroll-restore come from the
    // engine instead of being emulated. The host tells the model which page a screen shows, and
    // asks what a row would open without activating it.
    void set_page(int32_t page);
    int32_t subpage_target(size_t row_index);

    // SCREEN-PER-PAGE mode. With it on the model never navigates on its own: activate() only
    // PREPARES a transition (remembering which entry a value/rebind page will edit) and reports
    // it, so the host can open or close a real native screen and keep the engine's own Back,
    // Esc and scroll restore. The page a screen shows is set by the host through set_page().
    void set_nested(bool on);
    bool nested();
    // The page the last activate() wants opened as its OWN screen, or -1. Reading it clears it.
    int32_t take_child_page();
    // The last activate() finished an edit (a value was chosen, a key was bound, a "Back" row
    // was confirmed): the host should close the screen showing it. Reading it clears it.
    bool take_close_request();

    // Enter the screen at the root page (called when our screen opens).
    void reset_to_root();

    // Handle "decide" on a row: navigates, toggles, steps or runs the action. Returns
    // true if the row list changed (the caller must rebuild the native view).
    bool activate(size_t row_index);

    // Go up one level. Returns false when already at the root (the caller should then
    // let the screen close normally).
    bool navigate_back();

    // How many levels down we are (0 = root). The host uses it to tell "went deeper" from
    // "came back", so it can park the list at the top of a new page and restore the previous
    // position on the way out.
    size_t depth();

    // Rows are handed to the engine in PAIRS (left column, right column). This returns the
    // right-column row for pair `index`, or nullptr when that half should stay empty.
    const Row *right_row(size_t index);

    // Was anything changed that the ini should be saved for?
    bool dirty();
    void clear_dirty();

    // ── hotkey capture ───────────────────────────────────────────────────────────────
    // The "press a key" page has no input of its own: the screen host polls for a key
    // while this is true and hands the result back. Keeping the polling in the host keeps
    // the model free of Windows input, and lets the same page serve a gamepad combo later.
    bool rebind_pending();
    // Apply the captured virtual-key code (0 = the player cancelled) and leave the page.
    void rebind_apply(uint32_t vk);

    // Progress-bar glyph set. Which of these actually renders depends on the menu font,
    // so the first in-game run shows all of them side by side and we keep the winner.
    enum class BarStyle : uint8_t
    {
        Ascii,  // [####----]
        Blocks, // [****....] using U+2588 / U+2591
        Both,   // both, for the one-off comparison run
    };
    void set_bar_style(BarStyle style);
    BarStyle bar_style();

    // Experiment: draw the progress bar as a SCALED CLIP in the row instead of text.
    // Uses the row's own Conflict sprite (the only spare graphic in the 02_160 row clip)
    // with setScale/setColor - so it needs no new movie assets.
    bool graphic_bar();
}
