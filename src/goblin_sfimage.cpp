#include "goblin_sfimage.hpp"

#include "goblin_anchors.hpp" // the named RVA of every engine helper called below

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
    constexpr goblin::AnchorId kRawImageCreate = goblin::AnchorId::rawimage_create;
    constexpr goblin::AnchorId kImageResourceCtor = goblin::AnchorId::image_resource_ctor; // (shell, name, image, flags)
    constexpr goblin::AnchorId kDrawImageInto = goblin::AnchorId::draw_image_into_clip;     // (sfv, ImageResource**, float rect[4], 0)
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

    // find_create_slot() resolved the vtable offset of CreateEmptyMovieClip by scanning the live
    // vtable for the AS3 implementation's address rather than counting slots from the SDK header
    // (counting put the call on SetText, which politely reported success and created nothing).
    // Its only caller was create_child, and it went with the rest of the own-icon path. The
    // technique is the reusable part: look the slot UP by a known function address.
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
    void *make_raw_image(int width, int height, const uint8_t *rgba)
    {
        using CreateFn = void *(uint32_t format, int32_t mips, const RawCreateArgs *size,
                                uint32_t use, void *heap, uint32_t arena, void *sync);
        void *img = nullptr;
        __try
        {
            RawCreateArgs size{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            img = reinterpret_cast<CreateFn *>(goblin::anchors::at(kRawImageCreate))(kImageR8G8B8A8, 1, &size, 0,
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

    void *wrap_resource(const wchar_t *name, void *img)
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
            return reinterpret_cast<ResCtorFn *>(goblin::anchors::at(kImageResourceCtor))(shell, name, img, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }


    // draw_raw() stood here: it drew a created resource into a clip's GFx value through the
    // engine's drawImageInto. No caller remains - the own-icon path it served was permanently
    // disabled (kEnableOwnIconPath = false) and the row icons are spliced into the movie instead.
    // Two things worth keeping from it if it is ever revived: the drawing target must be a
    // display object with a payload (type & 0x8F == 0x0A, non-null pdata), and the engine's addref
    // takes the OBJECT, not its counter - it does `add rcx, 8` itself, so passing resource+8 landed
    // the increment on a neighbouring field and let the callee's Release destroy a live resource.


    // create_child() stood here: it asked the movie's ObjectInterface for a new empty clip so a
    // row could be given somewhere to draw. Dead with the rest of the own-icon path. Two hazards
    // it documented, worth carrying forward if it is revived: the `out` value needs far more than
    // 0x20 bytes (0x20 tripped the stack cookie - a value is vptr + interface + type + payload,
    // at least 0x38), and the reference the out value holds must be LEAKED, not released - the
    // release made the engine fault inside its own name resolver and value-release helper.
}

bool goblin::sfimage::available() { return heap() != nullptr; }

// draw_failure(), last_value_type(), icon_state(), set_icon_state() and icon_state_name() were
// exported here. They reported how far the row-icon drawing got, for an overlay Tools page that
// no longer exists; nothing set the state and nothing read it.

void *goblin::sfimage::create_resource(const wchar_t *name, int width, int height,
                                       const uint8_t *rgba)
{
    if (width <= 0 || height <= 0 || !rgba)
        return nullptr;
    void *img = make_raw_image(width, height, rgba);
    if (!img)
    {
        spdlog::warn("[sfimage] could not build a {}x{} image", width, height);
        return nullptr;
    }
    void *res = wrap_resource(name ? name : L"MFG", img);
    if (!res)
    {
        spdlog::warn("[sfimage] could not wrap the image as a resource");
        return nullptr;
    }
    g_resources.push_back(res);
    return res;
}

// draw_into() and ensure_child_clip() were the public face of the drawing half removed above.
//
// NOTHING IN THIS FILE IS LIVE, and this note used to say the opposite. The claim was true when it
// was written (create_resource() served goblin_stall_probe's own-draw icon route) and stopped being
// true one round later, when that route and its icon_resource_for() were removed on 2026-07-30 -
// which took the module's last external caller with them. As of 2026-07-31 the file is out of
// CMakeLists.txt for that reason; see the note there. Everything below now calls only its
// neighbours, and g_resources is a write-only vector: it is push_back'ed and never walked, so the
// "hold a reference" it claims to implement is really just the absence of a Release.
