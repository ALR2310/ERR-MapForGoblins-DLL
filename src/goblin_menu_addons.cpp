#include "goblin_menu_addons.hpp"

#include "../sdk/mfg_menu_api.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstring>
#include <unordered_set>

#include <windows.h>

namespace
{
    struct AddonPage
    {
        goblin::addons::Page data;
        void *user = nullptr;
        mfg_on_activate on_activate = nullptr;
        mfg_on_build on_build = nullptr;
        int strikes = 0;   // an add-on that keeps faulting is dropped
        bool alive = true;
    };

    std::vector<AddonPage> g_pages;
    std::unordered_set<void *> g_seen_modules; // modules we already offered the API to
    std::atomic<bool> g_redraw_requested{false};

    constexpr int kMaxStrikes = 3;
    constexpr uint32_t kMaxRowsPerPage = 128;

    AddonPage *page_at(size_t index)
    {
        return index < g_pages.size() ? &g_pages[index] : nullptr;
    }

    // POD-only bounded read of an untrusted client string (SEH frames must not hold
    // objects with destructors - MSVC C2712).
    size_t safe_wcs_len(const wchar_t *src, size_t cap)
    {
        size_t n = 0;
        __try
        {
            while (n < cap && src[n])
                ++n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return n;
    }

    std::wstring copy_text(const wchar_t *src, size_t cap = 128)
    {
        if (!src)
            return {};
        const size_t n = safe_wcs_len(src, cap);
        if (!n)
            return {};
        return std::wstring(src, n);
    }

    // POD-only copy of one client row struct.
    bool safe_copy_row(const mfg_row *src, mfg_row *out)
    {
        __try
        {
            *out = *src;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    // ── the API table handed to add-ons ───────────────────────────────────────────────
    mfg_page api_add_page(const wchar_t *title, void *user)
    {
        std::wstring t = copy_text(title);
        if (t.empty())
            return nullptr;
        AddonPage p;
        p.data.title = std::move(t);
        p.user = user;
        g_pages.push_back(std::move(p));
        return reinterpret_cast<mfg_page>(static_cast<uintptr_t>(g_pages.size()));
    }

    AddonPage *from_handle(mfg_page handle)
    {
        const size_t one_based = static_cast<size_t>(reinterpret_cast<uintptr_t>(handle));
        if (one_based == 0 || one_based > g_pages.size())
            return nullptr;
        return &g_pages[one_based - 1];
    }

    void api_set_rows(mfg_page handle, const mfg_row *rows, uint32_t count)
    {
        AddonPage *p = from_handle(handle);
        if (!p)
            return;
        if (count > kMaxRowsPerPage)
            count = kMaxRowsPerPage;
        std::vector<goblin::addons::Row> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            mfg_row src{};
            if (!safe_copy_row(&rows[i], &src))
                break; // bad array - keep whatever we already copied
            goblin::addons::Row r;
            r.kind = src.kind;
            r.label = copy_text(src.label);
            r.value = copy_text(src.value);
            r.done = src.done;
            r.total = src.total;
            r.row_id = src.row_id;
            if (r.label.empty())
                continue;
            out.push_back(std::move(r));
        }
        p->data.rows = std::move(out);
    }

    void api_set_on_activate(mfg_page handle, mfg_on_activate cb)
    {
        if (AddonPage *p = from_handle(handle))
            p->on_activate = cb;
    }

    void api_set_on_build(mfg_page handle, mfg_on_build cb)
    {
        if (AddonPage *p = from_handle(handle))
            p->on_build = cb;
    }

    void api_request_redraw() { g_redraw_requested.store(true, std::memory_order_release); }

    const mfg_menu_api kApi = {
        MFG_MENU_ABI_VERSION, 0u, &api_add_page, &api_set_rows,
        &api_set_on_activate, &api_set_on_build, &api_request_redraw,
    };

    using AddonInitFn = int32_t(const mfg_menu_api *);

    // Call one add-on's entry point behind an SEH boundary; a faulting add-on must never
    // take the game down with it.
    int32_t call_init(AddonInitFn *fn, bool *faulted)
    {
        *faulted = false;
        __try
        {
            return fn(&kApi);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *faulted = true;
            return 0;
        }
    }

    int32_t call_activate(mfg_on_activate cb, void *user, uint32_t row_id, bool *faulted)
    {
        *faulted = false;
        __try
        {
            return cb(user, row_id);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *faulted = true;
            return 0;
        }
    }

    void call_build(mfg_on_build cb, void *user, mfg_page handle, bool *faulted)
    {
        *faulted = false;
        __try
        {
            cb(user, handle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *faulted = true;
        }
    }

    // K32EnumProcessModules resolved at runtime, so no new import-table entry appears.
    using EnumModulesFn = BOOL(WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD);
    EnumModulesFn resolve_enum_modules()
    {
        static EnumModulesFn fn = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
                fn = reinterpret_cast<EnumModulesFn>(
                    GetProcAddress(k32, "K32EnumProcessModules"));
        }
        return fn;
    }
}

void goblin::addons::scan()
{
    EnumModulesFn enum_modules = resolve_enum_modules();
    if (!enum_modules)
        return;
    HMODULE mods[512];
    DWORD needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 512);
    for (size_t i = 0; i < n; ++i)
    {
        HMODULE m = mods[i];
        if (!m || g_seen_modules.count(m))
            continue;
        auto fn = reinterpret_cast<AddonInitFn *>(GetProcAddress(m, "MfgMenuAddonInit"));
        g_seen_modules.insert(m); // ask each module exactly once
        if (!fn)
            continue;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t *name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        const size_t before = g_pages.size();
        bool faulted = false;
        const int32_t accepted = call_init(fn, &faulted);
        for (size_t p = before; p < g_pages.size(); ++p)
            g_pages[p].data.owner = name;
        if (faulted)
        {
            spdlog::warn("[menu] add-on module reported an error while registering");
            g_pages.resize(before); // drop half-registered pages
            continue;
        }
        if (!accepted)
        {
            g_pages.resize(before);
            continue;
        }
        // Keep the module resident: we hold pointers into its code.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(fn), &pinned);
        spdlog::info("[menu] add-on pages registered: {} (total {})",
                     g_pages.size() - before, g_pages.size());
    }
}

size_t goblin::addons::page_count() { return g_pages.size(); }

const goblin::addons::Page *goblin::addons::page(size_t index)
{
    AddonPage *p = page_at(index);
    return (p && p->alive) ? &p->data : nullptr;
}

const goblin::addons::Page *goblin::addons::build_page(size_t index)
{
    AddonPage *p = page_at(index);
    if (!p || !p->alive)
        return nullptr;
    if (p->on_build)
    {
        bool faulted = false;
        call_build(p->on_build, p->user,
                   reinterpret_cast<mfg_page>(static_cast<uintptr_t>(index + 1)), &faulted);
        if (faulted && ++p->strikes >= kMaxStrikes)
        {
            p->alive = false;
            spdlog::warn("[menu] add-on page disabled after repeated errors");
            return nullptr;
        }
    }
    return &p->data;
}

bool goblin::addons::activate(size_t page_index, uint32_t row_id)
{
    AddonPage *p = page_at(page_index);
    if (!p || !p->alive || !p->on_activate)
        return false;
    bool faulted = false;
    const int32_t changed = call_activate(p->on_activate, p->user, row_id, &faulted);
    if (faulted && ++p->strikes >= kMaxStrikes)
    {
        p->alive = false;
        spdlog::warn("[menu] add-on page disabled after repeated errors");
    }
    return changed != 0;
}

bool goblin::addons::take_redraw_request()
{
    return g_redraw_requested.exchange(false, std::memory_order_acq_rel);
}
