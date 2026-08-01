#include "goblin_own_movie.hpp"

#include "goblin_config.hpp"
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
    // ── engine entry points (anchored in tools/rva_anchors.py) ───────────────────────
    // CS::CSScaleformFileOpener::OpenFile(this, const char* name, int flags, int mode)
    // returns a Scaleform File*. The name is NARROW here (the CS layer converts the wide
    // screen name), which the tail of the function makes plain: it tests `cmp byte [rbx], 0`
    // on the second argument.
    constexpr uintptr_t kOpenFile = 0xD6B4D0;

    // The vtable of the engine's memory-backed File. Movies come out of the archives as one
    // blob and are handed to Scaleform through this class, whose layout the constructor at
    // 0xCE7BB0 spells out:
    //   +0x00 vptr   +0x08 refcount   +0x10 path string
    //   +0x18 buffer  +0x20 size (u32)  +0x24 position (u32)
    // Because the loader only ever reads through those fields, re-pointing them at a buffer
    // of ours is all it takes to have our bytes parsed - no File implementation of our own,
    // no lifetime games with an object the engine allocated.
    constexpr uintptr_t kMemFileVtable = 0x2BA4C80;
    constexpr size_t kMemFileBuffer = 0x18;
    constexpr size_t kMemFileSize = 0x20;
    constexpr size_t kMemFilePos = 0x24;

    // The screen's movie. Matched as a case-insensitive substring on the SHORT id, because
    // the opener is handed a path built around it and we do not want to guess at the casing
    // or the extension the CS layer settles on.
    constexpr const char *kMovieName = "02_160";
    // The world map, transformed for a second reason: it gets ONE extra named placement so the
    // mod has a hover panel of its own (see kTipName below).
    constexpr const char *kMapMovieName = "02_120";

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

    bool name_matches(const char *name) { return contains_ci(name, kMovieName); }
    bool map_name_matches(const char *name) { return contains_ci(name, kMapMovieName); }

    // ── our own hover panel in the world map ─────────────────────────────────────────
    // The game's tooltip is Body/PlaceName (sprite cid 225: State_0 / State_1, eight lines each,
    // one line = cid 220 with a backdrop and a plain-text EditText). We want OUR OWN panel with
    // our own position and line count, and the game's left completely alone - so nothing is
    // copied or redefined: a SECOND named placement of the very same cid 225 goes into Body at a
    // depth above everything it holds. One tag, no new characters, no art duplication.
    constexpr uint16_t kMapBodyCid = 241;    // Body
    constexpr uint16_t kTipCid = 225;        // the tooltip sprite (State_0 + State_1)
    constexpr uint16_t kTipDepth = 106;      // Body's own children top out at 104
    constexpr const char *kTipName = "MfgTip";
    // A second panel of ours, for the focus banner in the corner. Same sprite, own depth+name,
    // so the two are independent of each other and of the game's.
    constexpr uint16_t kBannerDepth = 108;
    constexpr const char *kBannerName = "MfgBanner";
    int g_tip_placed = 0;

    // PlaceObject2: flags, depth u16, char u16, MATRIX, name. Flags 0x26 = HasCharacter |
    // HasMatrix | HasName. The matrix is the minimal identity one (no scale, no rotate, a
    // 1-bit zero translate) because the runtime sets the position anyway.
    size_t build_place(uint8_t *out, uint16_t depth, const char *name)
    {
        // Parked FAR off-screen (translate -20000, -20000 px), not at the origin: a placement
        // has no visibility flag, so an origin-placed panel renders for the frames between the
        // map opening and our first update - in game that was a red block flashing in the
        // corner. The runtime sets the position before showing it, which overwrites this.
        // MATRIX: HasScale 0, HasRotate 0, NTranslateBits 20, tx = ty = -400000 twips.
        static const uint8_t kParkedMatrix[6] = {0x29, 0x3C, 0xB0, 0x13, 0xCB, 0x00};
        size_t n = 0;
        out[n++] = 0x26;
        out[n++] = static_cast<uint8_t>(depth & 0xFF);
        out[n++] = static_cast<uint8_t>(depth >> 8);
        out[n++] = static_cast<uint8_t>(kTipCid & 0xFF);
        out[n++] = static_cast<uint8_t>(kTipCid >> 8);
        for (unsigned char b : kParkedMatrix)
            out[n++] = b;
        for (const char *c = name; *c; ++c)
            out[n++] = static_cast<uint8_t>(*c);
        out[n++] = 0;
        return n;
    }

    // One tagged PlaceObject2 appended to `dst`. The size is always short-form here.
    size_t append_place_tag(uint8_t *dst, uint16_t depth, const char *name)
    {
        uint8_t body[40];
        const size_t len = build_place(body, depth, name);
        const uint16_t th = static_cast<uint16_t>((26u << 6) | (len & 0x3F));
        dst[0] = static_cast<uint8_t>(th & 0xFF);
        dst[1] = static_cast<uint8_t>(th >> 8);
        std::memcpy(dst + 2, body, len);
        return len + 2;
    }

    using OpenFileFn = void *(void *self, const char *name, int flags, int mode);
    OpenFileFn *o_open_file = nullptr;

    std::atomic<int> g_armed{0};
    std::atomic<bool> g_ready{false};

    // Our bytes must outlive the load: the File does not copy them, it points at them.
    std::vector<uint8_t> g_served;
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
    int g_captions_renamed = 0;

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

    // Every character id the movie defines, so we can prove our additions collide with none.
    // Define tags all start with a u16 character id; that is the only field we need.
    bool cid_range_free(const uint8_t *src, const std::vector<Tag> &tags, uint16_t lo,
                        uint16_t hi)
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
            if (cid >= lo && cid <= hi)
                return false;
        }
        return true;
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
    constexpr uint16_t kRowSectionSpriteCid = 198;
    constexpr bool kDropRowPanel = false;
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

    // Splice our icon characters in and swap the row clip for the version that places them.
    // Both pieces are generated at build time (tools/generate_menu_icon_tags.py, which
    // re-parses its own output before emitting it), so all that is left here is to find the
    // row tag and prove the movie is the one those bytes were built against.
    // Which construction to splice comes from the ini (native_menu_icons), because the two
    // candidates have to be comparable without a rebuild:
    //   1 = one strip of all icons behind a real DefineShape mask, picked by shifting the strip
    //   2 = one named child per icon, all baked invisible, the chosen one scaled up
    // Neither uses frames: a timeline cannot be stopped from a tag stream, so a multi-frame
    // sprite animates in every instance we do not reach. Variant 1 previously faulted inside
    // Scaleform when its mask was an image character - it now gets a genuine shape.
    bool add_menu_icons(const uint8_t *src, const std::vector<Tag> &tags,
                        std::vector<uint8_t> &out, const char **why)
    {
        namespace mi = goblin::menu_icon_tags;
        const uint8_t variant = goblin::config::nativeMenuIcons;
        if (variant != 1 && variant != 2)
        {
            *why = "icons off (native_menu_icons)";
            return false;
        }
        const unsigned char *blob = variant == 1 ? mi::ICON_BLOB_A : mi::ICON_BLOB_B;
        const size_t blob_len = variant == 1 ? mi::ICON_BLOB_A_LEN : mi::ICON_BLOB_B_LEN;
        const unsigned char *rowtag = variant == 1 ? mi::ROW_TAG_A : mi::ROW_TAG_B;
        const size_t rowtag_len = variant == 1 ? mi::ROW_TAG_A_LEN : mi::ROW_TAG_B_LEN;
        const Tag *row = nullptr;
        for (const Tag &t : tags)
        {
            if (t.code != 39 || t.length < 2) // DefineSprite
                continue;
            uint16_t cid = 0;
            std::memcpy(&cid, src + t.offset, 2);
            if (cid == mi::ROW_CID)
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
        if (row->length != mi::ORIG_ROW_BODY_LEN)
        {
            *why = "row clip differs from the one the icons were built against";
            return false;
        }
        if (!cid_range_free(src, tags, mi::FIRST_CID, mi::LAST_CID))
        {
            *why = "our character ids are already taken in this movie";
            return false;
        }
        // Our defines go immediately before the row clip, which uses them. Both are define
        // tags, so this lands before the movie's first frame either way.
        const size_t row_start = row->offset - (row->long_form ? 6u : 2u);
        for (const Tag &t : tags)
        {
            const size_t start = t.offset - (t.long_form ? 6u : 2u);
            if (start == row_start)
            {
                out.insert(out.end(), blob, blob + blob_len);
                out.insert(out.end(), rowtag, rowtag + rowtag_len);
                continue;
            }
            if (t.code == 39 && t.length >= 2)
            {
                uint16_t cid = 0;
                std::memcpy(&cid, src + t.offset, 2);
                if (cid == kBgSpriteCid && emit_sprite_without(out, t, src, kCaptionSpriteCid))
                {
                    ++g_caption_block_dropped;
                    continue;
                }
                if (kDropRowPanel && cid == kRowSectionSpriteCid &&
                    emit_sprite_without(out, t, src, kBgSpriteCid))
                {
                    spdlog::info("[ownmovie] row panel dropped: BG sprite {} removed from the row "
                                 "section {}", kBgSpriteCid, kRowSectionSpriteCid);
                    continue;
                }
            }
            emit_tag(out, t, src);
        }
        return true;
    }

    // The key-binding screen labels two of its columns ("Keyboard" over the first bind and
    // "Mouse" over the second) with EditText fields whose FMG id is encoded IN THE INSTANCE
    // NAME - StaticText_280005 / StaticText_280006 (gfx_02_160_dump.txt line 917). On our
    // settings list those headers mean nothing, and in this client they draw as tofu boxes
    // because the fields are 22pt while the rest of the menu is 24pt.
    //
    // They cannot be hidden at runtime: they sit inside sprite 168, which frame 2 of
    // KeySetting/BG places as an UNNAMED child, and the engine resolves paths by name only -
    // which is why a runtime pass over eight candidate paths found nothing. So the name is
    // taken away instead: renaming them to something the name-driven populator does not match
    // leaves the fields empty. The replacements are the SAME LENGTH, so this is an in-place
    // patch - no tag length changes, nothing after them shifts.
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
    // (the same rule rename_column_captions follows).
    // Derived, not guessed. Measured from the movie: 'KeySetting' (the section) sits at x=960, its
    // 'ItemList' child at -854 inside it, and the LEFT column items ('Item_N_0') at +7 inside that, each
    // about 750 px wide (child extents -14 .. 704). Our page fills only the left column - the screen is
    // authored with TWO columns of 11 rows, 'Item_N_0' at x=7 and 'Item_N_1' at x=892 - so the visible
    // block spans roughly 113..863 px and its centre is ~488. Moving that centre to the stage centre
    // (960) is +472. Set to 0 to leave the screen exactly as authored.
    constexpr int32_t kRowSectionShiftPx = 472;
    constexpr uint16_t kRowSectionDepth = 344;
    constexpr uint16_t kRowSectionCid = 198;
    constexpr const char *kRowSectionName = "KeySetting";

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

    int rename_column_captions(std::vector<uint8_t> &buf)
    {
        static const char *const kFrom[] = {"StaticText_280005", "StaticText_280006"};
        static const char *const kTo[] = {"MfgHiddenText_005", "MfgHiddenText_006"};
        int patched = 0;
        for (size_t k = 0; k < 2; ++k)
        {
            const size_t n = std::strlen(kFrom[k]);
            if (std::strlen(kTo[k]) != n)
                continue; // a length change would shift the whole tag stream
            for (size_t i = 0; i + n <= buf.size(); ++i)
                if (std::memcmp(buf.data() + i, kFrom[k], n) == 0)
                {
                    std::memcpy(buf.data() + i, kTo[k], n);
                    ++patched;
                    i += n - 1;
                }
        }
        return patched;
    }

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

    // The world map's transform: add our own hover panel and change nothing else.
    bool rebuild_map(const uint8_t *src, size_t n, std::vector<uint8_t> &out)
    {
        if (n < 16 || std::memcmp(src, "GFX", 3) != 0)
            return false;
        const size_t hdr = header_size(src, n);
        if (!hdr)
            return false;
        std::vector<Tag> tags;
        g_tail_offset = 0;
        if (!parse_tags(src, n, hdr, tags) || tags.empty())
            return false;
        uint8_t tagged[96];
        size_t tagged_len = append_place_tag(tagged, kTipDepth, kTipName);
        tagged_len += append_place_tag(tagged + tagged_len, kBannerDepth, kBannerName);

        out.clear();
        out.reserve(n + 64);
        out.insert(out.end(), src, src + hdr);
        g_tip_placed = 0;
        for (const Tag &t : tags)
        {
            if (t.code == 39 && t.length >= 2 && !g_tip_placed)
            {
                uint16_t cid = 0;
                std::memcpy(&cid, src + t.offset, 2);
                if (cid == kMapBodyCid && emit_sprite_with(out, t, src, tagged, tagged_len))
                {
                    g_tip_placed = 1;
                    continue;
                }
            }
            emit_tag(out, t, src);
        }
        if (g_tail_offset && g_tail_offset < n)
            out.insert(out.end(), src + g_tail_offset, src + n);
        if (!g_tip_placed)
            return false; // nothing of ours went in - serve the game's bytes untouched
        uint32_t stored = 0;
        std::memcpy(&stored, src + 4, 4);
        const int64_t delta = static_cast<int64_t>(out.size()) - static_cast<int64_t>(n);
        const uint32_t adjusted = static_cast<uint32_t>(static_cast<int64_t>(stored) + delta);
        std::memcpy(out.data() + 4, &adjusted, 4);
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
        g_captions_renamed = rename_column_captions(out);
        shift_row_section(out); // menu centring, same-length in-place edit
        retune_help_text(out);  // the description block under the rows
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

    // POD-only (MSVC forbids objects with destructors in an SEH frame): pull the buffer and
    // size out of the File the engine just handed us.
    bool read_memory_file(void *f, const uint8_t **src, uint32_t *size)
    {
        __try
        {
            if (*reinterpret_cast<uintptr_t *>(f) != base() + kMemFileVtable)
                return false;
            uint8_t *o = reinterpret_cast<uint8_t *>(f);
            *src = *reinterpret_cast<const uint8_t **>(o + kMemFileBuffer);
            *size = *reinterpret_cast<uint32_t *>(o + kMemFileSize);
            return *src != nullptr && *size >= 16;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // POD-only: point the File at our bytes and rewind it.
    bool repoint_memory_file(void *f, const uint8_t *buf, uint32_t size)
    {
        __try
        {
            uint8_t *o = reinterpret_cast<uint8_t *>(f);
            *reinterpret_cast<const uint8_t **>(o + kMemFileBuffer) = buf;
            *reinterpret_cast<uint32_t *>(o + kMemFileSize) = size;
            *reinterpret_cast<uint32_t *>(o + kMemFilePos) = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::vector<uint8_t> g_served_map;

    void *open_map_file(void *self, const char *name, int flags, int mode)
    {
        void *f = o_open_file(self, name, flags, mode);
        if (!f)
            return f;
        const uint8_t *src = nullptr;
        uint32_t n = 0;
        if (!read_memory_file(f, &src, &n))
            return f;
        std::vector<uint8_t> built;
        if (!rebuild_map(src, n, built))
        {
            spdlog::info("[ownmovie] '{}': hover panel NOT added (Body sprite not found)", name);
            return f;
        }
        g_served_map.swap(built);
        if (!repoint_memory_file(f, g_served_map.data(),
                                 static_cast<uint32_t>(g_served_map.size())))
            return f;
        spdlog::info("[ownmovie] '{}': {} bytes in, {} out - own panels '{}' + '{}' placed in Body",
                     name, n, g_served_map.size(), kTipName, kBannerName);
        return f;
    }

    void *open_file_detour(void *self, const char *name, int flags, int mode)
    {
        if (map_name_matches(name))
            return open_map_file(self, name, flags, mode);
        // The menu movies are all loaded once at game start (the opener is asked for
        // 'data0:/menu/02_160_keyconfiguration.gfx' seconds into the run, lowercase), so there
        // is no per-open load to scope this to: the interception has to be live from the
        // start. That is fine for an ADDITIVE transform - extra characters plus a child clip
        // that stays invisible until we draw into it - which the real key-binding screen never
        // shows. arm()/disarm() therefore only annotate the log.
        if (!name_matches(name))
            return o_open_file(self, name, flags, mode);

        void *f = o_open_file(self, name, flags, mode);
        if (!f)
            return f;
        const uint8_t *src = nullptr;
        uint32_t n = 0;
        if (!read_memory_file(f, &src, &n))
        {
            g_status = "movie did not arrive as a readable memory file";
            return f;
        }
        std::vector<uint8_t> built;
        if (!rebuild(src, n, built))
            return f; // status already says why; the game's own bytes are served
        const bool identical = built.size() == n && std::memcmp(built.data(), src, n) == 0;
        // The previous buffer is only dropped here, once the next load starts: the File does
        // not copy, it points, and the parse happens inside this load.
        g_served.swap(built);
        if (!repoint_memory_file(f, g_served.data(), static_cast<uint32_t>(g_served.size())))
        {
            g_status = "could not re-point the movie file";
            return f;
        }
        g_status = g_icons_added ? "icons spliced into the movie"
                   : identical    ? "rebuilt, no icons added"
                                  : "rebuilt (size changed), no icons";
        spdlog::info("[ownmovie] '{}': {} bytes in, {} out, {} tags, {} trailing, {} captions "
                     "renamed, unchanged {}",
                     name, n, g_served.size(), g_tag_count,
                     g_tail_offset && g_tail_offset < n ? n - g_tail_offset : 0,
                     g_captions_renamed, identical ? "exact" : "DIFFERENT");
        spdlog::info("[ownmovie] caption block dropped from the panel: {}",
                     g_caption_block_dropped ? "yes" : "no");
        return f;
    }
}

void goblin::own_movie::install()
{
    try
    {
        modutils::hook<OpenFileFn>({.address = reinterpret_cast<void *>(base() + kOpenFile)},
                                   open_file_detour, o_open_file);
        g_ready.store(true, std::memory_order_release);
        spdlog::info("[ownmovie] movie load interception armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[ownmovie] load interception unavailable: {}", e.what());
    }
}

bool goblin::own_movie::available() { return g_ready.load(std::memory_order_acquire); }

void goblin::own_movie::arm() { g_armed.fetch_add(1, std::memory_order_acq_rel); }

void goblin::own_movie::disarm()
{
    int prev = g_armed.load(std::memory_order_acquire);
    while (prev > 0 && !g_armed.compare_exchange_weak(prev, prev - 1, std::memory_order_acq_rel))
        ;
}

const char *goblin::own_movie::status() { return g_status; }
