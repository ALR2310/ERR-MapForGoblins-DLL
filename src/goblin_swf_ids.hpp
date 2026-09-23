#pragma once

// Which character ids a Scaleform movie registers, read from its tag stream the way the engine's own
// tag loaders read them. Shared by the world map's icon ids (goblin_gfx_probe.cpp, chosen at runtime
// above everything the loaded movie defines) and the menu movie's (goblin_own_movie.cpp).
//
// The tag set is not a list of "SWF define tags". It is the set of tags whose loader in the exe's own
// loader table registers an id in the movie's character registry, read off 2.2.0, 2.2.3, 2.6.0, 2.6.1,
// 2.6.2, 2.7.0 and 2.7.1 (identical on all seven; scratch/charid_runtime/tag_loaders.py):
//   * u16 id at the start of the body: 2 6 7 10 11 20 21 22 32 33 34 35 36 37 39 46 48 75 83 84 87 90,
//     1005 (compacted font), 1008 (sub-image);
//   * 1009 (external image): u32 id, and the loader keys it as (id & 0x9FFFF);
//   * 1002 (font texture info): u32 id, keyed as read;
//   * 57 / 71 (import assets): a url, (71 only) two reserved bytes, a u16 count, then count x
//     (u16 id, name) - each imported id is registered like a define.
// Not in the set because this engine registers nothing for them: 14, 60, 91 (no loader), 1001, 1006,
// 1007 (no loader), 1003 (keyed with type bits, 0x50000 | id), 1004 (stub loader). Only keys below
// 0x10000 are reported: every id we place is a u16, so a typed key can never meet ours.
//
// Pure functions over a byte buffer: no allocation, no exceptions, every read bounds-checked, no
// objects with destructors - callable from inside a __try.

#include <cstddef>
#include <cstdint>

namespace goblin::swf
{
    inline bool registers_u16_id(uint32_t code)
    {
        switch (code)
        {
        case 2: case 6: case 7: case 10: case 11: case 20: case 21: case 22: case 32: case 33:
        case 34: case 35: case 36: case 37: case 39: case 46: case 48: case 75: case 83: case 84:
        case 87: case 90: case 1005: case 1008:
            return true;
        default:
            return false;
        }
    }

    inline uint32_t le16(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
    inline uint32_t le32(const uint8_t *p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    // Optional id set: 65536 bits (1024 u64), bit n = id n is registered.
    inline void mark_id(uint64_t *bits, uint32_t id)
    {
        if (bits && id < 0x10000u)
            bits[id >> 6] |= 1ull << (id & 63);
    }

    // Every id below 0x10000 that ONE tag registers: `out` gets the highest, and each one is also set
    // in `bits` when that is given. false when the tag registers none.
    inline bool tag_ids(uint32_t code, const uint8_t *body, size_t len, uint32_t &out, uint64_t *bits)
    {
        uint32_t best = 0;
        bool any = false;
        if (registers_u16_id(code))
        {
            if (len < 2)
                return false;
            best = le16(body);
            any = true;
        }
        else if (code == 1009 || code == 1002)
        {
            if (len < 4)
                return false;
            const uint32_t id = le32(body);
            const uint32_t key = code == 1009 ? (id & 0x9FFFFu) : id;
            if (key >= 0x10000u)
                return false;
            best = key;
            any = true;
        }
        else if (code == 57 || code == 71)
        {
            size_t q = 0;
            while (q < len && body[q])
                ++q;
            ++q; // the url's terminator
            if (code == 71)
                q += 2; // two reserved bytes
            if (q + 2 > len)
                return false;
            uint32_t count = le16(body + q);
            q += 2;
            for (; count && q + 2 <= len; --count)
            {
                const uint32_t id = le16(body + q);
                q += 2;
                mark_id(bits, id);
                if (!any || id > best)
                    best = id;
                any = true;
                while (q < len && body[q])
                    ++q;
                ++q; // the name's terminator
            }
            if (any)
                out = best;
            return any;
        }
        if (any)
        {
            mark_id(bits, best);
            out = best;
        }
        return any;
    }

    // The highest id below 0x10000 that ONE tag registers, or false when it registers none.
    inline bool tag_max_id(uint32_t code, const uint8_t *body, size_t len, uint32_t &out)
    {
        return tag_ids(code, body, len, out, nullptr);
    }

    struct IdScan
    {
        uint32_t max_id;  // highest id below 0x10000 the movie registers (0 = none)
        uint32_t tags;    // tags walked, End included
        bool complete;    // the walk reached the End tag inside the buffer
    };

    // `d` = the whole movie as the parser reads it: uncompressed ("GFX" or "FWS"), header included.
    // A compressed or malformed movie comes back with complete == false. `bits` (optional, 1024 u64,
    // not cleared here) collects every id the stream registers.
    inline IdScan scan_movie_ids(const uint8_t *d, size_t n, uint64_t *bits = nullptr)
    {
        IdScan r{0, 0, false};
        if (!d || n < 13)
            return r;
        const bool gfx = d[0] == 'G' && d[1] == 'F' && d[2] == 'X';
        const bool swf = d[0] == 'F' && d[1] == 'W' && d[2] == 'S';
        if (!gfx && !swf)
            return r;
        const uint32_t nbits = d[8] >> 3; // frame RECT: 5 bits of field width, then four fields
        size_t pos = 8 + (5 + nbits * 4 + 7) / 8 + 4; // + frame rate u16 + frame count u16
        while (pos + 2 <= n)
        {
            const uint32_t head = le16(d + pos);
            pos += 2;
            const uint32_t code = head >> 6;
            size_t len = head & 0x3F;
            if (len == 0x3F)
            {
                if (pos + 4 > n)
                    return r;
                len = le32(d + pos);
                pos += 4;
            }
            if (len > n - pos)
                return r;
            ++r.tags;
            if (code == 0)
            {
                r.complete = true;
                return r;
            }
            uint32_t id = 0;
            if (tag_ids(code, d + pos, len, id, bits) && id > r.max_id)
                r.max_id = id;
            pos += len;
        }
        return r;
    }
}
