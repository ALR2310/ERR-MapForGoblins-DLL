# MapForGoblins in-game menu SDK

Add your own settings page to the MapForGoblins in-game menu, so players configure your
mod from the same screen the game itself draws over the world map - no overlay, no extra
window, controller-friendly.

Status: **ABI v1, preview.** The menu itself is still behind a dev gate while the layout
is being finished, so treat the API as usable but not yet frozen.

## How it works

1. Copy `mfg_menu_api.h` into your project.
2. Export one function from your DLL:

```cpp
#include "mfg_menu_api.h"

static bool g_feature_on = true;
static const mfg_menu_api *g_api = nullptr;
static mfg_page g_page = nullptr;

static void build(void *user, mfg_page page)
{
    mfg_row rows[3] = {};
    rows[0].kind    = MFG_ROW_TOGGLE;
    rows[0].label   = L"Enable my feature";
    rows[0].value   = g_feature_on ? L"On" : L"Off";
    rows[0].row_id  = 1;

    rows[1].kind    = MFG_ROW_PROGRESS;
    rows[1].label   = L"Things collected";
    rows[1].done    = 7;
    rows[1].total   = 12;
    rows[1].row_id  = 2;

    rows[2].kind    = MFG_ROW_ACTION;
    rows[2].label   = L"Reset my counters";
    rows[2].row_id  = 3;

    g_api->set_rows(page, rows, 3);
}

static int32_t activate(void *user, uint32_t row_id)
{
    if (row_id == 1) { g_feature_on = !g_feature_on; return 1; } /* 1 = we changed something */
    if (row_id == 3) { /* reset */ return 1; }
    return 0;
}

extern "C" __declspec(dllexport) int MfgMenuAddonInit(const mfg_menu_api *api)
{
    if (!api || api->abi_version < 1) return 0;      /* decline politely */
    g_api  = api;
    g_page = api->add_page(L"My Mod", nullptr);
    if (!g_page) return 0;
    api->set_on_build(g_page, &build);
    api->set_on_activate(g_page, &activate);
    return 1;                                        /* accepted */
}
```

That is all. MapForGoblins walks the loaded modules (at its own start and again each time
the menu opens), finds `MfgMenuAddonInit`, and your page shows up as a row in the menu
root. Load order does not matter.

## Rules that keep everyone safe

* **Never block.** Callbacks run on the game's UI thread, one at a time.
* Strings are **copied immediately**, so temporaries are fine. Labels are capped at 128
  characters and a page at 128 rows.
* Return `1` from `activate` only when something actually changed - that is what triggers
  a rebuild and redraw.
* Everything you hand over is called behind an error boundary. A page whose callbacks
  keep faulting is disabled rather than allowed to take the game down.
* Your module is pinned once a page is accepted, because the menu holds pointers into
  your code.

## Formatting

Row text is drawn into the game's own HTML-capable text fields, so simple markup works in
`label` and `value`:

```
L"<font color=\"#7FD97F\">On</font>"
```

Keep it to `<font color>`, `<b>`, `<i>`. Note that the game's menu font does not cover
every script, so prefer plain characters for anything that must render in all languages.

## Roadmap

Planned for v2: real slider/combo widgets (the game's own settings-page controls), an
icon per row, per-row help text, and a manifest form for mods that would rather ship a
config file than code.
