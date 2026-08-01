#include "goblin_sfimage.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <vector>

#include <windows.h>

namespace
{
    // ── engine entry points (all under tools/rva_anchors.py) ─────────────────────────
    // Render::RawImage::Create(format, mipLevels, const ImageSize&, use, heap, arena, sync)
    // Verified by decompile: allocates a 0x68-byte object, installs the RawImage vtable and
    // allocates one pixel buffer per mip level; plane records are 0x20 bytes and match the
    // SDK's Render::ImagePlane exactly (Width, Height, Pitch, DataSize, pData).
    constexpr uintptr_t kRawImageCreate = 0x11489B0;
    constexpr uintptr_t kImageResourceCtor = 0xD5FEE0; // (shell, name, image, flags)
    constexpr uintptr_t kDrawImageInto = 0xD81640;     // (sfv, ImageResource**, float rect[4], 0)
    // How to reach the GFx::Value behind a resolved clip proxy, and where its fields sit.
    //
    // The proxy is NOT a GFx::Value: the engine's own helpers all start with
    //   mov rax,[rcx]; call [rax+8]      (clip_set_visible at 0x733340)
    // i.e. they ask the proxy's vtable for the value and use the RETURNED pointer. Reading
    // the value out of the proxy at a fixed offset - which is what this file used to do - hands
    // the engine garbage, which is exactly why the interface pointer came back null.
    //
    // On the returned value P (confirmed in Value::SetVisible 0xD844D0, Value::SetText
    // 0xD842A0 and drawImageInto 0xD81640, which all agree):
    //   P+0x18  ObjectInterface*      P+0x20  type word (& 0x8F, 0x0A = display object)
    //   P+0x28  pdata (the AS object / character handle the interface expects)
    constexpr size_t kProxyGetValueSlot = 8; // vtable slot 1 returns the value
    constexpr size_t kValueIfaceOff = 0x18;
    constexpr size_t kValueTypeOff = 0x20;
    constexpr size_t kValueDataOff = 0x28;
    constexpr uint32_t kTypeDisplayObject = 0x0A;

    // GFx::Value::ObjectInterface is a plain virtual interface and the engine links BOTH
    // implementations (AS2 and AS3 - the exe contains "createEmptyMovieClip" and
    // "flash.display.Sprite" alike), so which one a movie uses is decided by the movie, not
    // by us. Calling a fixed address therefore risks running the AS3 body against an AS2
    // object (it silently returns false, which is exactly what we were seeing). We call the
    // VIRTUAL slot on the interface the value itself carries instead: correct for either VM,
    // and immune to a game update moving the code.
    //
    // Which vtable slot holds CreateEmptyMovieClip is NOT derived from the SDK header: this
    // build's ObjectInterface has extra virtuals, and the runtime interface pointer turned out
    // to sit at +0x2CBC828 rather than where the declaration order suggested, so counting slots
    // from GFx_Player.h put the call on SetText instead (it politely returned "success" while
    // creating nothing). Instead we look the slot UP: the AS3 implementation is the function
    // holding the "flash.display.Sprite" reference, whose address is anchored in
    // tools/rva_anchors.py, so scanning the live vtable for it gives the offset with no
    // counting at all - and keeps working if a patch inserts or removes methods.
    constexpr uintptr_t kCreateEmptyClipAS3 = 0x10DFDA0;
    constexpr size_t kVtScanSlots = 128;
    constexpr size_t kVtCreateEmptyClipHint = 0x138;

    size_t g_vt_create_offset = 0;

    size_t find_create_slot(uintptr_t b, uintptr_t vt)
    {
        if (g_vt_create_offset)
            return g_vt_create_offset;
        const uintptr_t want = b + kCreateEmptyClipAS3;
        __try
        {
            if (*reinterpret_cast<uintptr_t *>(vt + kVtCreateEmptyClipHint) == want)
            {
                g_vt_create_offset = kVtCreateEmptyClipHint;
                return g_vt_create_offset;
            }
            for (size_t k = 0; k < kVtScanSlots; ++k)
            {
                if (*reinterpret_cast<uintptr_t *>(vt + k * 8) == want)
                {
                    g_vt_create_offset = k * 8;
                    spdlog::info("[sfimage] CreateEmptyMovieClip found at vtable +0x{:X}",
                                 g_vt_create_offset);
                    return g_vt_create_offset;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return 0;
    }
    constexpr uintptr_t kAddRef = 0x113BB70;
    constexpr uintptr_t kHeapPtr = 0x4593250; // Scaleform MemoryHeap*, vt+0x50 = Alloc

    // Scaleform image formats (SDK Render_Image.h)
    constexpr uint32_t kImageR8G8B8A8 = 1;

    // Offsets inside a RawImage (from the Create decompile)
    constexpr size_t kRawImagePlanesPtr = 0x38; // -> array of 0x20-byte planes
    constexpr size_t kPlanePitch = 0x08;
    constexpr size_t kPlaneData = 0x18;


    uintptr_t base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

    // Ask a resolved clip proxy for the GFx::Value it wraps, the way the engine does.
    void *value_of(void *proxy)
    {
        __try
        {
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(proxy);
            if (!vt)
                return nullptr;
            using GetValueFn = void *(void *self);
            auto fn = *reinterpret_cast<GetValueFn **>(vt + kProxyGetValueSlot);
            return fn ? fn(proxy) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void *heap()
    {
        __try
        {
            return *reinterpret_cast<void **>(base() + kHeapPtr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // Everything we create is kept for the process lifetime: the resources are tiny, and a
    // Release would take the image with it while a clip may still reference it.
    std::vector<void *> g_resources;

    struct RawCreateArgs
    {
        uint32_t width;
        uint32_t height;
    };

    // POD-only worker: build the image and copy the pixels in.
    void *make_raw_image(uintptr_t b, int width, int height, const uint8_t *rgba)
    {
        using CreateFn = void *(uint32_t format, int32_t mips, const RawCreateArgs *size,
                                uint32_t use, void *heap, uint32_t arena, void *sync);
        void *img = nullptr;
        __try
        {
            RawCreateArgs size{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            img = reinterpret_cast<CreateFn *>(b + kRawImageCreate)(kImageR8G8B8A8, 1, &size, 0,
                                                                   nullptr, 0, nullptr);
            if (!img)
                return nullptr;
            uint8_t *plane = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(img) +
                                                          kRawImagePlanesPtr);
            if (!plane)
                return nullptr;
            const uint64_t pitch = *reinterpret_cast<uint64_t *>(plane + kPlanePitch);
            uint8_t *dst = *reinterpret_cast<uint8_t **>(plane + kPlaneData);
            if (!dst || pitch < static_cast<uint64_t>(width) * 4)
                return nullptr;
            for (int y = 0; y < height; ++y)
                std::memcpy(dst + static_cast<size_t>(y) * pitch,
                            rgba + static_cast<size_t>(y) * width * 4,
                            static_cast<size_t>(width) * 4);
            return img;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void *wrap_resource(uintptr_t b, const wchar_t *name, void *img)
    {
        __try
        {
            void *h = heap();
            if (!h)
                return nullptr;
            // MemoryHeap::Alloc is vtable slot +0x50; the resource object is 0x98 bytes.
            using AllocFn = void *(void *heap, size_t size, void *stat);
            const uintptr_t hvt = *reinterpret_cast<uintptr_t *>(h);
            void *shell = (*reinterpret_cast<AllocFn **>(hvt + 0x50))(h, 0x98, nullptr);
            if (!shell)
                return nullptr;
            using ResCtorFn = void *(void *shell, const wchar_t *name, void *image, uint32_t f);
            return reinterpret_cast<ResCtorFn *>(b + kImageResourceCtor)(shell, name, img, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // Why the last drawing attempt failed, so a refusal is diagnosable from one log line
    // instead of three indistinguishable `false`s.
    const char *g_draw_why = "not tried";
    uint32_t g_draw_type = 0;

    bool draw_raw(uintptr_t b, void *clip_proxy, void *resource, float x, float y, float w,
                  float h)
    {
        uint8_t *sfv = reinterpret_cast<uint8_t *>(value_of(clip_proxy));
        if (!sfv)
        {
            g_draw_why = "clip has no GFx value";
            return false;
        }
        __try
        {
            // The drawing path only accepts a display object.
            g_draw_type = *reinterpret_cast<uint32_t *>(sfv + kValueTypeOff);
            if ((g_draw_type & 0x8F) != kTypeDisplayObject)
            {
                g_draw_why = "value is not a display object";
                return false;
            }
            if (!*reinterpret_cast<void **>(sfv + kValueDataOff))
            {
                g_draw_why = "display object has no payload";
                return false;
            }
            // It Releases the resource on every path, so hand it a reference of its own.
            //
            // Pass the OBJECT, not the counter. The engine's addref begins `add rcx, 8` and only then
            // does the interlocked increment, i.e. it derives the counter itself (its sibling release at
            // 0x14113bb90 does the same with edx = -1). Passing `resource + 8` made the increment land on
            // resource+0x10 - the low half of a neighbouring pointer field - while the real refcount
            // stayed at 1, so the callee's own Release took it 1 -> 0 and destroyed a resource we were
            // still using. Audited 2026-07-28.
            using AddRefFn = int(void *);
            reinterpret_cast<AddRefFn *>(b + kAddRef)(resource);
            void *res = resource;
            // rect = {left, top, right, bottom} in pixels: the engine derives the scale as
            // (right-left)/imageWidth, (bottom-top)/imageHeight.
            float rect[4] = {x, y, x + w, y + h};
            using DrawFn = uint32_t(void *sfv, void **res, float *rect, void *unused);
            const bool ok =
                reinterpret_cast<DrawFn *>(b + kDrawImageInto)(sfv, &res, rect, nullptr) != 0;
            g_draw_why = ok ? "drawn" : "engine refused the drawing context";
            return ok;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_draw_why = "SEH while drawing";
            return false;
        }
    }

    // Reported once so a game update (or an overhaul shipping AS3 movies) is visible in the
    // log rather than silent: which interface implementation the menu movie actually uses.
    uintptr_t g_iface_vt_seen = 0;

    bool create_child(uintptr_t b, void *parent_proxy, const char *name, int32_t depth)
    {
        uint8_t *sfv = reinterpret_cast<uint8_t *>(value_of(parent_proxy));
        if (!sfv)
            return false;
        __try
        {
            void *iface = *reinterpret_cast<void **>(sfv + kValueIfaceOff);
            void *pdata = *reinterpret_cast<void **>(sfv + kValueDataOff);
            if (!iface || !pdata)
                return false;
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(iface);
            if (!vt)
                return false;
            g_iface_vt_seen = vt - b;
            // ObjectInterface::CreateEmptyMovieClip(pdata, out, instanceName, depth).
            // `out` receives a value object, and 0x20 bytes were NOT enough for it: the call
            // wrote past the buffer and tripped the stack-cookie check (0xC0000409 with
            // FAST_FAIL_STACK_COOKIE_CHECK_FAILURE, faulting in our own frame). The live
            // objects show why - a value carries a vptr plus interface, type and payload, so
            // it reaches at least 0x38 bytes. Give it room to spare; it is zeroed, so the
            // callee sees an undefined value and releases nothing that is not ours.
            uint8_t out[0x100] = {};
            using CreateClipFn = uint32_t(void *iface, void *pdata, void *out, const char *name,
                                          int32_t depth);
            const size_t slot = find_create_slot(b, vt);
            if (!slot)
                return false;
            auto fn = *reinterpret_cast<CreateClipFn **>(vt + slot);
            if (!fn)
                return false;
            // The out value DOES hold a reference we own (one AS3 Sprite per clip), and releasing it
            // here is NOT safe: doing so made the engine throw repeatedly inside its own name resolver
            // (0x14074A2F0+0x505) and value-release helper (0x140D7F86A, `mov [rcx],rax` on a bad rcx),
            // six times each, and then crash - reproduced by opening F8, closing it and pressing F6.
            // So something else still holds or re-reads this value after we return; the reference is
            // deliberately LEAKED instead. Bounded: ensure_child_clip only runs when the clip does not
            // already exist. Do not re-add a release without first finding that other owner.
            // Tried and reverted 2026-07-28.
            return fn(iface, pdata, out, name, depth) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

bool goblin::sfimage::available() { return heap() != nullptr; }

const char *goblin::sfimage::draw_failure() { return g_draw_why; }
uint32_t goblin::sfimage::last_value_type() { return g_draw_type; }

namespace
{
    goblin::sfimage::IconState g_icon_state = goblin::sfimage::IconState::Untried;
}

goblin::sfimage::IconState goblin::sfimage::icon_state() { return g_icon_state; }

void goblin::sfimage::set_icon_state(IconState state) { g_icon_state = state; }

const char *goblin::sfimage::icon_state_name()
{
    switch (g_icon_state)
    {
    case IconState::NoImage: return "no image";
    case IconState::NoClip: return "no clip";
    case IconState::NoDraw: return "refused";
    case IconState::Drawn: return "drawn";
    default: return "not tried";
    }
}

void *goblin::sfimage::create_resource(const wchar_t *name, int width, int height,
                                       const uint8_t *rgba)
{
    if (width <= 0 || height <= 0 || !rgba)
        return nullptr;
    const uintptr_t b = base();
    void *img = make_raw_image(b, width, height, rgba);
    if (!img)
    {
        spdlog::warn("[sfimage] could not build a {}x{} image", width, height);
        return nullptr;
    }
    void *res = wrap_resource(b, name ? name : L"MFG", img);
    if (!res)
    {
        spdlog::warn("[sfimage] could not wrap the image as a resource");
        return nullptr;
    }
    g_resources.push_back(res);
    return res;
}

bool goblin::sfimage::draw_into(void *clip_proxy, void *resource, float x, float y,
                                float width, float height)
{
    if (!clip_proxy || !resource)
        return false;
    return draw_raw(base(), clip_proxy, resource, x, y, width, height);
}

bool goblin::sfimage::ensure_child_clip(void *parent_proxy, const char *name, int32_t depth)
{
    if (!parent_proxy || !name)
        return false;
    const bool ok = create_child(base(), parent_proxy, name, depth);
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        spdlog::info("[sfimage] object interface vt at +0x{:X}, create slot +0x{:X}, "
                     "child '{}' {}",
                     g_iface_vt_seen, g_vt_create_offset, name, ok ? "made" : "refused");
    }
    return ok;
}
