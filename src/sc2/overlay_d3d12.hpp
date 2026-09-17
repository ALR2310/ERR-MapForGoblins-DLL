#pragma once

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace cte::overlay::frame {
struct frame_packet;
struct font_atlas_packet;
}

namespace cte::overlay::d3d12 {

// Compile and validate the tiny renderer shaders on the bootstrap worker, not
// on the game's first visible Present.
bool prepare_shaders();

// NVIDIA Streamline's slGetNativeInterface: the object a Streamline proxy wraps, or nullptr when
// there is no sl.interposer.dll or `proxy` is not one of its proxies. What it returns is never
// Released (it may carry an extra reference; callers ask once per long-lived object).
void* streamline_native(void* proxy) noexcept;

// How work recorded on `queue_device` reaches `swap_device`, or nullptr when that is not proven.
// Frame-generation layers (ERSS-FG measured 2026-09-11) hand the game a wrapper device and queue
// while its swapchain answers GetDevice with the device underneath, so the two never compare equal
// although the game renders into that swapchain through exactly that queue every frame.
const char* device_reaches(ID3D12Device* queue_device, ID3D12Device* swap_device) noexcept;

// Why the most recent Session::render returned Skipped (a static string), or nullptr.
const char* last_render_skip() noexcept;

enum class ColorMode : uint8_t {
    Sdr,
    ScRgb,
    Hdr10,
    Unknown = 0xff,
};

enum class ColorEvidence : uint8_t {
    // No observation at all: an 8-bit backbuffer on a chain whose creation and SetColorSpace1 calls we
    // never saw. SDR is then an assumption from the format, not something DXGI told us - and logging it
    // as Dxgi made the log claim evidence it did not have.
    AssumedFromFormat,
    Dxgi,
    NvidiaNvapi,
    // The DISPLAY's own colour space, read back from IDXGIOutput6::GetDesc1 for the output the
    // swapchain presents to. Vendor-neutral, and the only evidence there is on a non-NVIDIA card
    // whose swapchain was created before our hooks went in (report 42's AMD + 10-bit rig).
    DxgiOutput,
    // Nothing proved it. A last-resort guess, so the overlay is visible-and-possibly-mis-encoded
    // instead of permanently absent. Always logged when it is used.
    AssumedFallback,
};

enum class RenderResult : uint8_t {
    Submitted,
    Skipped,
    RecoverableFailure,
    DeviceLost,
};

// One session belongs to one canonical swapchain/device/queue tuple.  It owns
// only device children; it never owns, resizes, presents, or changes the color
// state of the game's swapchain.
class Session final {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool initialize(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue,
                    ColorMode color_mode, ColorEvidence color_evidence);
    bool matches(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue,
                 ColorMode color_mode) const;

    RenderResult render(
        IDXGISwapChain3* swapchain,
        const frame::frame_packet* packet,
        const frame::font_atlas_packet* font,
        ColorMode color_mode);

    // Called before forwarding ResizeBuffers/ResizeBuffers1.  A true result
    // means every CustomTalismanEffects submission is complete and all backbuffer-derived
    // references were released.  A false result deliberately retains them;
    // freeing GPU-live D3D12 objects would be worse than letting resize fail.
    bool before_resize();
    bool gpu_idle() const;

    HRESULT device_removed_reason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cte::overlay::d3d12
