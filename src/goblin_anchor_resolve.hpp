#pragma once

// The anchor rebase ALGORITHM, and nothing else: no Windows, no logging, no globals.
// goblin_anchors.cpp feeds it the live module; scratch/anchor_resolver_test.cpp feeds it a
// mapped exe file, which is how this is verified against real downpatched builds instead of
// being argued about (2.6.2 / 2.6.0 / 2.2.3 / 2.2.0 - see that test).
//
// Why a rebase exists at all: a set of small engine helpers is called as `base + RVA`
// (their prologues are short and shared, so an AOB cannot pick them out). Those RVAs are
// measured on the build-target exe. On any other build they address unrelated code, and
// calling - or MinHook-patching - one corrupts the game. So each is anchored by the bytes
// that must live at it, and when the bytes are elsewhere it is re-found:
//
//   R1  bytes verify at the baked RVA. On the supported exe every anchor stops here.
//   R2  exactly one match within +/-0x4000 of the baked RVA -> that is the new home.
//   R3  predict the shift from the nearest ALREADY-resolved anchor (compilers keep
//       function order and local spacing across patches), match within +/-0x2000 of the
//       prediction, nearest wins.
//   R4  whole .text, nearest to the prediction. Last resort.
//
// Then ORDER: resolved anchors must appear in the same relative order as their baked RVAs.
// A violator is a wrong nearest-pick, and a wrong address is worse than a missing one, so
// it is dropped. Callers treat 0 as "this feature is off on this exe".

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace goblin::anchor_resolve
{
    // Order checking wants the resolved set in baked order; the table is generated and small,
    // so a stack buffer serves. A table larger than this simply skips the order check (every
    // anchor still had to match its own bytes) rather than allocating on a startup path.
    constexpr size_t kMaxEntries = 256;

    struct TableEntry
    {
        const char *name;
        uint32_t rva;
        const uint8_t *bytes;
        const uint8_t *mask; // 1 = compare, 0 = wildcard
        uint16_t len;
    };

    struct Image
    {
        uintptr_t base = 0;     // module base (VA)
        uintptr_t text_va = 0;  // .text start (VA)
        size_t text_len = 0;    // .text size
        uint32_t text_rva() const { return uint32_t(text_va - base); }
    };

    struct Stats
    {
        size_t at_baked = 0;
        size_t rebased = 0;
        size_t dead = 0;
        size_t order_dropped = 0;
        int64_t min_shift = 0;
        int64_t max_shift = 0;
    };

    struct Hit
    {
        uint32_t rva = 0;
        uint32_t count = 0;
    };

    inline bool matches_at(const Image &im, uintptr_t va, const TableEntry &e)
    {
        if (va < im.text_va || va + e.len > im.text_va + im.text_len)
            return false;
        const auto *p = reinterpret_cast<const uint8_t *>(va);
        for (uint16_t i = 0; i < e.len; ++i)
            if (e.mask[i] && p[i] != e.bytes[i])
                return false;
        return true;
    }

    // Count every match of e whose RVA lies within radius of center, and keep the one
    // NEAREST to pred. Counting is what makes R2's "unique near home" test possible;
    // nearest-to-prediction is what makes R3/R4 pick the right copy of a shared prologue.
    // No result cap: a truncated hit list could hide the true nearest.
    inline Hit find_nearest(const Image &im, const TableEntry &e, uint32_t center,
                            uint32_t radius, uint32_t pred)
    {
        Hit out{};
        const uint32_t trva = im.text_rva();
        uint64_t lo = center > radius ? uint64_t(center) - radius : 0;
        uint64_t hi = uint64_t(center) + radius;
        if (lo < trva)
            lo = trva;
        if (hi > uint64_t(trva) + im.text_len)
            hi = uint64_t(trva) + im.text_len;
        if (hi <= lo || hi - lo < e.len)
            return out;
        // Walk the first non-wildcard byte; confirm the rest only on a lead hit.
        uint16_t lead = 0;
        while (lead < e.len && !e.mask[lead])
            ++lead;
        if (lead == e.len)
            return out; // an all-wildcard pattern matches nothing meaningful
        const uint8_t first = e.bytes[lead];
        const uint8_t *p = reinterpret_cast<const uint8_t *>(im.base + lo) + lead;
        const uint8_t *end = reinterpret_cast<const uint8_t *>(im.base + hi) - (e.len - lead);
        int64_t best_d = INT64_MAX;
        while (p <= end)
        {
            const auto *f = static_cast<const uint8_t *>(
                std::memchr(p, first, size_t(end - p) + 1));
            if (!f)
                break;
            const uintptr_t va = reinterpret_cast<uintptr_t>(f) - lead;
            if (matches_at(im, va, e))
            {
                const uint32_t rva = uint32_t(va - im.base);
                ++out.count;
                const int64_t d = int64_t(rva) > int64_t(pred) ? int64_t(rva) - int64_t(pred)
                                                              : int64_t(pred) - int64_t(rva);
                if (d < best_d)
                {
                    best_d = d;
                    out.rva = rva;
                }
            }
            p = f + 1;
        }
        return out;
    }

    // Fill out[0..n) with the live RVA of each table entry, 0 when it could not be trusted.
    inline Stats resolve_into(const Image &im, const TableEntry *tbl, size_t n, uint32_t *out)
    {
        Stats st{};
        for (size_t i = 0; i < n; ++i)
            out[i] = 0;
        if (!im.base || !im.text_va || !im.text_len)
        {
            st.dead = n;
            return st;
        }

        // R1
        for (size_t i = 0; i < n; ++i)
            if (matches_at(im, im.base + tbl[i].rva, tbl[i]))
            {
                out[i] = tbl[i].rva;
                ++st.at_baked;
            }
        if (st.at_baked == n)
            return st; // the supported exe: no scanning happened at all

        // R2 - unique within +/-0x4000 of home
        for (size_t i = 0; i < n; ++i)
        {
            if (out[i])
                continue;
            const Hit h = find_nearest(im, tbl[i], tbl[i].rva, 0x4000, tbl[i].rva);
            if (h.count == 1)
                out[i] = h.rva;
        }

        // R3 (near the predicted shift), then R4 (whole .text)
        auto predict = [&](uint32_t baked) -> uint32_t {
            int64_t best_d = INT64_MAX, shift = 0;
            for (size_t j = 0; j < n; ++j)
                if (out[j])
                {
                    const int64_t d = int64_t(tbl[j].rva) > int64_t(baked)
                                          ? int64_t(tbl[j].rva) - int64_t(baked)
                                          : int64_t(baked) - int64_t(tbl[j].rva);
                    if (d < best_d)
                    {
                        best_d = d;
                        shift = int64_t(out[j]) - int64_t(tbl[j].rva);
                    }
                }
            const int64_t p = int64_t(baked) + shift;
            return p < 0 ? 0u : uint32_t(p);
        };
        for (int round = 0; round < 2; ++round)
        {
            for (size_t i = 0; i < n; ++i)
            {
                if (out[i])
                    continue;
                const uint32_t pred = predict(tbl[i].rva);
                const Hit h =
                    round == 0
                        ? find_nearest(im, tbl[i], pred, 0x2000, pred)
                        : find_nearest(im, tbl[i],
                                       uint32_t(im.text_rva() + im.text_len / 2),
                                       uint32_t(im.text_len), pred);
                if (h.count)
                    out[i] = h.rva;
            }
        }

        // Order: the resolved addresses must ascend in the same sequence the baked ones do.
        // Walked as CONSECUTIVE pairs in baked order (not every pair): a single bad pick must
        // cost itself, not every anchor after it. The later member of an inverted pair is the
        // one dropped, since the earlier one already agreed with everything before it.
        if (n <= kMaxEntries)
        {
            size_t order[kMaxEntries];
            size_t m = 0;
            for (size_t i = 0; i < n; ++i)
                if (out[i])
                {
                    size_t k = m++;
                    while (k > 0 && tbl[order[k - 1]].rva > tbl[i].rva)
                    {
                        order[k] = order[k - 1];
                        --k;
                    }
                    order[k] = i;
                }
            for (size_t k = 1; k < m; ++k)
            {
                const size_t prev = order[k - 1], cur = order[k];
                if (!out[prev]) // dropped earlier in this walk: compare against the last kept
                {
                    size_t back = k - 1;
                    while (back > 0 && !out[order[back]])
                        --back;
                    if (!out[order[back]])
                        continue;
                    if (out[cur] <= out[order[back]])
                    {
                        out[cur] = 0;
                        ++st.order_dropped;
                    }
                    continue;
                }
                if (out[cur] <= out[prev])
                {
                    out[cur] = 0;
                    ++st.order_dropped;
                }
            }
        }

        for (size_t i = 0; i < n; ++i)
        {
            if (!out[i])
            {
                ++st.dead;
                continue;
            }
            if (out[i] == tbl[i].rva)
                continue;
            ++st.rebased;
            const int64_t s = int64_t(out[i]) - int64_t(tbl[i].rva);
            if (s < st.min_shift)
                st.min_shift = s;
            if (s > st.max_shift)
                st.max_shift = s;
        }
        st.at_baked = 0;
        for (size_t i = 0; i < n; ++i)
            if (out[i] == tbl[i].rva)
                ++st.at_baked;
        return st;
    }
}
