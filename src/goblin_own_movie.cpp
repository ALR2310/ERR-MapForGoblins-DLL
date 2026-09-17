#include "goblin_own_movie.hpp"

#include "goblin_anchors.hpp" // the memory-file vtable is derived from an anchored ctor

#include "generated_shared/goblin_logo.hpp" // LOGO_TAG: the same bitmap the map registers

#include "goblin_config.hpp"
#include "goblin_gfx_probe.hpp"  // movie_name(): which movie a parse belongs to
#include "modutils.hpp"

#include "generated_shared/goblin_menu_icon_tags.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace
{
    // The vtable of the engine's memory-backed File. Movies come out of the archives as one
    // blob and are handed to Scaleform through this class, whose layout the constructor at
    // 0xCE7BB0 spells out:
    //   +0x00 vptr   +0x08 refcount   +0x10 path string
    //   +0x18 buffer  +0x20 size (u32)  +0x24 position (u32)
    // Because the loader only ever reads through those fields, re-pointing them at a buffer
    // of ours is all it takes to have our bytes parsed - no File implementation of our own,
    // no lifetime games with an object the engine allocated.
    // NOT a baked RVA any more (it was exe+0x2BA4C80): a vtable lives in .rdata, which moves
    // between exe builds like everything else, and a wrong vtable here means the "is this the
    // engine's memory file?" test compares against an unrelated pointer - it would answer NO
    // forever (transform silently off) or, worse, YES for something else. It is DERIVED from
    // the anchored constructor instead: the ctor's last `lea rax,[rip+..]` before its first
    // call is the class's own vptr. 0 when that cannot be established, and every caller below
    // treats 0 as "never match", so the transform stays off rather than guessing.
    uintptr_t memfile_vtable() { return goblin::anchors::memfile_vtable(); }
    constexpr size_t kMemFileBuffer = 0x18;
    constexpr size_t kMemFileSize = 0x20;
    constexpr size_t kMemFilePos = 0x24;

    // The screen's movie, matched as a case-insensitive substring of the movie's own name.
    constexpr const char *kMovieName = "02_160";

    bool contains_ci(const char *name, const char *needle)
    {
        if (!name)
            return false;
        for (const char *p = name; *p; ++p)
        {
            size_t k = 0;
            while (needle[k] && std::tolower(static_cast<unsigned char>(p[k])) ==
                                    std::tolower(static_cast<unsigned char>(needle[k])))
                ++k;
            if (!needle[k])
                return true;
        }
        return false;
    }


    // The world map's two panels used to be added here as well, by the same byte transform. They now
    // go into the PARSED movie instead (goblin_gfx_probe inject_map_panels), which reads the host
    // sprite, the character it places and a free depth out of the movie rather than knowing them as
    // constants - so this file is the MENU screen's transform and nothing else.

    std::atomic<bool> g_ready{false};

    const char *g_status = "not tried";

    uintptr_t base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

    // ── the movie container ──────────────────────────────────────────────────────────
    // An uncompressed Scaleform movie: "GFX" + version, u32 total length, then the frame
    // RECT, frame rate and frame count, then a flat tag stream. Tag headers are the SWF
    // ones: u16 (code << 6 | length), with length 0x3F meaning "u32 follows".
    struct Tag
    {
        uint16_t code = 0;
        uint32_t offset = 0; // payload start in the source buffer
        uint32_t length = 0;
        bool long_form = false; // how the SOURCE encoded the header
    };

    size_t header_size(const uint8_t *b, size_t n)
    {
        if (n < 13)
            return 0;
        const uint32_t nbits = b[8] >> 3; // RECT: 5 bits of count, then 4 signed fields
        const size_t rect = (5 + nbits * 4 + 7) / 8;
        const size_t total = 8 + rect + 4; // + frame rate (u16) + frame count (u16)
        return total <= n ? total : 0;
    }

    // Bytes that follow the End tag. Real files carry a few (alignment/padding), and dropping
    // them is what made the first round trip come out 8 bytes short.
    size_t g_tail_offset = 0;
    size_t g_tag_count = 0;
    bool g_icons_added = false;

    bool parse_tags(const uint8_t *b, size_t n, size_t start, std::vector<Tag> &out)
    {
        size_t off = start;
        while (off + 2 <= n)
        {
            const uint16_t head = static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
            off += 2;
            Tag t;
            t.code = static_cast<uint16_t>(head >> 6);
            uint32_t len = head & 0x3F;
            if (len == 0x3F)
            {
                if (off + 4 > n)
                    return false;
                std::memcpy(&len, b + off, 4);
                off += 4;
                t.long_form = true;
            }
            if (off + len > n)
                return false;
            t.offset = static_cast<uint32_t>(off);
            t.length = len;
            out.push_back(t);
            off += len;
            if (t.code == 0) // End
            {
                g_tail_offset = off;
                return true;
            }
        }
        return false;
    }

    void emit_tag(std::vector<uint8_t> &out, const Tag &t, const uint8_t *src)
    {
        const bool longform = t.long_form || t.length >= 0x3F;
        const uint16_t head =
            static_cast<uint16_t>((t.code << 6) | (longform ? 0x3F : (t.length & 0x3F)));
        out.push_back(static_cast<uint8_t>(head & 0xFF));
        out.push_back(static_cast<uint8_t>(head >> 8));
        if (longform)
        {
            const uint32_t len = t.length;
            out.insert(out.end(), reinterpret_cast<const uint8_t *>(&len),
                       reinterpret_cast<const uint8_t *>(&len) + 4);
        }
        out.insert(out.end(), src + t.offset, src + t.offset + t.length);
    }

    // The highest character id the movie defines: ours start one past it, so they collide with none
    // whatever movie loaded (the stock one, a patched one, an overhaul's). Define tags all start with
    // a character id; that is the only field we need.
    uint16_t highest_defined_cid(const uint8_t *src, const std::vector<Tag> &tags)
    {
        // SWF defines plus the GFX EXTENSIONS - 1009 (external image) is by far the most common define
        // tag in these movies and was missing, so the guard used to declare a range "free" while ids in
        // it were defined by image tags. 8 (JPEGTables) and 13 (DefineFontInfo) define no character and
        // are dropped. Audited 2026-07-28; the 1009 body leads with a u32 id, and reading its low u16 is
        // fine for the ids these movies use (all well under 0x10000).
        static const uint16_t kDefineTags[] = {
            2,  6,  7,  10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60, 75, 83,
            84, 87, 90, 91,
            1001, 1003, 1004, 1005, 1006, 1007, 1008, 1009};
        uint16_t highest = 0;
        for (const Tag &t : tags)
        {
            bool defines = false;
            for (uint16_t d : kDefineTags)
                if (t.code == d)
                {
                    defines = true;
                    break;
                }
            if (!defines || t.length < 2)
                continue;
            uint16_t cid = 0;
            std::memcpy(&cid, src + t.offset, 2);
            if (cid > highest)
                highest = cid;
        }
        return highest;
    }

    // Does the sprite `t` place anything at a depth in [lo, hi]? Our row children go there, so a row
    // clip that already uses one of those depths is left alone rather than having its own child
    // replaced. PlaceObject2 = {flags, depth u16, ...}, PlaceObject3 = {flags, flags2, depth u16, ...}.
    bool sprite_uses_depths(const Tag &t, const uint8_t *src, uint16_t lo, uint16_t hi)
    {
        const uint8_t *body = src + t.offset;
        size_t off = 4; // past cid + frameCount
        while (off + 2 <= t.length)
        {
            const uint16_t head = static_cast<uint16_t>(body[off] | (body[off + 1] << 8));
            off += 2;
            const uint16_t code = static_cast<uint16_t>(head >> 6);
            uint32_t len = head & 0x3F;
            if (len == 0x3F)
            {
                if (off + 4 > t.length)
                    return true; // unreadable - treat as taken
                std::memcpy(&len, body + off, 4);
                off += 4;
            }
            if (off + len > t.length)
                return true;
            const size_t depth_at = code == 26 ? 1 : code == 70 ? 2 : 0;
            if (depth_at && len >= depth_at + 2)
            {
                uint16_t depth = 0;
                std::memcpy(&depth, body + off + depth_at, 2);
                if (depth >= lo && depth <= hi)
                    return true;
            }
            off += len;
            if (code == 0) // End
                break;
        }
        return false;
    }

    // The column-caption block, removed for good. The two tofu headers ("Keyboard" / "Mouse")
    // live in sprite cid 168, and renaming their instances did NOT blank them - which proved the
    // text comes from initialText baked into their DefineEditText tags, not from the FMG id in
    // the name. They are also unreachable at runtime: frame 2 of KeySetting/BG (sprite cid 169)
    // places cid 168 as an UNNAMED child, and the resolver walks names only.
    //
    // So the placement itself goes. Sprite 169's body is {u16 cid, u16 frameCount, inner tags};
    // the inner PlaceObject2 for cid 168 is dropped and the body re-emitted at its new length,
    // which the full re-emit in rebuild() makes legal. Frame 1 of BG carries the backdrop and is
    // left completely alone.
    // The dark rectangle behind the rows is this BG sprite, placed at depth 1 inside the row section
    // (sprite 198). Removing that placement from the MOVIE was tried on 2026-07-28 and REVERTED: the
    // screen hung for several seconds on F8 and then crashed, so the dialog depends on that child being
    // there (it resolves or measures it). Hiding it at runtime instead - see the visibility call in
    // goblin_stall_probe.cpp - leaves the object in place and only turns it off.
    // A kDropRowPanel switch stood here, hard-coded false, gating a branch that dropped the dark
    // row-section panel from the movie instead of hiding it at runtime. Dropping it hung and then
    // crashed the game (reverted 2026-07-29 - the panel is hidden live by hide_row_panel now), so
    // the switch could never be turned on; its branch and the kRowSectionSpriteCid it needed went
    // with it. A compile-time switch that ships as a constant belongs in goblin_build_variants.hpp,
    // not in a .cpp - that is the whole point of that header.
    constexpr uint16_t kBgSpriteCid = 169;
    constexpr uint16_t kCaptionSpriteCid = 168;
    int g_caption_block_dropped = 0;

    // Emit `t` (a DefineSprite) with any PlaceObject2 of `dropCid` removed. Returns false when
    // the body does not parse or holds no such placement, in which case nothing is written.
    bool emit_sprite_without(std::vector<uint8_t> &out, const Tag &t, const uint8_t *src,
                             uint16_t dropCid)
    {
        if (t.length < 4)
            return false;
        const uint8_t *body = src + t.offset;
        std::vector<uint8_t> inner;
        inner.reserve(t.length);
        size_t off = 4; // past cid + frameCount
        bool dropped = false;
        while (off + 2 <= t.length)
        {
            const size_t head_at = off;
            const uint16_t head = static_cast<uint16_t>(body[off] | (body[off + 1] << 8));
            off += 2;
            const uint16_t code = static_cast<uint16_t>(head >> 6);
            uint32_t len = head & 0x3F;
            bool longform = false;
            if (len == 0x3F)
            {
                if (off + 4 > t.length)
                    return false;
                std::memcpy(&len, body + off, 4);
                off += 4;
                longform = true;
            }
            if (off + len > t.length)
                return false;
            bool skip = false;
            if (code == 26 && len >= 5) // PlaceObject2: flags, depth u16, [char u16]
            {
                const uint8_t flags = body[off];
                if (flags & 0x02)
                {
                    uint16_t cid = 0;
                    std::memcpy(&cid, body + off + 3, 2);
                    skip = cid == dropCid;
                }
            }
            if (skip)
                dropped = true;
            else
                inner.insert(inner.end(), body + head_at,
                             body + off + len); // header (+ long length) + payload
            off += len;
            if (code == 0) // End
                break;
        }
        if (!dropped)
            return false;
        const size_t total = 4 + inner.size();
        const bool longform = total >= 0x3F;
        const uint16_t head =
            static_cast<uint16_t>((39u << 6) | (longform ? 0x3F : (total & 0x3F)));
        out.push_back(static_cast<uint8_t>(head & 0xFF));
        out.push_back(static_cast<uint8_t>(head >> 8));
        if (longform)
        {
            const uint32_t n32 = static_cast<uint32_t>(total);
            out.insert(out.end(), reinterpret_cast<const uint8_t *>(&n32),
                       reinterpret_cast<const uint8_t *>(&n32) + 4);
        }
        out.insert(out.end(), body, body + 4); // cid + frameCount unchanged
        out.insert(out.end(), inner.begin(), inner.end());
        return true;
    }

    // Splice our icon characters in and give the row clip the children that place them.
    // Our own content comes from the build (tools/generate_menu_icon_tags.py: bitmaps, masks, the
    // strips, the four row placements) with LOCAL character ids; everything that depends on the movie
    // is decided here from the movie that actually loaded: the ids start one past its highest, and the
    // placements go into ITS row clip. Nothing about a stock .gfx is baked in.
    // ONE construction is spliced: a named child per icon, all baked invisible, the chosen one
    // scaled up. The ini key that used to pick between two candidates (`native_menu_icons`) was
    // removed along with the strip-plus-mask variant on 2026-07-29 - it is not in build_schema()
    // and not in ini_retired_keys(); nothing reads it anywhere.
    // The construction uses no frames: a timeline cannot be stopped from a tag stream, so a
    // multi-frame sprite would animate in every instance we do not reach.
    // Defined further down with the rest of the header-icon work; used here because the logo bitmap
    // goes in at the same place as the icon defines.
    uint16_t logo_cid();
    bool build_logo_define(std::vector<uint8_t> &tag);
    extern bool g_logo_spliced;
    // One past our icon ids in the movie being rebuilt; set by add_menu_icons before the logo goes in.
    uint16_t g_logo_cid = 0;
    // ── row density: a tighter pitch, and more row clips ─────────────────────────────
    // The screen shows as many rows as the row pool (sprite 190) has clips - eleven in the left
    // column (Item_0_0 .. Item_10_0), pitched 63.75 px apart between y = 35.3 and y = 673.2 px. The
    // gap is far larger than the 24 pt text needs, so the pitch comes down and the freed space pays
    // for extra clips. Both halves are needed: a tighter pitch alone would just leave a hole at the
    // bottom, and extra clips alone would not fit.
    //
    // The clips are OUR placements of the game's own row character, so they carry the icon child the
    // rebuilt row clip has. Whether the engine ever ASKS for the added slots (11..14 - four of them,
    // kRowSlots 15 minus kRowAuthored 11) is not assumed: the row-path hook logs the highest slot it
    // is called with, so one run in game settles it.
    constexpr uint16_t kRowCid = 189;           // the row clip: Item_N_0 in the pool below
    constexpr uint16_t kRowPoolCid = 190;       // KeySetting/ItemList
    constexpr int32_t kRowPitchPx = 49;         // authored 63.75; 14 rows then end where 11 did
    // Measured chain: KeySetting sits at y = 480 px, ItemList at -369.65 px inside it, so row 0
    // was drawn at 145.65 px absolute while the title's text ends around 77 px. 25 px of that gap
    // pays for the fifteenth row: the last one then ends at 806.65 px, and the help block below
    // starts at 863 px.
    constexpr int32_t kRowFirstYTwips = 706 - 25 * 20; // authored 706, lifted 25 px
    constexpr int32_t kRowXTwips = 155;         // Item_0_0's x, for the clips we add
    constexpr uint16_t kRowFirstDepth = 304;    // Item_0_0; each next row sits 16 lower
    constexpr uint16_t kRowDepthStep = 16;
    constexpr int kRowAuthored = 11;            // clips the movie ships in the left column
    constexpr int kRowExtra = goblin::own_movie::kRowSlots - kRowAuthored;
    // The RIGHT column partner of each added row. The screen is one parse shared with the player's
    // own Key Assignments screen and any other mod's key-binding pages, and those lay a keyboard
    // bind in Item_N_0 and a mouse bind in Item_N_1. Rows 11..14 with only a left clip made the
    // mouse binds of those rows vanish on such pages (reported by another modder, 2026-09-06). The
    // authored right clips (offline parse of 02_160): cid 182, x 892.15 px, depths 131 - 13*N
    // (Item_10_1 = 1). Depths 145..159 are unused, between Item_10_0 (144) and Item_9_0 (160).
    constexpr uint16_t kRowRightCid = 182;
    constexpr int32_t kRowRightXTwips = 17843;
    constexpr uint16_t kRowRightExtraDepth = 145;

    int32_t row_y_twips(int index) { return kRowFirstYTwips + kRowPitchPx * 20 * index; }

    // A PlaceObject2 for one row clip: flags 0x26 (HasCharacter|HasMatrix|HasName), then the matrix
    // packed as {HasScale 0, HasRotate 0, nTranslateBits 16, tx, ty} - the same shape the authored
    // rows use, with a fixed 16 bits so our own numbers always fit.
    size_t build_row_place(uint8_t *out, uint16_t depth, uint16_t cid, const char *name, int32_t tx,
                           int32_t ty)
    {
        size_t n = 0;
        out[n++] = 0x26;
        out[n++] = static_cast<uint8_t>(depth & 0xFF);
        out[n++] = static_cast<uint8_t>(depth >> 8);
        out[n++] = static_cast<uint8_t>(cid & 0xFF);
        out[n++] = static_cast<uint8_t>(cid >> 8);
        uint8_t mx[5] = {};
        uint32_t bit = 0;
        auto put = [&](uint32_t value, uint32_t bits) {
            for (uint32_t k = 0; k < bits; ++k, ++bit)
                if ((value >> (bits - 1 - k)) & 1)
                    mx[bit >> 3] |= static_cast<uint8_t>(1u << (7 - (bit & 7)));
        };
        put(0, 1);  // HasScale
        put(0, 1);  // HasRotate
        put(16, 5); // nTranslateBits
        put(static_cast<uint32_t>(tx) & 0xFFFF, 16);
        put(static_cast<uint32_t>(ty) & 0xFFFF, 16);
        for (uint8_t b : mx)
            out[n++] = b;
        for (const char *c = name; *c; ++c)
            out[n++] = static_cast<uint8_t>(*c);
        out[n++] = 0;
        return n;
    }

    size_t append_extra_rows(uint8_t *dst, size_t cap)
    {
        size_t len = 0;
        auto emit = [&](uint16_t depth, uint16_t cid, const char *name, int32_t tx, int32_t ty) {
            uint8_t body[48];
            const size_t blen = build_row_place(body, depth, cid, name, tx, ty);
            const uint16_t th = static_cast<uint16_t>((26u << 6) | (blen & 0x3F));
            if (len + 2 + blen > cap)
                return false;
            dst[len++] = static_cast<uint8_t>(th & 0xFF);
            dst[len++] = static_cast<uint8_t>(th >> 8);
            std::memcpy(dst + len, body, blen);
            len += blen;
            return true;
        };
        for (int k = 0; k < kRowExtra; ++k)
        {
            const int index = kRowAuthored + k;
            char name[16];
            _snprintf_s(name, sizeof(name), _TRUNCATE, "Item_%d_0", index);
            if (!emit(static_cast<uint16_t>(kRowFirstDepth - kRowDepthStep * index),
                      kRowCid, name, kRowXTwips, row_y_twips(index)))
                return len;
            // Its right-column partner, so a page that fills both columns keeps its mouse binds.
            _snprintf_s(name, sizeof(name), _TRUNCATE, "Item_%d_1", index);
            if (!emit(static_cast<uint16_t>(kRowRightExtraDepth + k), kRowRightCid, name,
                      kRowRightXTwips, row_y_twips(index)))
                return len;
        }
        return len;
    }

    // Re-space the rows the movie already has. Same-length in-place edit: only the translate bits of
    // each placement's matrix change, and every new value is smaller than the authored one, so it
    // still fits the field width that value was encoded with.
    int retune_row_pitch(std::vector<uint8_t> &buf)
    {
        int done = 0;
        for (int col = 0; col <= 1; ++col)
        {
            // Column 1 is our right-hand preview panel; it keeps the same pitch so the two columns
            // stay level with each other.
            for (int i = 0; i < kRowAuthored; ++i)
            {
                char name[16];
                _snprintf_s(name, sizeof(name), _TRUNCATE, "Item_%d_%d", i, col);
                const size_t nlen = std::strlen(name);
                const int32_t want = row_y_twips(i);
                bool patched = false;
                for (size_t at = 0; at + nlen + 1 <= buf.size() && !patched; ++at)
                {
                    if (std::memcmp(buf.data() + at, name, nlen + 1) != 0)
                        continue;
                    // The matrix length varies with how wide the authored numbers needed to be, so the
                    // body start is found by checking the three possibilities against known fields.
                    for (size_t mlen = 4; mlen <= 6 && !patched; ++mlen)
                    {
                        if (at < 5 + mlen)
                            continue;
                        const size_t body = at - 5 - mlen;
                        if (buf[body] != 0x26)
                            continue;
                        uint8_t *mx = buf.data() + body + 5;
                        auto read_bit = [&](uint32_t b) { return (mx[b >> 3] >> (7 - (b & 7))) & 1; };
                        auto write_bit = [&](uint32_t b, int v) {
                            const uint8_t m = static_cast<uint8_t>(1u << (7 - (b & 7)));
                            if (v) mx[b >> 3] |= m;
                            else   mx[b >> 3] &= static_cast<uint8_t>(~m);
                        };
                        if (read_bit(0) || read_bit(1)) // no scale, no rotation, as authored
                            continue;
                        uint32_t nt = 0;
                        for (uint32_t k = 0; k < 5; ++k)
                            nt = (nt << 1) | static_cast<uint32_t>(read_bit(2 + k));
                        if (nt < 8 || 7 + 2 * nt > mlen * 8)
                            continue; // not the shape we parsed - leave this one alone
                        const int32_t hi = (1 << (nt - 1)) - 1;
                        if (want > hi)
                            continue;
                        const uint32_t ty_at = 7 + nt;
                        for (uint32_t k = 0; k < nt; ++k)
                            write_bit(ty_at + k, (static_cast<uint32_t>(want) >> (nt - 1 - k)) & 1);
                        patched = true;
                        ++done;
                    }
                }
            }
        }
        spdlog::info("[ownmovie] row pitch set to {} px: {} of {} placements re-spaced", kRowPitchPx,
                     done, kRowAuthored * 2);
        return done;
    }

    // emit_sprite_with is defined further down; the row pool needs it here.
    bool emit_sprite_with(std::vector<uint8_t> &out, const Tag &t, const uint8_t *src,
                          const uint8_t *extra, size_t extra_len);

    bool add_menu_icons(const uint8_t *src, const std::vector<Tag> &tags,
                        std::vector<uint8_t> &out, const char **why)
    {
        namespace mi = goblin::menu_icon_tags;
        const Tag *row = nullptr;
        for (const Tag &t : tags)
        {
            if (t.code != 39 || t.length < 4) // DefineSprite
                continue;
            uint16_t cid = 0;
            std::memcpy(&cid, src + t.offset, 2);
            if (cid == kRowCid)
            {
                row = &t;
                break;
            }
        }
        if (!row)
        {
            *why = "row clip not found in this movie";
            return false;
        }
        if (sprite_uses_depths(*row, src, mi::ROW_DEPTH_LO, mi::ROW_DEPTH_HI))
        {
            *why = "the row clip already uses the depths our icon and slider children go on";
            return false;
        }
        // Our ids: one past the movie's highest, then the logo one past ours - so a window is only
        // "ours" if it is above everything the movie defines, whatever movie it is.
        const uint32_t base = static_cast<uint32_t>(highest_defined_cid(src, tags)) + 1;
        if (base + mi::CID_COUNT + 1 > 0xFFFFu)
        {
            *why = "no character ids left above this movie's own";
            return false;
        }
        g_logo_cid = static_cast<uint16_t>(base + mi::CID_COUNT);
        auto relocate = [base](const unsigned char *data, size_t len, const uint32_t *relocs,
                               size_t count) {
            std::vector<uint8_t> v(data, data + len);
            for (size_t k = 0; k < count; ++k)
            {
                uint16_t id = 0;
                std::memcpy(&id, v.data() + relocs[k], 2);
                id = static_cast<uint16_t>(id + base);
                std::memcpy(v.data() + relocs[k], &id, 2);
            }
            return v;
        };
        const std::vector<uint8_t> blob =
            relocate(mi::ICON_BLOB, mi::ICON_BLOB_LEN, mi::ICON_BLOB_CID_RELOCS,
                     sizeof(mi::ICON_BLOB_CID_RELOCS) / sizeof(mi::ICON_BLOB_CID_RELOCS[0]));
        const std::vector<uint8_t> places =
            relocate(mi::ROW_PLACES, mi::ROW_PLACES_LEN, mi::ROW_PLACES_CID_RELOCS,
                     sizeof(mi::ROW_PLACES_CID_RELOCS) / sizeof(mi::ROW_PLACES_CID_RELOCS[0]));
        spdlog::info("[ownmovie] menu icons: character ids {}..{} (one past the movie's highest), "
                     "{} icons + slider into row clip {} ({} bytes)",
                     base, base + mi::CID_COUNT - 1, mi::ICON_COUNT, kRowCid, row->length);
        // Our defines go immediately before the row clip, which uses them. Both are define
        // tags, so this lands before the movie's first frame either way.
        const size_t row_start = row->offset - (row->long_form ? 6u : 2u);
        for (const Tag &t : tags)
        {
            const size_t start = t.offset - (t.long_form ? 6u : 2u);
            if (start == row_start)
            {
                out.insert(out.end(), blob.begin(), blob.end());
                // The loaded row clip itself, with our children at the start of its frame 1 (where
                // they persist across the row's style frames).
                if (!emit_sprite_with(out, t, src, places.data(), places.size()))
                {
                    *why = "row clip body too short to extend";
                    return false;
                }
                // The logo bitmap joins the icons here: same kind of tag, same guaranteed-valid spot
                // before the movie's first frame, so the character exists by the time the header
                // places it.
                std::vector<uint8_t> logo;
                g_logo_spliced = build_logo_define(logo);
                if (g_logo_spliced)
                {
                    out.insert(out.end(), logo.begin(), logo.end());
                    spdlog::info("[ownmovie] logo bitmap added as charId {} ({} bytes)", logo_cid(),
                                 goblin::generated::LOGO_TAG_LEN);
                }
                continue;
            }
            if (t.code == 39 && t.length >= 2)
            {
                uint16_t cid = 0;
                std::memcpy(&cid, src + t.offset, 2);
                if (cid == kRowPoolCid && kRowExtra > 0)
                {
                    uint8_t extra[256];
                    const size_t elen = append_extra_rows(extra, sizeof(extra));
                    if (elen && emit_sprite_with(out, t, src, extra, elen))
                    {
                        spdlog::info("[ownmovie] row pool: {} extra row pairs added (slots {}..{}, "
                                     "left + right clip each)",
                                     kRowExtra, kRowAuthored, goblin::own_movie::kRowSlots - 1);
                        continue;
                    }
                }
                if (cid == kBgSpriteCid && emit_sprite_without(out, t, src, kCaptionSpriteCid))
                {
                    ++g_caption_block_dropped;
                    continue;
                }
                // (A third branch here dropped the BG sprite from the row section outright. See the
                //  kDropRowPanel note at the top of this file for why it can never be taken again.)
            }
            emit_tag(out, t, src);
        }
        return true;
    }

    // The key-binding screen labels two of its columns ("Keyboard" over the first bind and
    // (A dozen lines here argued that the two tofu column headers cannot be hidden at runtime and
    //  so must be RENAMED out of the name-driven populator's reach. That reasoning was disproved by
    //  the experiment recorded at the top of this file: renaming their instances did not blank them,
    //  because the text is initialText baked into the DefineEditText tags. The placement is dropped
    //  instead - emit_sprite_without(kBgSpriteCid, kCaptionSpriteCid) in rebuild().)
    // ── move the row section (menu centring) ─────────────────────────────────────────
    // The key-binding screen lays its rows out to the LEFT of the section origin, so the menu reads as
    // "left half" even though the origin itself is centred. Three runtime attempts to move it failed (see
    // the note in goblin_stall_probe.cpp): the movie's own timeline re-places the section a few frames in
    // and restores the authored position, and fighting that every frame reached a dying screen on close.
    // So change what the engine restores TO - the authored matrix in the movie we already rebuild.
    //
    // The tag, from an offline parse of 02_160_keyconfiguration.gfx: root PlaceObject2, flags 0x26
    // (HasCharacter|HasMatrix|HasName), depth 344, cid 198, MATRIX = {HasScale 0, HasRotate 0,
    // nTranslateBits 16, tx 19200, ty 9600} (twips; 19200 = 960 px = the centre of a 1920 stage), then the
    // name "KeySetting". So the body is 10 bytes before the name, the matrix 5 of those, and tx lives in
    // bits 7..22 of the matrix - a same-length in-place edit, which keeps every following offset intact
    // (the same rule retune_help_text and retune_row_font follow).
    // Derived, not guessed. Measured from the movie: 'KeySetting' (the section) sits at x=960, its
    // 'ItemList' child at -854 inside it, and the LEFT column items ('Item_N_0') at +7 inside that, each
    // about 750 px wide (child extents -14 .. 704). Our page fills only the left column - the screen is
    // authored with TWO columns of 11 rows, 'Item_N_0' at x=7 and 'Item_N_1' at x=892 - so the visible
    // block spans roughly 113..863 px and its centre is ~488. Moving that centre to the stage centre
    // (960) is +472. Set to 0 to leave the screen exactly as authored.
    //
    // Back to 472 on 2026-09-06 (it was 0 for a few hours): moving the authored matrix used to move
    // EVERY instance of the one shared parse - the player's own Key Assignments screen and other
    // mods' key-binding pages came up shifted right too. With the SEPARATE movie definition (see the
    // header) the transform only ever runs for our own parse, so the movie-level shift is per screen
    // again. goblin_stall_probe still carries a per-instance runtime shift for the fallback where the
    // opener hook is dead and the shared parse is transformed; it is a no-op when this def is in use.
    constexpr int32_t kRowSectionShiftPx = 472;
    constexpr uint16_t kRowSectionDepth = 344;
    constexpr uint16_t kRowSectionCid = 198;
    constexpr const char *kRowSectionName = "KeySetting";

    // ── the header icon: our logo instead of MENU_FL_Sysytem ─────────────────────────
    // sprite 201 (MenuTitle) -> sprite 199 -> character 23 (an external image). We re-point that
    // innermost placement, so the game keeps its own position and scale for the corner.
    constexpr uint16_t kTitleIconSpriteCid = 199; // the sprite that holds the icon image
    constexpr uint16_t kTitleIconImageCid = 23;   // MENU_FL_Sysytem.tga
    // One past our icon ids in this movie (add_menu_icons picks the window above the movie's own).
    uint16_t logo_cid() { return g_logo_cid; }

    // Re-point sprite 199's PlaceObject3 (code 70: flags1, flags2, depth u16, char u16) from the
    // game's icon to our logo. Same-length edit, applied to the finished buffer.
    // Append our logo bitmap as a define tag. DefineBitsLossless2 is tag 36; the body starts with the
    // charId, which LOGO_TAG carries as a placeholder for exactly this reason.
    // ── row text size ────────────────────────────────────────────────────────────────
    // The row's fields are 24 px as authored: Text_0 (the label) is cid 186 on the value frames and
    // cid 188 on the wide category frame, Text_1 (the value) is cid 185. Taking them to 22 px buys
    // about a sixth more characters per row - which the progress bars in the value column need more
    // than the labels do, and the labels lose nothing at this size.
    constexpr uint16_t kRowFontTwips = 22 * 20;
    const uint16_t kRowTextCids[] = {185, 186, 188};

    int retune_row_font(std::vector<uint8_t> &buf)
    {
        int done = 0;
        for (size_t i = 0; i + 8 < buf.size(); ++i)
        {
            const uint16_t head = static_cast<uint16_t>(buf[i] | (buf[i + 1] << 8));
            if ((head >> 6) != 37) // DefineEditText
                continue;
            uint32_t len = head & 0x3F;
            size_t body = i + 2;
            if (len == 0x3F)
            {
                std::memcpy(&len, buf.data() + body, 4);
                body += 4;
            }
            if (body + len > buf.size() || len < 8)
                continue;
            uint16_t cid = 0;
            std::memcpy(&cid, buf.data() + body, 2);
            bool wanted = false;
            for (uint16_t want : kRowTextCids)
                wanted = wanted || cid == want;
            if (!wanted)
                continue;
            // Same walk as the help field: RECT, flags, then the font id / class before the height.
            const uint32_t nbits = static_cast<uint32_t>(buf[body + 2] >> 3);
            size_t q = body + 2 + (5 + nbits * 4 + 7) / 8;
            const uint8_t f1 = buf[q], f2 = buf[q + 1];
            q += 2;
            if (f1 & 0x01)
                q += 2; // fontId
            if (f2 & 0x80)
            {
                while (q < body + len && buf[q] != 0)
                    ++q;
                ++q; // past the fontClass string
            }
            if (!((f1 & 0x01) || (f2 & 0x80)) || q + 2 > body + len)
                continue;
            uint16_t was = 0;
            std::memcpy(&was, buf.data() + q, 2);
            if (was == kRowFontTwips)
                continue;
            std::memcpy(buf.data() + q, &kRowFontTwips, 2);
            spdlog::info("[ownmovie] row text {}: font {} -> {} px", cid, was / 20,
                         kRowFontTwips / 20);
            ++done;
        }
        return done;
    }

    // ── nudging the header icon ──────────────────────────────────────────────────────
    // The icon's OWN placement (char 23 inside sprite 199) cannot be moved in place: its matrix carries
    // nTranslateBits = 0, i.e. no translation field at all, so writing one would change the tag length.
    // The level above can: sprite 201 (MenuTitle) places sprite 199 with a 14-bit translate, and sprite
    // 201 itself sits at root scale 1.0 - so 20 twips there is exactly one screen pixel, and sprite 199
    // holds nothing but the icon, so moving it moves the icon and not the title text beside it.
    // These two numbers are the whole adjustment. Negative = left / up.
    constexpr int32_t kTitleIconDxPx = -36;
    constexpr int32_t kTitleIconDyPx = -22;

    int nudge_title_icon(std::vector<uint8_t> &buf)
    {
        if (kTitleIconDxPx == 0 && kTitleIconDyPx == 0)
            return 0;
        // PlaceObject2 body: flags 0x06 (HasCharacter|HasMatrix), depth 1, cid 199. That five-byte
        // signature occurs exactly once in the movie.
        const uint8_t sig[5] = {0x06, 0x01, 0x00, static_cast<uint8_t>(kTitleIconSpriteCid & 0xFF),
                                static_cast<uint8_t>(kTitleIconSpriteCid >> 8)};
        for (size_t i = 0; i + 5 + 10 <= buf.size(); ++i)
        {
            if (std::memcmp(buf.data() + i, sig, 5) != 0)
                continue;
            uint8_t *mx = buf.data() + i + 5;
            auto read_bit = [&](uint32_t b) { return (mx[b >> 3] >> (7 - (b & 7))) & 1; };
            auto write_bit = [&](uint32_t b, int v) {
                const uint8_t mask = static_cast<uint8_t>(1u << (7 - (b & 7)));
                if (v) mx[b >> 3] |= mask;
                else   mx[b >> 3] &= static_cast<uint8_t>(~mask);
            };
            auto read_bits = [&](uint32_t at, uint32_t n) {
                uint32_t v = 0;
                for (uint32_t k = 0; k < n; ++k)
                    v = (v << 1) | static_cast<uint32_t>(read_bit(at + k));
                return v;
            };
            // Verify the shape before touching it, the same way the row-section shift does: scale
            // present with 17 bits per field, no rotation, 14 bits per translate.
            if (!read_bit(0))
                continue;
            const uint32_t nscale = read_bits(1, 5);
            const uint32_t rot_at = 6 + 2 * nscale;
            if (nscale != 17 || read_bit(rot_at))
                continue;
            const uint32_t nt = read_bits(rot_at + 1, 5);
            if (nt != 14)
                continue;
            const uint32_t tx_at = rot_at + 6;
            const uint32_t ty_at = tx_at + nt;
            auto sign_extend = [nt](uint32_t v) {
                const uint32_t sign = 1u << (nt - 1);
                return static_cast<int32_t>((v & sign) ? (v | ~(sign * 2 - 1)) : v);
            };
            const int32_t tx = sign_extend(read_bits(tx_at, nt));
            const int32_t ty = sign_extend(read_bits(ty_at, nt));
            const int32_t want_x = tx + kTitleIconDxPx * 20; // 20 twips = 1 px at this level
            const int32_t want_y = ty + kTitleIconDyPx * 20;
            const int32_t lo = -(1 << (nt - 1)), hi = (1 << (nt - 1)) - 1;
            if (want_x < lo || want_x > hi || want_y < lo || want_y > hi)
            {
                spdlog::info("[ownmovie] header icon: {},{} twips does not fit {} signed bits - left "
                             "where it was", want_x, want_y, nt);
                return 0;
            }
            for (uint32_t k = 0; k < nt; ++k)
            {
                write_bit(tx_at + k, (static_cast<uint32_t>(want_x) >> (nt - 1 - k)) & 1);
                write_bit(ty_at + k, (static_cast<uint32_t>(want_y) >> (nt - 1 - k)) & 1);
            }
            spdlog::info("[ownmovie] header icon moved: {},{} -> {},{} twips ({},{} -> {},{} px)", tx,
                         ty, want_x, want_y, tx / 20, ty / 20, want_x / 20, want_y / 20);
            return 1;
        }
        spdlog::info("[ownmovie] header icon: the sprite {} placement was not found - position left as "
                     "authored", kTitleIconSpriteCid);
        return 0;
    }

    // Build the tag, do not place it: WHERE it goes is decided by add_menu_icons, which is walking the
    // parsed tag list and knows a real tag boundary. Scanning the finished buffer for a ShowFrame byte
    // pattern (the first attempt) could just as easily land inside another tag's payload and corrupt
    // the movie, and a define inserted at a bogus offset is not something the game reports - it just
    // stops working.
    bool build_logo_define(std::vector<uint8_t> &tag)
    {
        const size_t body_len = goblin::generated::LOGO_TAG_LEN;
        if (body_len < 2)
            return false;
        const uint32_t code_len = static_cast<uint32_t>(body_len);
        const uint16_t head = static_cast<uint16_t>((36u << 6) | 0x3F); // DefineBitsLossless2, long form
        tag.insert(tag.end(), reinterpret_cast<const uint8_t *>(&head),
                   reinterpret_cast<const uint8_t *>(&head) + 2);
        tag.insert(tag.end(), reinterpret_cast<const uint8_t *>(&code_len),
                   reinterpret_cast<const uint8_t *>(&code_len) + 4);
        tag.insert(tag.end(), goblin::generated::LOGO_TAG,
                   goblin::generated::LOGO_TAG + body_len);
        const uint16_t cid = logo_cid();
        std::memcpy(tag.data() + 6, &cid, 2); // the body starts with the charId placeholder
        return true;
    }

    bool g_logo_spliced = false;

    int swap_title_icon(std::vector<uint8_t> &buf)
    {
        const uint16_t want_from = kTitleIconImageCid;
        const uint16_t want_to = logo_cid();
        int patched = 0;
        for (size_t i = 0; i + 16 <= buf.size(); ++i)
        {
            // DefineSprite body for cid 199: charId, frameCount, then the sprite's own tag list. We
            // anchor on that rather than walking the whole tree again, and verify the placement below
            // before touching anything.
            uint16_t cid = 0;
            std::memcpy(&cid, buf.data() + i, 2);
            if (cid != kTitleIconSpriteCid)
                continue;
            const size_t inner = i + 4; // past charId + frameCount
            const uint16_t head = static_cast<uint16_t>(buf[inner] | (buf[inner + 1] << 8));
            const uint16_t code = static_cast<uint16_t>(head >> 6);
            if (code != 26 && code != 70) // PlaceObject2 / PlaceObject3
                continue;
            // A length field of 0x3F means the real length follows as a u32. The shipped movie uses
            // that LONG FORM here even though its 12-byte body would fit the short one, and assuming
            // the short form is why the first attempt reported "not found" while the placement was
            // sitting right there.
            const size_t body = inner + 2 + (((head & 0x3F) == 0x3F) ? 4u : 0u);
            if (body + 8 > buf.size())
                continue;
            if (!(buf[body] & 0x02)) // HasCharacter
                continue;
            // PlaceObject3 carries a second flag byte; a class name (flags2 bit 3) would push a string
            // in front of the character id, so leave that shape alone instead of guessing at it.
            if (code == 70 && (buf[body + 1] & 0x08))
                continue;
            const size_t at = body + (code == 70 ? 2u : 1u) + 2u; // past the flags and the depth
            uint16_t placed = 0;
            std::memcpy(&placed, buf.data() + at, 2);
            if (placed != want_from)
                continue;
            std::memcpy(buf.data() + at, &want_to, 2);
            ++patched;
            spdlog::info("[ownmovie] title icon re-pointed: sprite {} child {} -> our logo {}",
                         kTitleIconSpriteCid, want_from, want_to);
            break;
        }
        if (!patched)
            spdlog::info("[ownmovie] title icon left as authored (sprite {} child {} not found - the "
                         "movie is not the one we parsed)", kTitleIconSpriteCid, want_from);
        return patched;
    }

    int shift_row_section(std::vector<uint8_t> &buf)
    {
        if (kRowSectionShiftPx == 0)
            return 0;
        const size_t nlen = std::strlen(kRowSectionName);
        for (size_t i = 0; i + nlen + 1 <= buf.size(); ++i)
        {
            if (std::memcmp(buf.data() + i, kRowSectionName, nlen + 1) != 0)
                continue;
            if (i < 10)
                continue;
            const size_t body = i - 10;
            uint16_t depth = 0, cid = 0;
            std::memcpy(&depth, buf.data() + body + 1, 2);
            std::memcpy(&cid, buf.data() + body + 3, 2);
            if (buf[body] != 0x26 || depth != kRowSectionDepth || cid != kRowSectionCid)
                continue;
            uint8_t *mx = buf.data() + body + 5;
            // {HasScale, HasRotate, nT[5]} must match what we parsed, or the layout is not what we think.
            const bool has_scale = (mx[0] & 0x80) != 0;
            const bool has_rotate = (mx[0] & 0x40) != 0;
            const uint32_t nT = static_cast<uint32_t>((mx[0] >> 1) & 0x1F);
            if (has_scale || has_rotate || nT != 16)
            {
                spdlog::info("[ownmovie] row section: unexpected matrix (scale {} rotate {} nT {}) - "
                             "left as authored", has_scale, has_rotate, nT);
                return 0;
            }
            // tx = 16 bits starting at bit 7 of the matrix.
            auto read_bit = [&](uint32_t b) { return (mx[b >> 3] >> (7 - (b & 7))) & 1; };
            auto write_bit = [&](uint32_t b, int v) {
                const uint8_t mask = static_cast<uint8_t>(1u << (7 - (b & 7)));
                if (v) mx[b >> 3] |= mask;
                else   mx[b >> 3] &= static_cast<uint8_t>(~mask);
            };
            int32_t tx = 0;
            for (uint32_t k = 0; k < 16; ++k)
                tx = (tx << 1) | read_bit(7 + k);
            if (tx & 0x8000)
                tx -= 0x10000; // sign-extend
            const int32_t want = tx + kRowSectionShiftPx * 20;
            if (want < -32768 || want > 32767)
            {
                spdlog::info("[ownmovie] row section: {} twips does not fit 16 signed bits - left alone",
                             want);
                return 0;
            }
            const uint32_t enc = static_cast<uint32_t>(want) & 0xFFFF;
            for (uint32_t k = 0; k < 16; ++k)
                write_bit(7 + k, (enc >> (15 - k)) & 1);
            spdlog::info("[ownmovie] row section moved: tx {} -> {} twips ({} -> {} px)", tx, want,
                         tx / 20, want / 20);
            return 1;
        }
        spdlog::info("[ownmovie] row section: '{}' placement not found - screen left as authored",
                     kRowSectionName);
        return 0;
    }

    // ── the help field under the rows ────────────────────────────────────────────────
    // 'ActionHelp/Text_0' is the block our row descriptions go into (DefineEditText cid 163). As shipped:
    // bounds -2..1668 x -2..91 px (so ~1670 x 93), WordWrap and Multiline on, HTML on, font class
    // MenuFont_01 at 24 px with 7 px leading - which is two, at most three lines, and in game the text
    // wraps around the middle of the screen even though the declared width is nearly the full stage.
    // The RECT is bit-packed with 17 bits per coordinate, so anything up to 65535 twips (3276 px) can be
    // written back IN PLACE without changing the tag length - the same rule the row-section shift follows.
    // We make it TALLER (more lines) and drop the font size so more characters fit per line; the width
    // is deliberately left as authored.
    constexpr uint16_t kHelpTextCid = 163;
    // WIDTH IS LEFT ALONE. As authored the field is already 1670 px wide starting at x=121 on the stage,
    // i.e. it reaches 1791 of 1920 - the width was never the limit. Two attempts proved it: 3000 px only
    // let a long line run off the screen, and 1780 px merely ate the right margin. What actually limited
    // the text was the HEIGHT (two lines) and our own hard newlines in the i18n strings.
    constexpr int32_t kHelpHeightTwips = 4000;  // 200 px ~ six lines at 18 px + leading
    constexpr uint16_t kHelpFontTwips = 360;    // 18 px (was 24)

    int retune_help_text(std::vector<uint8_t> &buf)
    {
        // DefineEditText body: cid u16, RECT, flags1, flags2, [fontId u16] [fontClass string]
        // [fontHeight u16] [colour RGBA] [maxLength u16] [layout 9] varName z-string [initialText]
        for (size_t i = 0; i + 8 < buf.size(); ++i)
        {
            const uint16_t head = static_cast<uint16_t>(buf[i] | (buf[i + 1] << 8));
            if ((head >> 6) != 37)
                continue;
            uint32_t len = head & 0x3F;
            size_t body = i + 2;
            if (len == 0x3F)
            {
                std::memcpy(&len, buf.data() + body, 4);
                body += 4;
            }
            if (body + len > buf.size() || len < 8)
                continue;
            uint16_t cid = 0;
            std::memcpy(&cid, buf.data() + body, 2);
            if (cid != kHelpTextCid)
                continue;
            uint8_t *rect = buf.data() + body + 2;
            const uint32_t nbits = static_cast<uint32_t>(rect[0] >> 3);
            if (nbits != 17)
            {
                spdlog::info("[ownmovie] help text: RECT is {} bits, not the 17 we patch - left alone",
                             nbits);
                return 0;
            }
            // bit layout: 5 bits nbits, then x0, x1, y0, y1 - each `nbits` wide, signed.
            auto put = [&](uint32_t bit, int32_t value) {
                for (uint32_t k = 0; k < nbits; ++k)
                {
                    const uint32_t b = bit + k;
                    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (b & 7)));
                    if ((value >> (nbits - 1 - k)) & 1)
                        rect[b >> 3] |= mask;
                    else
                        rect[b >> 3] &= static_cast<uint8_t>(~mask);
                }
            };
            put(5 + nbits * 3, kHelpHeightTwips); // y1 only - see the note on the width above
            // Force WordWrap + Multiline. The shipped field claims both (flags1 0xEC) yet a long line ran
            // off the stage, so make the intent explicit rather than trust the read.
            {
                size_t fq = body + 2 + (5 + nbits * 4 + 7) / 8;
                const uint8_t before = buf[fq];
                buf[fq] = static_cast<uint8_t>(before | 0x40 | 0x20);
                if (buf[fq] != before)
                    spdlog::info("[ownmovie] help text: flags1 0x{:02X} -> 0x{:02X} (WordWrap+Multiline)",
                                 before, buf[fq]);
            }
            // font height: skip the RECT, then flags; HasFont(0x01) or HasFontClass(0x80 of flags2)
            size_t q = body + 2 + (5 + nbits * 4 + 7) / 8;
            const uint8_t f1 = buf[q], f2 = buf[q + 1];
            q += 2;
            if (f1 & 0x01)
                q += 2; // fontId
            if (f2 & 0x80)
            {
                while (q < body + len && buf[q] != 0)
                    ++q;
                ++q; // past the fontClass string
            }
            if ((f1 & 0x01) || (f2 & 0x80))
            {
                uint16_t was = 0;
                std::memcpy(&was, buf.data() + q, 2);
                std::memcpy(buf.data() + q, &kHelpFontTwips, 2);
                spdlog::info("[ownmovie] help text retuned: height {} px (width untouched), "
                             "font {} -> {} px", kHelpHeightTwips / 20, was / 20, kHelpFontTwips / 20);
            }
            else
                spdlog::info("[ownmovie] help text retuned: height {} px (width and font left "
                             "as authored)", kHelpHeightTwips / 20);
            return 1;
        }
        spdlog::info("[ownmovie] help text: DefineEditText {} not found", kHelpTextCid);
        return 0;
    }

    // rename_column_captions() stood here: it renamed StaticText_280005/280006 in place (same-length
    // patch) so the FMG-id-driven populator would not fill them. It kept running on every rebuild
    // long after the fix that actually works replaced it, and reported its count in the transform
    // log - a number that only ever described work with no effect. Removed 2026-07-31.

    // Emit `t` (a DefineSprite) with `extra` inserted at the start of its frame-1 tag list, i.e.
    // right after the {cid, frameCount} header - the same spot the row-clip edit uses, so the
    // placement survives every frame the sprite has. The length changes, which is legal because
    // rebuild() re-emits the whole stream.
    bool emit_sprite_with(std::vector<uint8_t> &out, const Tag &t, const uint8_t *src,
                          const uint8_t *extra, size_t extra_len)
    {
        if (t.length < 4)
            return false;
        const size_t total = t.length + extra_len;
        const bool longform = total >= 0x3F;
        const uint16_t head =
            static_cast<uint16_t>((39u << 6) | (longform ? 0x3F : (total & 0x3F)));
        out.push_back(static_cast<uint8_t>(head & 0xFF));
        out.push_back(static_cast<uint8_t>(head >> 8));
        if (longform)
        {
            const uint32_t n32 = static_cast<uint32_t>(total);
            out.insert(out.end(), reinterpret_cast<const uint8_t *>(&n32),
                       reinterpret_cast<const uint8_t *>(&n32) + 4);
        }
        const uint8_t *body = src + t.offset;
        out.insert(out.end(), body, body + 4); // cid + frameCount
        out.insert(out.end(), extra, extra + extra_len);
        out.insert(out.end(), body + 4, body + t.length);
        return true;
    }

    // Rebuild the movie from its parsed tags, adding our own pieces on the way. The full
    // parse and re-emit is what makes that safe: it was proven byte-for-byte identical
    // against the untouched file before any edit rode on it. Returns false to say "serve the
    // original instead".
    bool rebuild(const uint8_t *src, size_t n, std::vector<uint8_t> &out)
    {
        if (n < 16 || std::memcmp(src, "GFX", 3) != 0)
        {
            g_status = "not an uncompressed GFX movie";
            return false;
        }
        const size_t hdr = header_size(src, n);
        if (!hdr)
        {
            g_status = "unreadable movie header";
            return false;
        }
        std::vector<Tag> tags;
        g_tail_offset = 0;
        if (!parse_tags(src, n, hdr, tags) || tags.empty())
        {
            g_status = "tag stream did not parse";
            return false;
        }
        out.clear();
        out.reserve(n + 4096);
        out.insert(out.end(), src, src + hdr);
        g_tag_count = tags.size();
        g_logo_spliced = false; // per rebuild, so a failed one cannot leave the flag set
        const char *why = nullptr;
        g_icons_added = add_menu_icons(src, tags, out, &why);
        if (!g_icons_added)
        {
            // Nothing of ours goes in, so hand back the game's own bytes untouched rather
            // than a rebuild of them: with the direct-draw icon route (native_menu_icons =
            // 3) that leaves this movie exactly as the game - or an overhaul - shipped it.
            g_status = "movie untouched";
            spdlog::info("[ownmovie] serving '{}' unchanged: {}", kMovieName, why ? why : "?");
            return false;
        }
        if (g_tail_offset && g_tail_offset < n)
            out.insert(out.end(), src + g_tail_offset, src + n);
        shift_row_section(out); // menu centring, same-length in-place edit
        retune_help_text(out);  // the description block under the rows
        retune_row_pitch(out);  // tighter rows, to pay for the clips added above
        retune_row_font(out);   // kRowFontTwips = 22 px, so more of a bar fits in the value column
        // Only re-point the header icon if the bitmap actually went in - a placement pointing at a
        // character that does not exist draws nothing at all, which is worse than the game's own icon.
        if (g_logo_spliced)
        {
            swap_title_icon(out);
            nudge_title_icon(out); // our art is not framed like the icon it replaces
        }
        else
            spdlog::warn("[ownmovie] logo bitmap not added - header icon left as authored");
        // The header's length field is the only thing we author, and writing our absolute size
        // into it is what kept the round trip "DIFFERENT" at an identical size: the file does
        // not necessarily count the same bytes we do (its own value is 8 short here, matching
        // the trailer). So carry the file's convention over and only apply our DELTA.
        uint32_t stored = 0;
        std::memcpy(&stored, src + 4, 4);
        const int64_t delta = static_cast<int64_t>(out.size()) - static_cast<int64_t>(n);
        const uint32_t adjusted = static_cast<uint32_t>(static_cast<int64_t>(stored) + delta);
        std::memcpy(out.data() + 4, &adjusted, 4);
        return true;
    }

    // ══ the parse route: the bytes reach the PARSER whoever served the file ══════════
    // This used to run on the engine's own movie OPENER, and being asked at all was then the loader's
    // decision: ModEngine2 substitutes an overridden file underneath that call, so it still happens and
    // the bytes are the modded ones, while ModEngine3 answers for the files it overrides before the
    // engine ever gets there (its `MakeEblObject` returns None for a mapped path, and the engine goes
    // down its disk route instead). Measured on Convergence: of the 22 movies it overrides, the opener
    // saw 0; of the 90 it does not, all 90. Taking the opener's vtable slot to get above that changed
    // nothing - the slot still named the engine's own function - because the call is not intercepted,
    // it simply never happens. See docs/research_runtime_map_panels.md.
    //
    // Being asked to PARSE a movie is not the loader's decision: whatever produced the bytes, they end
    // up in the tag loop - which is also where the mod's icons already reach the world map.
    //
    // FUN_141169590(movieData, ctx, arg3) IS that loop: it takes the reader from ctx+0x418 (or the
    // inline one at ctx+0x50), announces the header it has just read ("Note: SWF Frame Rate = %f,
    // Frames = %d"), and only then starts reading tags - so on entry the header is done and NOT ONE TAG
    // has been read. That is what makes this the right place: our bytes are handed over before the first
    // one, so nothing about the file's own content has to line up with ours.
    //
    // The loop's own end-of-stream bound is ctx+0x2c4 (its condition is `position < that`), so a longer
    // movie needs that raised or the tags we add past the original length are never reached.
    using TagLoopFn = void(void *movieData, void *ctx, void *arg3);
    TagLoopFn *o_tag_loop = nullptr;
    constexpr size_t kCtxReader = 0x418;
    constexpr size_t kCtxReaderInline = 0x50;
    constexpr size_t kCtxTotalLen = 0x2C4;
    constexpr size_t kReaderSource = 0x20;
    // The Scaleform File interface, read off the engine's own memory-file implementation (the class
    // whose layout kMemFile* describes): GetLength at vtable+0x38, Read(buf, n) at +0x50, Seek(offset,
    // whence) at +0x70 with whence 0 = from the start. Every File the parser can be given implements
    // it, which is what lets the bytes be read whether the movie arrived as one blob from an archive or
    // as a stream from disk.
    constexpr size_t kFileGetLength = 0x38;
    constexpr size_t kFileRead = 0x50;
    constexpr size_t kFileSeek = 0x70;
    // Refcount field of that class. A source of ours is handed to the engine, and if it is ever
    // released we must not be freed out from under the parse - so the count starts far from zero.
    constexpr size_t kMemFileRefs = 0x08;
    constexpr uint32_t kNeverFree = 0x4000'0000u;

    // Every movie the parser is given, counted. Unlike the opener this route sees the ones a loader
    // overrides too, so the count is the honest total.
    std::atomic<uint32_t> g_movies_seen{0};
    std::vector<uint8_t> g_parse_bytes;       // the movie we serve to the parser (must outlive it)
    alignas(16) uint8_t g_parse_source[0x48]; // our File, cloned from one of the engine's own
    bool g_source_template = false;           // a memory-file object has been seen and copied
    // (a g_parsed_seen counter sat here with a single occurrence - this declaration. The live
    //  count of movies the parser was handed is g_movies_seen, logged in tag_loop_detour.)

    // `outPos` comes back with the position the parser is AT - the reader has already taken the header
    // out of this source and reads ahead of itself, so that position is what the swap has to preserve.
    bool read_whole_movie(void *file, uint32_t *outLen, uint32_t *outPos, std::vector<uint8_t> &out)
    {
        uintptr_t vt = 0;
        int32_t len = 0, pos = 0;
        // POD-only reads first; the vector work happens outside any __try.
        __try
        {
            vt = *reinterpret_cast<uintptr_t *>(file);
            len = reinterpret_cast<int32_t (*)(void *)>(*reinterpret_cast<void **>(vt + kFileGetLength))(
                file);
            pos = reinterpret_cast<int32_t (*)(void *, int32_t, int32_t)>(
                *reinterpret_cast<void **>(vt + kFileSeek))(file, 0, 1);  // whence 1, 0 bytes = tell
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (len < 16 || len > 64 * 1024 * 1024)
            return false;
        out.assign(static_cast<size_t>(len), 0);
        int32_t got = 0;
        __try
        {
            auto seek = reinterpret_cast<int32_t (*)(void *, int32_t, int32_t)>(
                *reinterpret_cast<void **>(vt + kFileSeek));
            auto read = reinterpret_cast<int32_t (*)(void *, void *, int32_t)>(
                *reinterpret_cast<void **>(vt + kFileRead));
            seek(file, 0, 0);
            while (got < len)
            {
                const int32_t n = read(file, out.data() + got, len - got);
                if (n <= 0)
                    break;
                got += n;
            }
            seek(file, pos, 0);  // leave the source exactly where the parser had it
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (got != len)
            return false;
        *outLen = static_cast<uint32_t>(len);
        *outPos = static_cast<uint32_t>(pos < 0 ? 0 : pos);
        return true;
    }

    // Keep a copy of the first of the engine's own memory-backed Files we see, to use as the shape of
    // the source we hand back. Building one field by field would mean inventing the parts of the class
    // that are not plain data; copying one the engine made avoids that entirely.
    void note_source_template(void *file)
    {
        if (g_source_template)
            return;
        __try
        {
            const uintptr_t vt = memfile_vtable();
            if (!vt || *reinterpret_cast<uintptr_t *>(file) != vt)
                return;
            std::memcpy(g_parse_source, file, sizeof(g_parse_source));
            g_source_template = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Point the parse at our bytes: in place when the source is the engine's own memory file (no
    // ownership changes hands), otherwise through a copy of one, which the reader then reads instead.
    //
    // `pos` is the position the source was at and MUST stay at. Rewinding it to 0 is what broke this:
    // the reader had already taken the header out of this source and buffers ahead of itself, so the
    // moment its window ran out it went on reading from byte 0 and parsed the movie's own header as a
    // tag. Everything downstream followed from that - the screen built from this movie never got a
    // display object, and the engine's walk over its children read through null. The file route could
    // not have this problem: there the buffer is replaced before a reader exists at all.
    //
    // Bytes the reader has already buffered stay the ORIGINAL movie's. That costs nothing: every edit
    // that early in the stream is a same-length one (a caption, a matrix, a font size), so at worst one
    // of those is missed, while the insertions - which are what would shift a tag boundary - all sit
    // deep inside the sprite defines, far past anything a 16KB window has reached.
    bool serve_to_parser(uint64_t reader, void *source, const uint8_t *bytes, uint32_t len, uint32_t pos)
    {
        if (pos > len)
            return false;
        __try
        {
            uint8_t *dst = reinterpret_cast<uint8_t *>(source);
            const uintptr_t vt = memfile_vtable();
            if (!vt || *reinterpret_cast<uintptr_t *>(source) != vt)
            {
                if (!g_source_template)
                    return false;
                dst = g_parse_source;
                *reinterpret_cast<uint32_t *>(dst + kMemFileRefs) = kNeverFree;
            }
            *reinterpret_cast<const uint8_t **>(dst + kMemFileBuffer) = bytes;
            *reinterpret_cast<uint32_t *>(dst + kMemFileSize) = len;
            *reinterpret_cast<uint32_t *>(dst + kMemFilePos) = pos;
            if (dst != source)
                *reinterpret_cast<void **>(reader + kReaderSource) = dst;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Are these bytes already ours? Covers this route being entered more than once for one movie:
    // rebuilding a rebuilt movie would try to add characters that are in it already.
    bool source_is_ours(void *source)
    {
        if (!source || g_parse_bytes.empty())
            return false;
        __try
        {
            return *reinterpret_cast<const uint8_t **>(reinterpret_cast<uint8_t *>(source) +
                                                       kMemFileBuffer) == g_parse_bytes.data();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool read_ctx(void *ctx, uint64_t *reader, void **source, uint32_t *total)
    {
        __try
        {
            const uintptr_t c = reinterpret_cast<uintptr_t>(ctx);
            uint64_t r = *reinterpret_cast<uint64_t *>(c + kCtxReader);
            if (!r)
                r = c + kCtxReaderInline;
            *reader = r;
            *source = *reinterpret_cast<void **>(r + kReaderSource);
            *total = *reinterpret_cast<uint32_t *>(c + kCtxTotalLen);
            return *source != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Move the loop's end-of-stream bound by the SAME amount the movie grew, instead of setting it to
    // our buffer's size. The engine derived that bound from the header it had already read, and its own
    // value is not the file's size - for the menu movie a 55600-byte file gives 55584, the length field's
    // convention less the trailer. Setting the raw size instead handed the parser 16 bytes it should
    // never see and it read the movie's trailer as a tag; the file route never had that problem because
    // the engine computed the bound from OUR header. Keeping the delta keeps whatever convention the
    // engine used, whatever the movie is.
    bool move_ctx_total(void *ctx, int64_t delta)
    {
        __try
        {
            auto *p = reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + kCtxTotalLen);
            const int64_t want = static_cast<int64_t>(*p) + delta;
            if (want <= 0 || want > 0x7FFFFFFF)
                return false;
            *p = static_cast<uint32_t>(want);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Which movie is this? The name from its definition answers it for a movie the game itself served,
    // but NOT for one a loader overrode: measured on Convergence, the movie ME3 serves in place of
    // 02_122 comes back with a name of unreadable bytes. So a name only ever RULES A MOVIE IN, and when
    // there is none the movie's own CONTENT decides - the authoring class name the menu screen's
    // SymbolClass carries, which no path or loader can distort.
    //
    // The MENU screen only. The map's two panels are added to the PARSED movie instead (goblin_gfx_probe
    // inject_map_panels), which derives the host sprite, the character it places and a free depth from
    // the movie itself - so rebuilding the map movie's bytes here would be the weaker of the two routes
    // (the byte transform knows the host sprite as a constant) and would only get in its way.
    constexpr const char *kMenuClassMark = "02_160_KeyConfiguration";

    // ── the separate movie definition: alias our URL onto the game's file ─────────────
    // The Scaleform file opener (GFx FileOpener::OpenFile on the engine's side: this, url as UTF-8,
    // two ints) turns `menu:/Win/<name>.gfx` into a memory-backed File by looking the path up in the
    // repository of files the menu preload loaded (a hash map keyed by the path, a pure lookup - a name
    // nobody preloaded finds nothing and the movie never comes up). For OUR name the lookup is done
    // under the game's name instead: same bytes, but the caller keys the definition it parses by the
    // URL it asked for, so it becomes a second definition. The flag tells the tag loop that the parse
    // about to run is ours - the only parse of this content since the startup preload.
    constexpr const char *kOurMovieName = "02_160_MfgSettings";
    constexpr const wchar_t *kOurMovieNameW = L"02_160_MfgSettings";
    constexpr const wchar_t *kMenuGameNameW = L"02_160_KeyConfiguration";
    // The world map's movie, same trick, different reason (2026-09-07): our map icons are injected
    // WHILE 02_120 parses (frames into sprite 171, bitmaps into the load context), so a DLL that
    // arrives after the game's parse - a late injector, a fast boot - has no icons and no way to add
    // them afterwards. With the map's job asking for a name of ours instead, the parse the map
    // instances is one that happens AFTER the request, i.e. after our hooks are live, whatever the
    // injection moment. The name keeps `02_120_worldmap` inside it: gfx_probe recognises the map's
    // movie by that substring of the (lower-cased) URL.
    constexpr const char *kOurWorldMapName = "02_120_WorldMap_Mfg";
    constexpr const wchar_t *kOurWorldMapNameW = L"02_120_WorldMap_Mfg";
    constexpr const char *kGameWorldMapName = "02_120_WorldMap";
    constexpr const wchar_t *kGameWorldMapNameW = L"02_120_WorldMap";
    struct MovieAlias { const char *ours; const char *game; bool menu_parse; };
    constexpr MovieAlias kAliases[] = {
        {kOurMovieName, kMenuClassMark, true},
        {kOurWorldMapName, kGameWorldMapName, false},
    };
    using OpenFileFn = void *(void *opener, const char *url, int a, int b);
    OpenFileFn *o_open_file = nullptr;
    std::atomic<bool> g_alias_armed{false};
    std::atomic<int> g_alias_pending{0};

    // The movie descriptor the job builders make and the job carries at +0x58.
    struct MovieDesc
    {
        uint32_t tag;   // 8
        uint8_t kind;   // 1 = key-binding form, 2 = pushed screen / the world map
        uint8_t pad[3];
        const wchar_t *name;
    };
    using NameToDefFn = void *(void *menuman, void *out, const MovieDesc *desc);
    using MoviePinFn = void(void *inner, const MovieDesc *desc);
    // The world map's file is not what its name says (2026-09-07, measured): with ERR under me3 the
    // path resolver (FUN_140d7b800: `menu:/Win/<name>.gfx`, then the FD4 device step where me3
    // substitutes its override) hands the loader an opaque token such as `\\me3??22`, and THAT is
    // the key the loaded file sits under. A name of ours resolves to a vanilla path nobody loaded.
    // So the resolver is hooked too: the first time it formats OUR world-map descriptor it also
    // formats the GAME's, records the URL that produced, and the opener alias maps our URL onto
    // that recorded one - the token under me3, the vanilla path without a loader.
    constexpr bool kWorldMapRedirect = true;
    NameToDefFn *o_name_to_def = nullptr;
    MoviePinFn *o_movie_pin = nullptr;
    using PathFmtFn = void *(void *out, const MovieDesc *desc);
    PathFmtFn *o_path_fmt = nullptr;
    std::string g_worldmap_game_url;            // UTF-8, as the opener will see it
    std::atomic<bool> g_worldmap_game_url_ready{false};
    // Same capture for the menu (2026-09-17, report 42). Hand-building the game's URL from ours by
    // string substitution is only right when the resolved path IS the plain mount path. Under me3
    // 0.13 the reporter's resolver hands out opaque tokens (`\me3??51`), and the file sits under
    // THAT - so our `data0:/menu/02_160_keyconfiguration.gfx` opened nothing in either casing while
    // his world map, which replays the recorded URL, opened fine in the same session.
    std::string g_menu_game_url;
    std::atomic<bool> g_menu_game_url_ready{false};

    // POD-only (SEH): the wide string a resolved DLString holds - data at +8 (heap pointer when the
    // capacity at +0x20 exceeds 7), length at +0x18 - narrowed into `out`.
    int read_dlstring_narrow(const void *str, char *out, size_t cap)
    {
        __try
        {
            const auto base = reinterpret_cast<uintptr_t>(str);
            const uint64_t capw = *reinterpret_cast<const uint64_t *>(base + 0x20);
            const uint64_t len = *reinterpret_cast<const uint64_t *>(base + 0x18);
            const wchar_t *w = capw > 7 ? *reinterpret_cast<const wchar_t *const *>(base + 8)
                                        : reinterpret_cast<const wchar_t *>(base + 8);
            if (!w || len == 0 || len >= cap) return 0;
            size_t k = 0;
            for (; k < len && k + 1 < cap; ++k) out[k] = static_cast<char>(w[k] & 0x7F);
            out[k] = 0;
            return static_cast<int>(k);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // POD-only (SEH): is this descriptor the GAME's own menu movie? Watching for it is what lets the
    // URL be recorded at the STARTUP PRELOAD, long before any menu open - so the alias already holds
    // the right URL the first time the player presses the key, and the fallback never has to fire.
    int is_game_menu_desc(const MovieDesc *desc)
    {
        __try
        {
            return desc && desc->name && wcscmp(desc->name, kMenuGameNameW) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // POD-only (SEH): is this descriptor ours for the menu?
    int is_our_menu_desc(const MovieDesc *desc)
    {
        __try
        {
            return desc && desc->name && wcscmp(desc->name, kOurMovieNameW) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // POD-only (SEH): is this descriptor ours for the world map?
    int is_our_worldmap_desc(const MovieDesc *desc)
    {
        __try
        {
            return desc && desc->name && wcscmp(desc->name, kOurWorldMapNameW) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void *path_fmt_detour(void *out, const MovieDesc *desc)
    {
        if (is_our_worldmap_desc(desc) && !g_worldmap_game_url_ready.load(std::memory_order_acquire))
        {
            MovieDesc game = *desc;
            game.name = kGameWorldMapNameW;
            o_path_fmt(out, &game); // the game's own resolution, override step included
            char narrow[256] = {};
            if (read_dlstring_narrow(out, narrow, sizeof(narrow)) > 0)
            {
                g_worldmap_game_url = narrow;
                g_worldmap_game_url_ready.store(true, std::memory_order_release);
                spdlog::info("[ownmovie] the game's world-map movie resolves to '{}' - our URL will alias onto it",
                             narrow);
            }
            else
                spdlog::warn("[ownmovie] could not read the game's world-map URL from the resolver");
            // then ours, assigned over the same out string (the resolver assigns, it does not construct)
        }
        // The menu, the same way and for the same reason. Ours is the only name the alias has to
        // map, so recording what the GAME's name resolves to here is the only way to learn the URL
        // the file actually sits under - the mount prefix, the case, and me3's token if it took the
        // path over. Building it from ours by substitution is a guess, and on the report-42 rig it
        // was the wrong one.
        if (is_our_menu_desc(desc) && !g_menu_game_url_ready.load(std::memory_order_acquire))
        {
            MovieDesc game = *desc;
            game.name = kMenuGameNameW;
            o_path_fmt(out, &game);
            char narrow[256] = {};
            if (read_dlstring_narrow(out, narrow, sizeof(narrow)) > 0)
            {
                g_menu_game_url = narrow;
                g_menu_game_url_ready.store(true, std::memory_order_release);
                spdlog::info("[ownmovie] the game's menu movie resolves to '{}' - our URL will alias onto it",
                             narrow);
            }
            else
                spdlog::warn("[ownmovie] could not read the game's menu URL from the resolver");
        }
        // The game resolving its OWN menu movie: the same URL, for free, and at the startup preload
        // rather than at the first key press. Taken from the resolver's own output, so there is
        // nothing to reconstruct and no second call to make.
        if (is_game_menu_desc(desc) && !g_menu_game_url_ready.load(std::memory_order_acquire))
        {
            void *r = o_path_fmt(out, desc);
            char narrow[256] = {};
            if (read_dlstring_narrow(out, narrow, sizeof(narrow)) > 0)
            {
                g_menu_game_url = narrow;
                g_menu_game_url_ready.store(true, std::memory_order_release);
                spdlog::info("[ownmovie] the game resolved its own menu movie to '{}' - recorded for the alias",
                             narrow);
            }
            return r;
        }
        return o_path_fmt(out, desc);
    }
    // The engine's on-demand file request by descriptor (FUN_140d77400 on 1.16): finds the descriptor
    // in its list at menuman+0x990, else formats `menu:/Win/<name>.gfx` and asks CSFile to load it.
    // Not hooked, CALLED: with a redirected name the engine requests OUR name's file, which does not
    // exist, and the Scaleform loader retries the opener forever (measured 2026-09-07: nine asks in five
    // seconds, the map never came up). So at every redirect the request is also made for the GAME'S
    // descriptor, so the real file is in the repository when the opener alias looks for it.
    using EnsureFileFn = void(void *menuman, const MovieDesc *desc);
    EnsureFileFn *p_ensure_file = nullptr;
    std::atomic<bool> g_worldmap_redirect{false};

    // POD-only (SEH): the engine's own request, with the engine's own descriptor.
    int request_game_file(void *menuman, const MovieDesc *game_desc)
    {
        if (!p_ensure_file || !menuman || !game_desc)
            return 0;
        __try
        {
            p_ensure_file(menuman, game_desc);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // POD-only (SEH): a descriptor naming the game's world-map movie becomes one naming ours.
    int redirect_worldmap_desc(const MovieDesc *desc, MovieDesc *copy)
    {
        if (!desc || !g_worldmap_redirect.load(std::memory_order_acquire))
            return 0;
        __try
        {
            if (desc->tag != 8 || !desc->name || wcscmp(desc->name, kGameWorldMapNameW) != 0)
                return 0;
            *copy = *desc;
            copy->name = kOurWorldMapNameW;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // CSMenuMan's name -> definition resolution (the job's load step). Redirected, the map's job
    // misses the name cache, goes through the opener alias below and parses a definition of its own.
    void *name_to_def_detour(void *menuman, void *out, const MovieDesc *desc)
    {
        MovieDesc copy{};
        if (redirect_worldmap_desc(desc, &copy))
        {
            // The game's file first (see request_game_file), then the definition under our name.
            const int req = request_game_file(menuman, desc);
            static std::atomic<int> s_logged{0};
            if (s_logged.fetch_add(1) < 2)
                spdlog::info("[ownmovie] world map asked for '{}' - resolving '{}' instead (game file "
                             "request: {})",
                             kGameWorldMapName, kOurWorldMapName,
                             req == 1 ? "made" : req == 0 ? "skipped" : "FAULTED");
            return o_name_to_def(menuman, out, &copy);
        }
        // Our MENU name arrives here too - it is the job's load step asking for the definition our
        // re-pointed job wants. This is the one place the engine hands us the CSMenuMan the file
        // request needs; asking with the pointer the menu-update detour caches instead FAULTED on
        // every open of report 42's run, because that is not the object this call reads its list
        // from. A request made here puts the game's own 02_160 in the repository before the alias
        // below looks for it.
        if (is_our_menu_desc(desc))
        {
            MovieDesc game = *desc;
            game.name = kMenuGameNameW;
            const int req = request_game_file(menuman, &game);
            static std::atomic<int> s_menu_logged{0};
            if (s_menu_logged.fetch_add(1) < 2)
                spdlog::info("[ownmovie] menu asked for '{}' - the game's file requested: {}",
                             kOurMovieName,
                             req == 1 ? "made" : req == 0 ? "skipped" : "FAULTED");
        }
        return o_name_to_def(menuman, out, desc);
    }

    // The keep-resident pin the map menu puts on its movie by NAME (timer -1 on the cache entry).
    // Redirected as well, or our definition would be the one the cache is free to drop.
    void movie_pin_detour(void *inner, const MovieDesc *desc)
    {
        MovieDesc copy{};
        if (redirect_worldmap_desc(desc, &copy))
        {
            o_movie_pin(inner, &copy);
            return;
        }
        o_movie_pin(inner, desc);
    }

    // Case-insensitive strstr: the URL that reaches the opener is the RESOLVED path, lower-cased by
    // the mount step (`menu:/Win/02_160_KeyConfiguration.gfx` arrives as
    // `data0:/menu/02_160_keyconfiguration.gfx` - the name the tag loop logs). The first build of
    // this hook compared case-sensitively, never matched, and the screen "never came up".
    const char *find_ci(const char *hay, const char *needle)
    {
        for (const char *p = hay; *p; ++p)
        {
            size_t k = 0;
            while (needle[k] && std::tolower(static_cast<unsigned char>(p[k])) ==
                                    std::tolower(static_cast<unsigned char>(needle[k])))
                ++k;
            if (!needle[k])
                return p;
        }
        return nullptr;
    }

    // ── diagnostic: who is on the stack ──────────────────────────────────────────────
    // Frames as module+offset, the crash log's convention, so a 1.17 address can be carried back
    // to the Ghidra project (1.16) by byte pattern. Used once per site, observation builds only.
    void log_stack_impl(const char *tag)
    {
        void *frames[24] = {};
        const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        std::string line;
        char buf[128];
        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE mod = nullptr;
            const auto addr = reinterpret_cast<uintptr_t>(frames[i]);
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(addr), &mod) && mod)
            {
                wchar_t path[MAX_PATH] = {};
                GetModuleFileNameW(mod, path, MAX_PATH);
                const wchar_t *base = wcsrchr(path, L'\\');
                base = base ? base + 1 : path;
                char name[64] = {};
                for (int k = 0; k < 63 && base[k]; ++k) name[k] = static_cast<char>(base[k] & 0x7F);
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, " #%u %s+0x%llX", i, name,
                            static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(mod)));
            }
            else
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, " #%u 0x%llX", i, static_cast<unsigned long long>(addr));
            line += buf;
        }
        spdlog::info("[ownmovie] stack {}:{}", tag, line);
    }

    // GFxLoader::CreateMovie (stub FUN_14112b130 on 1.16; tail-jumps into the real loader). Hooked
    // for OBSERVATION: which URLs the loader is asked for, with what flags, and what comes back - the
    // world map's URL never reached the file opener in the stock flow (2026-09-07), so its file gets to
    // the parser another way, and this is the level above the opener.
    using CreateMovieFn = void *(void *loader, const char *url, uint32_t flags, void *a4, uint64_t a5, uint64_t a6);
    CreateMovieFn *o_create_movie = nullptr;
    void *create_movie_detour(void *loader, const char *url, uint32_t flags, void *a4, uint64_t a5, uint64_t a6)
    {
        void *def = o_create_movie(loader, url, flags, a4, a5, a6);
        static std::atomic<int> s_count{0};
        const int n = s_count.fetch_add(1);
        const bool map = url && find_ci(url, "02_120_worldmap");
        if ((goblin::config::debugLogging && n < 140) || map)
            spdlog::info("[ownmovie] CreateMovie #{} '{}' flags 0x{:X} a4 0x{:X}: def 0x{:X}", n,
                         url ? url : "(null)", flags, reinterpret_cast<uintptr_t>(a4),
                         reinterpret_cast<uintptr_t>(def));
        if (map && goblin::config::debugLogging)
        {
            static std::atomic<int> s_stacks{0};
            if (s_stacks.fetch_add(1) < 2)
                log_stack_impl("CreateMovie(02_120)");
        }
        return def;
    }

    // ── diagnostic: what the preloaded-file repository holds ─────────────────────────────
    // The repository the opener consults (singleton, hash map at +0x78: bucket array @map+0x20,
    // bucket count @map+0x1C, node {hash @+8, std::wstring path @+0x18 (heap ptr when cap @+0x30 > 7),
    // next @+0x50, blob @+0x78, size u32 @+0x80}). The singleton slot is read off the hooked opener's
    // own body: its `cmp qword ptr [rip+slot], 0 ; jz` is the only instruction of that shape there.
    // Walked only when an aliased open came back empty, a few times per run, to say whether the
    // game's file is there at all and under which key (2026-09-07: the world map never came up).
    uintptr_t g_open_file_fn = 0;
    uintptr_t g_repo_slot = 0;

    uintptr_t repo_slot_from_opener(uintptr_t fn)
    {
        if (!fn) return 0;
        __try
        {
            const auto *p = reinterpret_cast<const uint8_t *>(fn);
            for (size_t i = 0; i + 10 < 0x300; ++i)
            {
                if (p[i] == 0x48 && p[i + 1] == 0x83 && p[i + 2] == 0x3D && p[i + 7] == 0x00 &&
                    p[i + 8] == 0x0F && p[i + 9] == 0x84)
                {
                    int32_t disp = 0;
                    std::memcpy(&disp, p + i + 3, 4);
                    return fn + i + 8 + static_cast<intptr_t>(disp);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return 0;
    }

    // Copies the matching entries into `out` (POD, SEH); returns the count seen.
    struct RepoEntry { wchar_t path[96]; uint64_t blob; uint32_t size; };
    int walk_repo(RepoEntry *out, int cap, uint32_t *bucket_count, uint32_t *nodes_total)
    {
        int n = 0;
        *bucket_count = 0;
        *nodes_total = 0;
        if (!g_repo_slot) return -1;
        __try
        {
            const uintptr_t single = *reinterpret_cast<const uintptr_t *>(g_repo_slot);
            if (!single) return -2;
            const uintptr_t map = single + 0x78;
            const uint32_t buckets = *reinterpret_cast<const uint32_t *>(map + 0x1C);
            const uintptr_t arr = *reinterpret_cast<const uintptr_t *>(map + 0x20);
            *bucket_count = buckets;
            if (!arr || buckets == 0 || buckets > 65536) return -3;
            for (uint32_t bi = 0; bi < buckets; ++bi)
            {
                uintptr_t node = *reinterpret_cast<const uintptr_t *>(arr + bi * 8);
                for (int guard = 0; node && guard < 64; ++guard)
                {
                    ++*nodes_total;
                    const uint64_t capw = *reinterpret_cast<const uint64_t *>(node + 0x30);
                    const wchar_t *s = capw > 7 ? *reinterpret_cast<const wchar_t *const *>(node + 0x18)
                                                : reinterpret_cast<const wchar_t *>(node + 0x18);
                    bool hit = false;
                    for (int k = 0; s && k < 200 && s[k]; ++k)
                        if (s[k] == L'0' && s[k + 1] == L'2' && s[k + 2] == L'_' && s[k + 3] == L'1')
                        {
                            hit = true;
                            break;
                        }
                    if (hit && n < cap)
                    {
                        int k = 0;
                        for (; k < 95 && s[k]; ++k) out[n].path[k] = s[k];
                        out[n].path[k] = 0;
                        out[n].blob = *reinterpret_cast<const uint64_t *>(node + 0x78);
                        out[n].size = *reinterpret_cast<const uint32_t *>(node + 0x80);
                        ++n;
                    }
                    node = *reinterpret_cast<const uintptr_t *>(node + 0x50);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -4;
        }
        return n;
    }

    void dump_repo_for(const char *url, const char *when = "after an empty answer for")
    {
        static std::atomic<int> s_dumps{0};
        if (s_dumps.fetch_add(1) >= 6) return;
        if (!g_repo_slot) g_repo_slot = repo_slot_from_opener(g_open_file_fn);
        static RepoEntry entries[24];
        uint32_t buckets = 0, nodes = 0;
        const int n = walk_repo(entries, 24, &buckets, &nodes);
        spdlog::info("[ownmovie] repository {} '{}': slot 0x{:X}, {} bucket(s), "
                     "{} node(s), {} with '02_1' in the path{}",
                     when, url, g_repo_slot, buckets, nodes, n < 0 ? 0 : n,
                     n < 0 ? " (walk failed)" : "");
        for (int i = 0; i < n; ++i)
        {
            char narrow[96];
            int k = 0;
            for (; k < 95 && entries[i].path[k]; ++k) narrow[k] = static_cast<char>(entries[i].path[k] & 0x7F);
            narrow[k] = 0;
            spdlog::info("[ownmovie]   '{}' blob=0x{:X} size={}", narrow, entries[i].blob, entries[i].size);
        }
    }

    // Both the opener alias (on an empty answer) and any caller that finds the route unusable end
    // up here. Idempotent: the first call logs, the rest are silent.
    void retire_separate(const char *why)
    {
        if (!g_alias_armed.exchange(false, std::memory_order_acq_rel))
            return;
        spdlog::warn("[ownmovie] separate menu definition retired ({}) - our screens now share the "
                     "game's own 02_160 definition",
                     why ? why : "no reason given");
    }

    void *open_file_detour(void *opener, const char *url, int a, int b)
    {
        // OBSERVATION (2026-09-07, while the world map's file route is being worked out): the first
        // opens of the run, and the repository at the first ask for 02_120, so the stock flow is on
        // record - which URL the map's file is asked under, with what flags, and whether it is there.
        {
            static std::atomic<int> s_count{0};
            const int n = s_count.fetch_add(1);
            if (goblin::config::debugLogging && n < 140 && !(url && find_ci(url, "_mfg"))) // ours take the alias path below
            {
                void *file = o_open_file(opener, url, a, b);
                spdlog::info("[ownmovie] open #{} '{}' (flags {} {}): {}", n, url ? url : "(null)", a, b,
                             file ? "file" : "NOTHING");
                if (url && find_ci(url, "02_120_worldmap") && !find_ci(url, "_mfg"))
                    dump_repo_for(url);
                return file;
            }
        }
        if (url)
        {
            // What the opener is asked for around our screen, a few times, so the shape of the URL
            // is on record (mount prefix, case) when the alias ever stops matching.
            if (goblin::config::debugLogging && find_ci(url, "02_160"))
            {
                static std::atomic<int> s_seen{0};
                if (s_seen.fetch_add(1) < 6)
                    spdlog::info("[ownmovie] opener url: '{}'", url);
            }
            for (const MovieAlias &al : kAliases)
            {
                const char *hit = find_ci(url, al.ours);
                if (!hit)
                    continue;
                // The URL arrives resolved AND lower-cased, so the game's name goes in lower-cased too:
                // the repository folds case, but the direct device route the opener can take instead
                // (flag `a` set) looks the path up by a hash of the bytes as given.
                std::string real;
                if (!al.menu_parse && g_worldmap_game_url_ready.load(std::memory_order_acquire))
                    real = g_worldmap_game_url; // whatever the game's own resolution produced (me3 token, or the vanilla path)
                else if (al.menu_parse && g_menu_game_url_ready.load(std::memory_order_acquire))
                    real = g_menu_game_url; // likewise for the menu - recorded, never rebuilt
                else
                {
                    real.assign(url, static_cast<size_t>(hit - url));
                    for (const char *c = al.game; *c; ++c)
                        real += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
                    real += hit + std::strlen(al.ours);
                }
                if (al.menu_parse)
                    g_alias_pending.store(1, std::memory_order_release);
                void *file = o_open_file(opener, real.c_str(), a, b);
                spdlog::info("[ownmovie] opener asked for '{}' - answered with the game's '{}' (flags {} {}): {}",
                             url, real, a, b, file ? "file" : "NOTHING");
                // The name went in lower-cased because that is how the mount step hands URLs over
                // and how the repository keys them. The DIRECT DEVICE route does not fold case
                // though - it hashes the bytes as given - so on an install serving this out of the
                // packed archive rather than a UXM-unpacked folder, the lower-cased name can hash to
                // nothing while the authored one resolves. One retry, failure path only.
                if (!file && al.menu_parse)
                {
                    std::string authored(url, static_cast<size_t>(hit - url));
                    authored += al.game;
                    authored += hit + std::strlen(al.ours);
                    file = o_open_file(opener, authored.c_str(), a, b);
                    spdlog::info("[ownmovie] retried with the authored casing '{}': {}", authored,
                                 file ? "file" : "NOTHING");
                }
                if (!file)
                {
                    dump_repo_for(real.c_str());
                    // Nothing came back, so the definition our name would have been cached under
                    // cannot be built and the screen this open was for will never come up. The
                    // request we make at the re-point should have put the game's file in the
                    // repository by now; if it lands late, the NEXT press finds it, so one empty
                    // answer is not enough to give up on. A second one is: from there the route is
                    // retired, the next open leaves the game's name on the job, and the screen
                    // shares the game's own definition - what every build before 2.1.4 did.
                    if (al.menu_parse)
                    {
                        g_alias_pending.store(0, std::memory_order_release);
                        static std::atomic<int> s_empty{0};
                        if (s_empty.fetch_add(1) >= 1)
                            retire_separate("the game's own movie file did not open under our name, twice");
                    }
                }
                return file;
            }
        }
        return o_open_file(opener, url, a, b);
    }

    bool content_is_menu(const uint8_t *b, uint32_t len)
    {
        const size_t mlen = std::strlen(kMenuClassMark);
        if (len < mlen)
            return false;
        for (uint32_t i = 0; i + mlen <= len; ++i)
            if (b[i] == '0' && std::memcmp(b + i, kMenuClassMark, mlen) == 0)
                return true;
        return false;
    }

    void tag_loop_detour(void *movieData, void *ctx, void *arg3)
    {
        uint64_t reader = 0;
        void *source = nullptr;
        uint32_t total = 0;
        char name[192] = {};
        bool named_mine = false, nameless = true;
        if (ctx && read_ctx(ctx, &reader, &source, &total))
        {
            note_source_template(source);
            nameless = !goblin::gfx_probe::movie_name(ctx, name, sizeof(name));
            named_mine = !nameless && contains_ci(name, kMovieName);
            // Under debug logging, every movie the PARSER is given, with a running count. This is the
            // measure that matters now: unlike the opener, this route sees the ones a loader overrides
            // too, so the count includes them.
            if (goblin::config::debugLogging)
                spdlog::info("[ownmovie] parsing '{}' (#{})", nameless ? "(no name)" : name,
                             g_movies_seen.fetch_add(1, std::memory_order_relaxed) + 1);
        }
        // A named movie that is not ours needs no bytes read at all. One without a name has to be looked
        // at, because that is exactly what an overridden movie looks like from here.
        //
        // NO "IS THIS OUR OPEN?" TERM HERE, and it is not an oversight. A g_armed counter armed in
        // open_screen was added to this gate on 2026-07-30 so the player's own Key Assignments screen
        // would keep the stock layout. Measured in game the next run: the gate can never be satisfied.
        // The engine parses every menu movie ONCE, in the startup preload burst - 02_160 came through
        // as movie #98, 21 seconds before the first press of the menu key - and every later screen is
        // an INSTANCE of that one parse. So with the counter in the gate the transform never ran at
        // all: no centring shift, no row pitch, no extra row clips, no category icons, no logo in the
        // header, and the row painting then resolved paths into clips that do not exist (an SEH storm
        // on close). Leaving the player's screen alone has to be decided per INSTANCE, at screen
        // level - it cannot be decided at parse time, because there is only one parse for both.
        if ((!named_mine && !nameless) || !goblin::config::native_menu_enabled())
        {
            o_tag_loop(movieData, ctx, arg3);
            return;
        }
        // WITH the separate definition (the opener hook is live) the shared parse - the game's own
        // 02_160, preloaded at startup and instanced by the player's screen - passes untouched, and
        // only the parse our aliased open just triggered is rebuilt. Without it (the opener pattern
        // is dead on this exe) the old behaviour stands: transform the one shared parse.
        const bool separate = g_alias_armed.load(std::memory_order_acquire);
        if (separate && g_alias_pending.load(std::memory_order_acquire) == 0)
        {
            o_tag_loop(movieData, ctx, arg3);
            return;
        }
        std::vector<uint8_t> src;
        uint32_t len = 0, at = 0;
        if (source_is_ours(source) || !read_whole_movie(source, &len, &at, src))
        {
            o_tag_loop(movieData, ctx, arg3);
            return;
        }
        if (!named_mine && !content_is_menu(src.data(), len))
        {
            o_tag_loop(movieData, ctx, arg3);
            return;
        }
        if (separate)
        {
            g_alias_pending.store(0, std::memory_order_release); // this is the parse the alias was for
            spdlog::info("[ownmovie] separate definition: transforming our own parse of '{}'",
                         nameless ? "(no name)" : name);
        }
        std::vector<uint8_t> built;
        if (!rebuild(src.data(), len, built) || built.empty())
        {
            o_tag_loop(movieData, ctx, arg3);
            return;
        }
        g_parse_bytes.swap(built);
        const int64_t delta = static_cast<int64_t>(g_parse_bytes.size()) - static_cast<int64_t>(len);
        if (!serve_to_parser(reader, source, g_parse_bytes.data(),
                             static_cast<uint32_t>(g_parse_bytes.size()), at) ||
            !move_ctx_total(ctx, delta))
        {
            // Every rebuild step above has already logged by now, so WITHOUT this line a failure
            // here reads like success minus its last line - which is how a derived-vtable bug
            // stayed invisible for a whole test round. The movie parses once, so this says it once.
            spdlog::warn("[ownmovie] '{}' was rebuilt but the parser could not be pointed at it "
                         "(memory-file vtable {}) - the screen keeps the stock movie",
                         name, memfile_vtable() ? "known" : "underived");
            o_tag_loop(movieData, ctx, arg3);
            return;
        }
        // Not a ternary: rebuild() returns false on every path that leaves g_icons_added unset
        // (see the icon splice in rebuild), so control only reaches here WITH icons. The other arm
        // was a string in .rdata for a state that cannot happen.
        g_status = "icons spliced into the movie";
        // `read ahead` is the position the source was at: everything before it the parser already has
        // buffered from the original movie, so it wants to be a long way short of where our first
        // insertion lands. If it ever is not, a tag boundary would move under the parser and this line
        // is where that shows.
        spdlog::info("[ownmovie] '{}' transformed on the way into the parser: {} bytes in, {} out, "
                     "{} tags, {} trailing (read ahead {}, stream bound {})",
                     name, len, g_parse_bytes.size(), g_tag_count,
                     g_tail_offset && g_tail_offset < len ? len - g_tail_offset : 0,
                     at, total);
        spdlog::info("[ownmovie] caption block dropped from the panel: {}",
                     g_caption_block_dropped ? "yes" : "no");
        o_tag_loop(movieData, ctx, arg3);
    }

}

void goblin::own_movie::install()
{
    try
    {
        // The parse route. AOB rather than an RVA, and the pattern is the loop's own prologue: it saves
        // arg3, takes the reader from ctx+0x418 and falls back to the inline one at ctx+0x50 - the three
        // facts this route is built on, so a game update that changes them breaks the pattern instead of
        // silently pointing us at something else.
        auto *fn = modutils::hook<TagLoopFn>(
            {.aob = "4C 89 44 24 18 53 55 56 57 41 55 41 56 48 83 EC 68 48 8B AA 18 04 00 00 "
                    "49 8B F8 4C 8B EA 4C 8B F1 48 85 ED"},
            tag_loop_detour, o_tag_loop);
        g_ready.store(true, std::memory_order_release);
        spdlog::info("[ownmovie] movie parse route ready @ 0x{:X}", reinterpret_cast<uintptr_t>(fn));
        // The file-opener route for the separate definition. Its own try: a dead pattern here only
        // costs the separation (the shared parse is transformed as before), not the menu.
        try
        {
            // GFx FileOpener::OpenFile: seven pushes, the 0xB0 frame, the stack cookie, then the
            // argument moves (r15d=r9d, r14d=r8d, rbx=url, rsi=this) and the singleton load.
            // Unique on 1.16 / 1.17 (checked against both exe files and the live one, 2026-09-06).
            auto *op = modutils::hook<OpenFileFn>(
                {.aob = "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 "
                        "48 C7 45 B7 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 1F 45 8B F9 "
                        "45 8B F0 48 8B DA 48 8B F1 48 8B 0D ?? ?? ?? ?? 48 85 C9 75"},
                open_file_detour, o_open_file);
            g_open_file_fn = reinterpret_cast<uintptr_t>(op);
            g_repo_slot = repo_slot_from_opener(g_open_file_fn);
            g_alias_armed.store(true, std::memory_order_release);
            // OBSERVATION hook on the loader's CreateMovie stub (see create_movie_detour). Its own try:
            // a miss costs only the observation.
            try
            {
                modutils::hook<CreateMovieFn>(
                    {.aob = "45 8B D0 48 8B C1 48 85 D2 74 1A 80 3A 00 74 15 48 8B 49 08 48 85 C9 74 0C "
                            "44 8B 40 18 45 0B C2 E9"},
                    create_movie_detour, o_create_movie);
                spdlog::info("[ownmovie] CreateMovie observation hooked");
            }
            catch (const std::exception &e)
            {
                spdlog::info("[ownmovie] CreateMovie observation unavailable ({})", e.what());
            }
            spdlog::info("[ownmovie] file-opener route ready @ 0x{:X}: our screens get their own "
                         "definition ('{}')",
                         reinterpret_cast<uintptr_t>(op), kOurMovieName);
            // The world map's own definition rides on the same alias: its two name-keyed entry
            // points (resolution + the keep-resident pin) are redirected to our name. Either pattern
            // dead = no redirect, and the map keeps depending on our hooks being live before its parse.
            if (!kWorldMapRedirect)
                spdlog::info("[ownmovie] world-map definition route is switched off in this build");
            else try
            {
                // FUN_140d77400 (1.16): the on-demand file request by descriptor - resolved, not hooked;
                // the redirect calls it with the game's descriptor so the real file gets loaded.
                p_ensure_file = reinterpret_cast<EnsureFileFn *>(modutils::scan<void>(
                    {.aob = "4C 8B DC 57 48 81 EC 90 00 00 00 49 C7 43 B8 FE FF FF FF 49 89 5B 18 49 89 73 20 "
                            "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 00 00 00 48 8B FA 48 8B 99 98 09 00 00 "
                            "48 8D B1 90 09 00 00 4C 8B C2 49 8D 53 98 48 8B CE E8 ?? ?? ?? ?? 48 39 18"}));
                // FUN_140d7b800 (1.16): descriptor -> `menu:/Win/<name>.gfx` -> resolved URL (this is where
                // a loader's override lands). Hooked so our world-map descriptor learns the game's URL.
                modutils::hook<PathFmtFn>(
                    {.aob = "48 8B C4 55 57 41 56 48 8D 68 A8 48 81 EC 40 01 00 00 48 C7 44 24 60 FE FF FF FF "
                            "48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 48 8B FA 48 8B D9 "
                            "48 89 4C 24 68 33 F6 89 74 24 30 4C 8B 42 08 48 8D 15 ?? ?? ?? ?? 48 8D 4D C8 E8 ?? ?? ?? ?? "
                            "90 48 8D 50 08 48 83 7A 18 08 72 03 48 8B 12"},
                    path_fmt_detour, o_path_fmt);
                // FUN_140d7a630 (1.16): CSMenuMan movie def by descriptor name; prologue with the 0x2E0
                // frame, the cookie and the (menuman, out, desc) moves. Unique on 1.16 / 1.17.
                auto *nd = modutils::hook<NameToDefFn>(
                    {.aob = "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 20 FE FF FF 48 81 EC E0 02 00 00 "
                            "48 C7 44 24 60 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 D0 01 00 00 "
                            "49 8B C0 48 89 44 24 50 48 8B FA 48 8B D9 48 89 4C 24 38 48 89 54 24 68 33 F6 "
                            "89 74"},
                    name_to_def_detour, o_name_to_def);
                // The pin's body (the exported stub is a 13-byte thunk): walks the list at inner+0xD00
                // comparing entry names with desc->name. Unique on 1.16 / 1.17.
                auto *pin = modutils::hook<MoviePinFn>(
                    {.aob = "4C 8B 81 00 0D 00 00 4C 8B DA 4C 8B D1 49 8B 00 49 3B C0 74 59 4C 8B 40 10 "
                            "4D 85 C0 74 44 41 0F 10 48 20 4D 8B 4B 08 66 0F 73 D9 08 66 48 0F 7E C9 "
                            "4C 2B C9 0F 1F 40 00 0F 1F 84 00 00 00 00 00 44 0F B7 01 42 0F B7 14 09 "
                            "44 2B C2 75 08 48 83"},
                    movie_pin_detour, o_movie_pin);
                g_worldmap_redirect.store(true, std::memory_order_release);
                spdlog::info("[ownmovie] world-map definition route ready (resolve @ 0x{:X}, pin @ 0x{:X}): "
                             "the map parses '{}' after our hooks, whatever the injection moment",
                             reinterpret_cast<uintptr_t>(nd), reinterpret_cast<uintptr_t>(pin),
                             kOurWorldMapName);
            }
            catch (const std::exception &e)
            {
                spdlog::warn("[ownmovie] world-map definition route unavailable ({}) - map icons still "
                             "need our hooks live before the game parses 02_120",
                             e.what());
            }
        }
        catch (const std::exception &e)
        {
            spdlog::warn("[ownmovie] file-opener route unavailable ({}) - our screens share the "
                         "game's own movie definition (the transform reaches its screen too)",
                         e.what());
        }
    }
    catch (const std::exception &e)
    {
        g_status = "the movie tag loop could not be found";
        spdlog::warn("[ownmovie] parse route unavailable ({}) - the menu screen keeps the movie the "
                     "game shipped",
                     e.what());
    }
}

bool goblin::own_movie::available() { return g_ready.load(std::memory_order_acquire); }

const wchar_t *goblin::own_movie::menu_movie_name() { return kOurMovieNameW; }

void goblin::own_movie::retire_separate_movie(const char *why) { retire_separate(why); }
bool goblin::own_movie::separate_movie_armed() { return g_alias_armed.load(std::memory_order_acquire); }
void goblin::own_movie::log_stack(const char *tag) { log_stack_impl(tag); }
const wchar_t *goblin::own_movie::worldmap_movie_name() { return kOurWorldMapNameW; }
bool goblin::own_movie::worldmap_redirect_armed() { return g_worldmap_redirect.load(std::memory_order_acquire); }

// arm() / disarm() stood here: a nesting counter the host raised around its own screen open, so the
// transform could tell our load of 02_160 from the player's. It is gone because there is no such
// thing as "our load" - see the note in tag_loop_detour: the movie is parsed once, in the startup
// preload, and every screen afterwards instances that parse.

const char *goblin::own_movie::status() { return g_status; }
