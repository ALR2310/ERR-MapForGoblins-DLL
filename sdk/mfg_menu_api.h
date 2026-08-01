/* mfg_menu_api.h - MapForGoblins in-game menu SDK, ABI v1.
 *
 * Lets any other Elden Ring mod DLL add its own page of rows to the MapForGoblins
 * in-game menu (the one drawn by the game itself over the world map), so players
 * configure every mod in one familiar place.
 *
 * Copy this header into your project. There is no library to link and nothing to
 * import: MapForGoblins looks for YOU.
 *
 * How it works
 *   1. Your DLL exports:   int MfgMenuAddonInit(const mfg_menu_api *api);
 *   2. MapForGoblins walks the loaded modules (at its own start, and again each time
 *      the menu opens), calls your export once, and passes the API table.
 *   3. You create a page and hand over your rows. Return 1 to accept, 0 to decline.
 *
 * Threading: every callback runs on the game's UI thread, one at a time. Never block.
 *
 * Lifetime: strings you pass are COPIED immediately, so they may be temporaries. Your
 * module is pinned once a page is accepted, so your code stays valid.
 *
 * Compatibility: check api->abi_version. Fields are only ever appended, so a v1 client
 * keeps working against a later host; a v2 client must handle abi_version == 1 by
 * declining (return 0) or falling back to v1 features only.
 */
#ifndef MFG_MENU_API_H
#define MFG_MENU_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MFG_MENU_ABI_VERSION 1u

    /* What a row looks like and how it behaves when the player presses confirm. */
    typedef enum mfg_row_kind
    {
        MFG_ROW_INFO = 0,    /* read-only label + value */
        MFG_ROW_TOGGLE = 1,  /* value is your own "on"/"off" text; confirm calls on_activate */
        MFG_ROW_NUMBER = 2,  /* value is your formatted number; confirm steps it (your side) */
        MFG_ROW_ENUM = 3,    /* value is the current option; confirm advances it (your side) */
        MFG_ROW_ACTION = 4,  /* a button; confirm calls on_activate */
        MFG_ROW_PROGRESS = 5 /* label + bar drawn from done/total (value is optional) */
    } mfg_row_kind;

    typedef struct mfg_row
    {
        int32_t kind;           /* mfg_row_kind */
        const wchar_t *label;   /* required */
        const wchar_t *value;   /* optional (NULL = none) */
        int32_t done;           /* MFG_ROW_PROGRESS */
        int32_t total;          /* MFG_ROW_PROGRESS */
        uint32_t row_id;        /* echoed back to on_activate; yours to interpret */
        uint32_t reserved;      /* must be 0 */
    } mfg_row;

    /* Opaque page handle. */
    typedef void *mfg_page;

    /* Called when the player confirms one of your rows. Return 1 if you changed
     * anything (the menu then rebuilds your rows and redraws), 0 otherwise. */
    typedef int32_t (*mfg_on_activate)(void *user, uint32_t row_id);

    /* Called right before your rows are drawn, so you can refresh values. Fill rows
     * via api->set_rows from inside this callback (or ahead of time - both work). */
    typedef void (*mfg_on_build)(void *user, mfg_page page);

    typedef struct mfg_menu_api
    {
        uint32_t abi_version; /* MFG_MENU_ABI_VERSION of the HOST */
        uint32_t reserved;

        /* Create your page. `title` shows as the screen title and as the row that
         * opens it from the menu root. Returns NULL if the host refused. */
        mfg_page (*add_page)(const wchar_t *title, void *user);

        /* Replace the page's rows. Copies everything; call as often as you like. */
        void (*set_rows)(mfg_page page, const mfg_row *rows, uint32_t count);

        /* Optional callbacks. */
        void (*set_on_activate)(mfg_page page, mfg_on_activate cb);
        void (*set_on_build)(mfg_page page, mfg_on_build cb);

        /* RESERVED - DO NOT CALL. Accepted and ignored.
           This slot was published as "ask the menu to rebuild + redraw". The host records the
           request and nothing polls it: the only place a poll could go is the per-frame dialog
           update, and rebuilding the model from there would bump the row-pool generation while the
           live screen still points at the previous one - the defect that made a returning page come
           back with dead rows. Rows pushed with set_rows() already appear on the next build, so an
           add-on does not need this. The slot stays so the struct layout is stable for anything
           compiled against an earlier copy of this header; it will be repurposed or removed only
           with a version bump. */
        void (*request_redraw)(void);
    } mfg_menu_api;

    /* Implement and EXPORT this from your DLL. Return 1 to accept, 0 to decline. */
    /* int MfgMenuAddonInit(const mfg_menu_api *api); */

#ifdef __cplusplus
}
#endif

#endif /* MFG_MENU_API_H */
