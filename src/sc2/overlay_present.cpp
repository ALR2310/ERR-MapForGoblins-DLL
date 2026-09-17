// CustomTalismanEffects backend v2 -- DXGI hook coordination and swapchain selection.
//
// The detours in this file are deliberately small wrappers.  They validate a
// D3D12 HWND swapchain, acquire the exact creation queue, submit one private
// command list, release every
// lock, and then call the captured next link exactly once.  They never call a
// swapchain method recursively and never rediscover a "clean" DXGI original.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <imgui.h>
#include <intrin.h>
#include <wrl/client.h>

#pragma intrinsic(_ReturnAddress)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "hooks.hpp"
#include "log.hpp"
#include "overlay_coexist.hpp"
#include "overlay_d3d12.hpp"
#include "overlay_dxgi_shadow.hpp"
#include "overlay_frame.hpp"
#include "overlay_present.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace cte::overlay::present {
namespace {

using Microsoft::WRL::ComPtr;
using clock_type = std::chrono::steady_clock;

// ── why the silent handlers now speak ────────────────────────────────────────────────
// Every `catch (...)` in this file used to swallow its exception without a word, and eight of them
// also clear g_renderer_healthy. That flag is otherwise STICKY: it is set true only when a primary
// swapchain is selected or a replacement adopted, and the one eligibility revocation lives at
// install time and never flips back. So when a player log (Linux/Proton, adoption path) showed
// healthy flapping 124 times against two selections, those handlers were the only thing that could
// be doing it - and they said nothing at all about why.
// Called from inside a catch block: `throw;` rethrows the in-flight exception so its type can be
// read. Rate-limited per site so a per-frame fault cannot flood the log.
void note_swallowed_exception(int line)
{
    const char *what = "non-std exception";
    try
    {
        throw;
    }
    catch (const std::exception &e)
    {
        what = e.what();
    }
    catch (...)
    {
        // Deliberately silent: this IS the reporter.
    }
    static std::atomic<int> counts[64]{};
    const int slot = line & 63;
    const int n = counts[slot].fetch_add(1, std::memory_order_relaxed);
    if (n < 3 || (n % 200) == 0)
        flog("[overlay-v2] swallowed exception at line %d (occurrence %d): %s", line, n + 1, what);
}

constexpr uintptr_t kFontTextureTokenValue =
    static_cast<uintptr_t>(0x5150464F4E54ull); // "QPFONT"
const ImTextureID kFontTextureToken =
    reinterpret_cast<ImTextureID>(kFontTextureTokenValue);

std::atomic<bool> g_installed{false};
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_renderer_ready{false};
std::atomic<bool> g_visible{false};
std::atomic<bool> g_renderer_healthy{true};
std::atomic<bool> g_retry_on_next_open{false};
std::atomic<uint64_t> g_font_generation{0};
bool g_packet_skip_warning_logged = false; // serialized frontend producer only

// Atomic fields plus a generation seqlock avoid taking a lock in either the
// Present callback or the control thread.  Every payload field is itself
// atomic, so this is data-race-free under the C++ memory model.
struct CanvasStore {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uintptr_t> hwnd{0};
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
    std::atomic<bool> hdr{false};
    std::atomic<bool> ready{false};
} g_canvas;
SRWLOCK g_canvas_writer_lock = SRWLOCK_INIT;

void publish_canvas(HWND hwnd, uint32_t width, uint32_t height, bool hdr,
                    bool ready) noexcept {
    // The sequence counter is a seqlock and therefore requires serialized
    // writers. Present, resize, color-space, device-loss, and shutdown paths
    // can run on different threads, so protect only the tiny publication.
    AcquireSRWLockExclusive(&g_canvas_writer_lock);
    g_canvas.sequence.fetch_add(1, std::memory_order_acq_rel); // odd
    g_canvas.hwnd.store(reinterpret_cast<uintptr_t>(hwnd), std::memory_order_relaxed);
    g_canvas.width.store(width, std::memory_order_relaxed);
    g_canvas.height.store(height, std::memory_order_relaxed);
    g_canvas.hdr.store(hdr, std::memory_order_relaxed);
    g_canvas.ready.store(ready, std::memory_order_relaxed);
    g_canvas.sequence.fetch_add(1, std::memory_order_release); // even
    ReleaseSRWLockExclusive(&g_canvas_writer_lock);
}

void* com_identity(IUnknown* object) noexcept {
    if (!object) return nullptr;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
        return nullptr;
    void* result = identity;
    identity->Release();
    return result;
}

// Behind ERSS-FG (4.15, measured 2026-09-11) the game presents a Streamline proxy swapchain
// (Present resolves into sl.interposer.dll) and submits on ERSS-FG's own wrapper queue
// (ExecuteCommandLists resolves into ERSS-FG.dll). The proxy swapchain, and the native one behind
// it, both answer GetDevice with one device; the only direct queue the game submits on answers with
// ANOTHER - the wrapper device the game created everything on. So a plain identity compare never
// holds and adoption sat on "waiting for sustained single-queue submit evidence" all session.
// d3d12::device_reaches proves the wrapper relation instead, and the overlay then renders exactly
// the way the game does: the presenting swapchain, the game's queue, the game's device.
// The Streamline unwrapping below stays as a second route for layers whose queue it does know.
IDXGISwapChain* streamline_native_swapchain(IDXGISwapChain* swapchain) noexcept {
    return static_cast<IDXGISwapChain*>(d3d12::streamline_native(swapchain));
}

ID3D12CommandQueue* streamline_native_queue(ID3D12CommandQueue* queue) noexcept {
    return static_cast<ID3D12CommandQueue*>(d3d12::streamline_native(queue));
}

uint64_t now_ticks() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_type::now().time_since_epoch()).count());
}

// -------------------------------------------------------------------------
// Queue discovery and creation-time swapchain binding

struct QueueBinding {
    QueueBinding() = default;
    QueueBinding(QueueBinding&&) noexcept = default;
    QueueBinding& operator=(QueueBinding&&) noexcept = default;
    QueueBinding(const QueueBinding&) = delete;
    QueueBinding& operator=(const QueueBinding&) = delete;

    uint64_t swapchain_generation = 0;
    ComPtr<ID3D12CommandQueue> queue;
    void* queue_identity = nullptr;
    void* device_identity = nullptr;
    uint64_t transaction_id = 0;
    uint32_t evidence_depth = 0;
    uint64_t last_seen_ms = 0;
    bool proxy_ambiguous = false;
    bool ambiguity_logged = false;
};

std::mutex g_registry_mutex;
std::vector<QueueBinding> g_queue_bindings;
std::atomic<uint64_t> g_next_binding_transaction{1};

constexpr size_t kMaximumQueueBindings = 32;
using RetiredQueues =
    std::array<ComPtr<ID3D12CommandQueue>, kMaximumQueueBindings + 1>;

void retire_queue(QueueBinding& binding, RetiredQueues& retired,
                  size_t& retired_count) noexcept {
    // The registry is capped, so this bound is an invariant rather than a
    // best-effort limit.  Moving first ensures vector erase/assignment cannot
    // call a proxy queue's Release while the registry mutex is held.
    if (binding.queue && retired_count < retired.size())
        retired[retired_count++] = std::move(binding.queue);
}

void prune_dead_bindings_locked(RetiredQueues& retired,
                                size_t& retired_count) noexcept {
    for (size_t index = 0; index < g_queue_bindings.size();) {
        QueueBinding& binding = g_queue_bindings[index];
        if (dxgi_shadow::generation_alive(binding.swapchain_generation)) {
            ++index;
            continue;
        }
        retire_queue(binding, retired, retired_count);
        g_queue_bindings.erase(g_queue_bindings.begin() +
                               static_cast<ptrdiff_t>(index));
    }
}

uint64_t next_binding_transaction() noexcept {
    uint64_t value =
        g_next_binding_transaction.fetch_add(1, std::memory_order_relaxed);
    if (value == 0)
        value = g_next_binding_transaction.fetch_add(1,
                                                      std::memory_order_relaxed);
    return value;
}

struct ThreadBindingTransaction {
    uint64_t id = 0;
    uint32_t depth = 0;
};

thread_local ThreadBindingTransaction g_creation_transaction;
thread_local ThreadBindingTransaction g_resize_transaction;

class BindingScope final {
public:
    explicit BindingScope(ThreadBindingTransaction& transaction) noexcept
        : transaction_(transaction) {
        if (transaction_.depth == 0)
            transaction_.id = next_binding_transaction();
        ++transaction_.depth;
        id_ = transaction_.id;
        depth_ = transaction_.depth;
    }
    ~BindingScope() {
        if (transaction_.depth != 0) --transaction_.depth;
        if (transaction_.depth == 0) transaction_.id = 0;
    }
    uint64_t id() const noexcept { return id_; }
    uint32_t depth() const noexcept { return depth_; }

private:
    ThreadBindingTransaction& transaction_;
    uint64_t id_ = 0;
    uint32_t depth_ = 0;
};

// `game_presented`: the swapchain is one whose Present the game's own code was seen calling
// (adoption via observe_present) - the only case where a queue on a wrapper device is accepted.
bool bind_swapchain_queue(IDXGISwapChain* swapchain,
                          IUnknown* device_or_queue,
                          uint64_t transaction_id,
                          uint32_t evidence_depth,
                          bool game_presented = false) noexcept {
    RetiredQueues retired{};
    size_t retired_count = 0;
    try {
        if (g_stopping.load(std::memory_order_acquire) || !swapchain ||
            !device_or_queue)
            return false;

        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device_or_queue->QueryInterface(IID_PPV_ARGS(&queue))) || !queue)
            return false; // D3D11 swapchain or an unrelated creation call
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;

        ComPtr<ID3D12Device> queue_device;
        ComPtr<ID3D12Device> swap_device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || !queue_device ||
            FAILED(swapchain->GetDevice(IID_PPV_ARGS(&swap_device))) || !swap_device)
            return false;

        // Keyed by the SWAPCHAIN's device: that is what the Present path looks the binding up by,
        // and behind a wrapper layer the queue's own device is a different object (device_reaches).
        void* const queue_device_id = com_identity(swap_device.Get());
        void* const queue_id = com_identity(queue.Get());
        const char* const reach = d3d12::device_reaches(queue_device.Get(), swap_device.Get());
        if (!queue_id || !queue_device_id || !reach) {
            flog("[overlay-v2] rejected swapchain creation queue: device identity mismatch");
            return false;
        }
        if (std::strcmp(reach, "same device") != 0 && !game_presented) {
            // A creation-time queue on a wrapper device says nothing about which layer's
            // swapchain the game presents (see observe_present). Pass-through only.
            flog("[overlay-v2] swapchain queue sits on a wrapper device (%s); pass-through only",
                 reach);
            return false;
        }

        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation || !dxgi_shadow::renderer_eligible(swapchain))
            return false;

        const uint64_t now = now_ticks();
        QueueBinding pending;
        pending.swapchain_generation = generation;
        pending.queue = std::move(queue);
        pending.queue_identity = queue_id;
        pending.device_identity = queue_device_id;
        pending.transaction_id = transaction_id;
        pending.evidence_depth = evidence_depth;
        pending.last_seen_ms = now;

        std::lock_guard lock(g_registry_mutex);
        if (g_stopping.load(std::memory_order_acquire)) return false;
        prune_dead_bindings_locked(retired, retired_count);
        bool proxy_ambiguous = false;
        for (const auto& binding : g_queue_bindings) {
            if (binding.transaction_id == transaction_id &&
                binding.evidence_depth > evidence_depth &&
                binding.swapchain_generation != generation &&
                binding.queue_identity != queue_id) {
                // A nested factory/ResizeBuffers1 call used a different queue.
                // The outer object is a proxy and DXGI provides no proof that
                // its caller-supplied queue sequences the presented buffers.
                proxy_ambiguous = true;
                break;
            }
        }
        for (auto& binding : g_queue_bindings) {
            if (binding.swapchain_generation == generation) {
                if (binding.transaction_id == transaction_id &&
                    binding.evidence_depth > evidence_depth) {
                    // The same installed object was observed at two wrapper
                    // layers. Preserve the innermost evidence.
                    return true;
                }
                if (binding.queue_identity != queue_id) {
                    retire_queue(binding, retired, retired_count);
                    binding.queue = std::move(pending.queue);
                }
                binding.queue_identity = queue_id;
                binding.device_identity = queue_device_id;
                binding.transaction_id = transaction_id;
                binding.evidence_depth = evidence_depth;
                binding.last_seen_ms = now;
                binding.proxy_ambiguous =
                    binding.proxy_ambiguous || proxy_ambiguous;
                return true;
            }
        }
        if (g_queue_bindings.size() >= kMaximumQueueBindings) {
            const auto oldest = std::min_element(
                g_queue_bindings.begin(), g_queue_bindings.end(),
                [](const QueueBinding& a, const QueueBinding& b) {
                    return a.last_seen_ms < b.last_seen_ms;
                });
            if (oldest != g_queue_bindings.end()) {
                retire_queue(*oldest, retired, retired_count);
                g_queue_bindings.erase(oldest);
            }
        }
        pending.proxy_ambiguous = proxy_ambiguous;
        g_queue_bindings.push_back(std::move(pending));
        if (proxy_ambiguous) {
            flog("[overlay-v2] nested proxy used a different queue; outer swapchain is pass-through only");
        } else {
            flog("[overlay-v2] captured D3D12 queue from the innermost observed swapchain transaction");
        }
        return true;
    } catch (...) {
        // Registry allocation failure disables this association only.  The
        // swapchain creation result and every later Present remain untouched.
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
        return false;
    }
}

void erase_swapchain_binding(uint64_t generation) noexcept {
    if (!generation) return;
    RetiredQueues retired{};
    size_t retired_count = 0;
    try {
        std::lock_guard lock(g_registry_mutex);
        for (size_t index = 0; index < g_queue_bindings.size();) {
            QueueBinding& binding = g_queue_bindings[index];
            if (binding.swapchain_generation != generation) {
                ++index;
                continue;
            }
            retire_queue(binding, retired, retired_count);
            g_queue_bindings.erase(g_queue_bindings.begin() +
                                   static_cast<ptrdiff_t>(index));
        }
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

ComPtr<ID3D12CommandQueue> queue_for_swapchain(
    uint64_t generation, void* device_identity) noexcept {
    try {
        std::unique_lock lock(g_registry_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return {};

        for (auto& binding : g_queue_bindings) {
            if (binding.swapchain_generation == generation &&
                binding.device_identity == device_identity) {
                if (binding.proxy_ambiguous) {
                    if (!binding.ambiguity_logged) {
                        binding.ambiguity_logged = true;
                        flog("[overlay-v2] proxy presentation queue is unverified; rendering disabled for this swapchain");
                    }
                    return {};
                }
                binding.last_seen_ms = now_ticks();
                return binding.queue;
            }
        }
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
    // Even a single DIRECT queue on the same device does not prove that it is
    // the queue DXGI associated with this swapchain.  Never guess.
    return {};
}

void bind_resize_queues(IDXGISwapChain3* swapchain, UINT requested_count,
                        const UINT* node_masks,
                        IUnknown* const* present_queues,
                        uint64_t transaction_id,
                        uint32_t evidence_depth) noexcept {
    try {
        if (!swapchain) return;
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation) return;

        // Per the API contract both arrays have exactly BufferCount entries. Zero means "preserve the
        // existing count" - which preserves the COUNT, not the queues: the caller still supplies arrays
        // at the existing length, and per the docs "the swapchain will also rotate through these command
        // queues" on Present. Keeping our old binding would therefore leave us submitting on a queue the
        // swapchain no longer rotates through. Since the real length is whatever the swapchain currently
        // has and we cannot read the caller's arrays safely without it, drop the exact binding and let
        // the code that needs it fail closed rather than trust a stale one.
        if (requested_count == 0) {
            erase_swapchain_binding(generation);
            return;
        }

        ComPtr<IDXGISwapChain1> swapchain1;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) ||
            !swapchain1 || FAILED(swapchain1->GetDesc1(&desc)) ||
            desc.BufferCount != requested_count || !present_queues) {
            erase_swapchain_binding(generation);
            return;
        }

        // ResizeBuffers1 permits a queue and node mask per backbuffer.  This
        // single-node renderer supports the common case only: every buffer is
        // presented on the exact same direct queue.  Heterogeneous arrays are
        // deliberately rejected instead of silently binding element zero.
        const UINT effective_count = requested_count;
        ComPtr<ID3D12CommandQueue> first_queue;
        void* first_identity = nullptr;
        UINT first_mask = 0;
        for (UINT i = 0; i < effective_count; ++i) {
            ComPtr<ID3D12CommandQueue> queue;
            if (!present_queues[i] ||
                FAILED(present_queues[i]->QueryInterface(IID_PPV_ARGS(&queue))) ||
                !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                erase_swapchain_binding(generation);
                return;
            }
            const void* identity = com_identity(queue.Get());
            const UINT mask = node_masks ? node_masks[i] : 0;
            if (i == 0) {
                first_queue = queue;
                first_identity = const_cast<void*>(identity);
                first_mask = mask;
            } else if (identity != first_identity || mask != first_mask) {
                flog("[overlay-v2] heterogeneous ResizeBuffers1 queues/nodes; pass-through only");
                erase_swapchain_binding(generation);
                return;
            }
        }
        if (!first_queue || (first_mask != 0 && first_mask != 1)) {
            erase_swapchain_binding(generation);
            return;
        }
        bind_swapchain_queue(swapchain, first_queue.Get(), transaction_id,
                             evidence_depth);
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

// -------------------------------------------------------------------------
// Swapchain selection, color observation, and renderer session

struct ObservedSwapchain {
    uint64_t generation = 0;
    HWND hwnd = nullptr;
    void* device_identity = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t consecutive_presents = 0;
    uint64_t last_seen_ms = 0;
};

std::atomic<uint64_t> g_next_color_event_sequence{1};
std::atomic<bool> g_nvapi_observer_enabled{false};
std::atomic<uint64_t> g_nvapi_state_revision{0};

enum class NvapiHdrCommand : int32_t {
    Get = 0,
    Set = 1,
};

enum class NvapiHdrMode : int32_t {
    Off = 0,
    Uhda = 2,
    UhdaPassthrough = 5,
};

// All NVAPI HDR color-data versions begin with these three public fields.
// Elden Ring currently passes V1 (version 0x00010028).
struct NvapiHdrColorDataHeader {
    uint32_t version;
    int32_t command;
    int32_t hdr_mode;
};

struct NvapiDisplayBinding {
    uint32_t display_id = 0;
    std::array<char, CCHDEVICENAME> output_name{};
};

struct NvapiColorObservation {
    uint32_t display_id = 0;
    std::array<char, CCHDEVICENAME> output_name{};
    int32_t hdr_mode = -1;
    uint64_t event_sequence = 0;
};

std::mutex g_nvapi_mutex;
std::vector<NvapiDisplayBinding> g_nvapi_display_bindings;
std::vector<NvapiColorObservation> g_nvapi_color_observations;

uint64_t next_color_event_sequence() noexcept {
    uint64_t value =
        g_next_color_event_sequence.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
        value = g_next_color_event_sequence.fetch_add(
            1, std::memory_order_relaxed);
    }
    return value;
}

struct ColorObservation {
    uint64_t generation = 0;
    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    uint64_t event_sequence = 0;
    bool known = false;
};

std::mutex g_render_mutex;
thread_local bool g_present_render_critical = false;
struct PresentRenderCriticalScope {
    PresentRenderCriticalScope() noexcept {
        g_present_render_critical = true;
    }
    ~PresentRenderCriticalScope() {
        g_present_render_critical = false;
    }
};
std::vector<ObservedSwapchain> g_observed_swapchains;
std::vector<ColorObservation> g_color_observations;
uint64_t g_primary_generation = 0;
HWND g_primary_hwnd = nullptr;
uint64_t g_color_unknown_logged_generation = 0;
uint64_t g_color_guess_logged_generation = 0;
// Diagnostics for the otherwise-silent shadow-to-primary path.  All are
// written only under g_render_mutex.
uint64_t g_present_reached_logged_generation = 0;
uint64_t g_describe_reject_logged_generation = 0;
const char* g_describe_reject_reason = nullptr;
const char* g_primary_block_reason = nullptr;
uint64_t g_queue_missing_logged_generation = 0;
std::unique_ptr<d3d12::Session> g_session;
struct PendingCompositedBuffer {
    uint64_t generation = 0;
    uint32_t slot = 0;
};
PendingCompositedBuffer g_pending_composited_buffer;
constexpr size_t kMaximumRetiredSessionsPerPresent = 3;
using RetiredSessions = std::array<std::unique_ptr<d3d12::Session>,
                                   kMaximumRetiredSessionsPerPresent>;

void retire_current_session(RetiredSessions& retired,
                            size_t& retired_count) noexcept {
    if (!g_session) return;
    if (retired_count < retired.size()) {
        retired[retired_count++] = std::move(g_session);
    } else {
        // This bound is not reachable in the current state machine. If a
        // future edit violates it, quarantine instead of running a potentially
        // re-entrant COM destruction while the renderer mutex is held.
        (void)g_session.release();
    }
}

bool g_session_rebuild_pending = false;
struct LifecycleGate {
    uint64_t generation = 0;
    uint32_t depth = 0;
};
LifecycleGate g_resize_gate;
LifecycleGate g_color_gate;

ColorObservation* color_record(uint64_t generation) {
    for (auto& item : g_color_observations)
        if (item.generation == generation) return &item;
    if (g_color_observations.size() >= 32) {
        const auto discard = std::find_if(
            g_color_observations.begin(), g_color_observations.end(),
            [](const ColorObservation& item) {
                return item.generation != g_primary_generation;
            });
        if (discard != g_color_observations.end())
            g_color_observations.erase(discard);
    }
    g_color_observations.push_back({generation});
    return &g_color_observations.back();
}

std::array<char, CCHDEVICENAME> copy_output_name(
    const char* source) noexcept {
    std::array<char, CCHDEVICENAME> result{};
    if (!source) return result;
    for (size_t index = 0; index + 1 < result.size() && source[index] != '\0';
         ++index)
        result[index] = source[index];
    return result;
}

const char* nvapi_hdr_mode_name(int32_t mode) noexcept {
    switch (static_cast<NvapiHdrMode>(mode)) {
    case NvapiHdrMode::Off: return "OFF";
    case NvapiHdrMode::Uhda: return "UHDA";
    case NvapiHdrMode::UhdaPassthrough: return "UHDA_PASSTHROUGH";
    default: return "unknown";
    }
}

void record_nvapi_display_binding(
    const std::array<char, CCHDEVICENAME>& output_name,
    uint32_t display_id) {
    {
        std::lock_guard lock(g_nvapi_mutex);
        bool binding_changed = false;
        const auto existing = std::find_if(
            g_nvapi_display_bindings.begin(), g_nvapi_display_bindings.end(),
            [display_id](const NvapiDisplayBinding& item) {
                return item.display_id == display_id;
            });
        if (existing != g_nvapi_display_bindings.end()) {
            binding_changed = existing->output_name[0] != '\0' &&
                              existing->output_name != output_name;
            existing->output_name = output_name;
        } else {
            if (g_nvapi_display_bindings.size() >= 32)
                g_nvapi_display_bindings.erase(
                    g_nvapi_display_bindings.begin());
            g_nvapi_display_bindings.push_back({display_id, output_name});
        }
        const auto observation = std::find_if(
            g_nvapi_color_observations.begin(),
            g_nvapi_color_observations.end(),
            [display_id](const NvapiColorObservation& item) {
                return item.display_id == display_id;
            });
        if (observation != g_nvapi_color_observations.end()) {
            if (binding_changed) {
                // Display IDs may be recycled after a topology change. Never
                // transfer an old HDR SET observation to a different output.
                g_nvapi_color_observations.erase(observation);
            } else if (observation->output_name[0] == '\0') {
                observation->output_name = output_name;
            }
        }
    }
    g_nvapi_state_revision.fetch_add(1, std::memory_order_release);
}

void record_nvapi_color_observation(uint32_t display_id, int32_t hdr_mode,
                                    const char* observed_via) {
    NvapiColorObservation observed;
    observed.display_id = display_id;
    observed.hdr_mode = hdr_mode;
    observed.event_sequence = next_color_event_sequence();

    {
        std::lock_guard lock(g_nvapi_mutex);
        const auto binding = std::find_if(
            g_nvapi_display_bindings.begin(), g_nvapi_display_bindings.end(),
            [display_id](const NvapiDisplayBinding& item) {
                return item.display_id == display_id;
            });
        if (binding != g_nvapi_display_bindings.end())
            observed.output_name = binding->output_name;

        const auto existing = std::find_if(
            g_nvapi_color_observations.begin(),
            g_nvapi_color_observations.end(),
            [display_id](const NvapiColorObservation& item) {
                return item.display_id == display_id;
            });
        if (existing != g_nvapi_color_observations.end()) {
            *existing = observed;
        } else {
            if (g_nvapi_color_observations.size() >= 32) {
                const auto oldest = std::min_element(
                    g_nvapi_color_observations.begin(),
                    g_nvapi_color_observations.end(),
                    [](const NvapiColorObservation& left,
                       const NvapiColorObservation& right) {
                        return left.event_sequence < right.event_sequence;
                    });
                if (oldest != g_nvapi_color_observations.end())
                    g_nvapi_color_observations.erase(oldest);
            }
            g_nvapi_color_observations.push_back(observed);
        }
    }
    g_nvapi_state_revision.fetch_add(1, std::memory_order_release);

    flog("[overlay-v2] observed NvAPI HDR state via %s "
         "(mode=%s, display_id=%u, output=%s)",
         observed_via, nvapi_hdr_mode_name(hdr_mode), display_id,
         observed.output_name[0] ? observed.output_name.data() : "unmapped");
}

std::array<char, CCHDEVICENAME> monitor_output_name(
    HMONITOR monitor) noexcept {
    std::array<char, CCHDEVICENAME> result{};
    if (!monitor) return result;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return result;
    if (WideCharToMultiByte(CP_ACP, 0, info.szDevice, -1, result.data(),
                            static_cast<int>(result.size()), nullptr,
                            nullptr) == 0)
        result = {};
    return result;
}

struct NvapiColorSnapshot {
    int32_t hdr_mode = -1;
    uint64_t event_sequence = 0;
    bool valid = false;
};

struct NvapiColorCache {
    uint64_t swapchain_generation = 0;
    uint64_t state_revision = 0;
    HMONITOR monitor = nullptr;
    NvapiColorSnapshot color;
} g_nvapi_color_cache;

NvapiColorSnapshot latest_nvapi_color_for_window(uint64_t generation,
                                                  HWND hwnd) {
    // Called with g_render_mutex held. A cheap monitor-handle comparison keeps
    // borderless/exclusive moves correct; name conversion and the vendor mutex
    // are needed only after an NVAPI state, monitor, or generation change.
    const uint64_t revision =
        g_nvapi_state_revision.load(std::memory_order_acquire);
    const HMONITOR monitor = revision != 0
        ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) : nullptr;
    if (g_nvapi_color_cache.swapchain_generation == generation &&
        g_nvapi_color_cache.state_revision == revision &&
        g_nvapi_color_cache.monitor == monitor)
        return g_nvapi_color_cache.color;

    NvapiColorSnapshot result;
    if (revision != 0) {
        const auto output_name = monitor_output_name(monitor);
        std::lock_guard lock(g_nvapi_mutex);
        for (const auto& item : g_nvapi_color_observations) {
            const bool matches_output =
                output_name[0] != '\0' && item.output_name[0] != '\0' &&
                _stricmp(output_name.data(), item.output_name.data()) == 0;
            if (!matches_output ||
                item.event_sequence <= result.event_sequence)
                continue;
            result.hdr_mode = item.hdr_mode;
            result.event_sequence = item.event_sequence;
            result.valid = true;
        }
    }
    g_nvapi_color_cache = {generation, revision, monitor, result};
    return result;
}

bool creation_default_is_authoritative(uint64_t generation,
                                       uint64_t transaction_id,
                                       uint32_t evidence_depth) noexcept {
    try {
        std::lock_guard lock(g_registry_mutex);
        for (const auto& binding : g_queue_bindings) {
            if (binding.transaction_id == transaction_id &&
                binding.evidence_depth > evidence_depth &&
                binding.swapchain_generation != generation) {
                // A factory wrapper created a distinct inner swapchain before
                // returning this object. It could also have changed the outer
                // object's color space before our instance shadow existed, so
                // its post-creation state is not the DXGI default we can prove
                // for a directly observed creation.
                return false;
            }
        }
        return true;
    } catch (...) {
        cte::note_swallowed(__FILE__, __LINE__);
        return false;
    }
}

void record_creation_color_default(IDXGISwapChain* swapchain,
                                   uint64_t transaction_id,
                                   uint32_t evidence_depth) noexcept {
    try {
        if (!swapchain) return;
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation ||
            !creation_default_is_authoritative(generation, transaction_id,
                                               evidence_depth))
            return;

        ComPtr<IDXGISwapChain1> swapchain1;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) ||
            !swapchain1 || FAILED(swapchain1->GetDesc1(&desc)))
            return;

        // DXGI defines the creation-time encoding by pixel format: floating
        // point swapchains start as linear scRGB, while normalized integer
        // formats (including R10) start as sRGB/SDR. A later successful
        // SetColorSpace1 callback replaces this baseline before the next
        // Present. This fact is essential for games that keep the default and
        // therefore never call the setter at all.
        const DXGI_COLOR_SPACE_TYPE default_space =
            desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        std::lock_guard lock(g_render_mutex);
        ColorObservation* record = color_record(generation);
        if (!record->known) {
            record->color_space = default_space;
            record->known = true;
        }
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

struct InferredColor {
    d3d12::ColorMode mode = d3d12::ColorMode::Unknown;
    d3d12::ColorEvidence evidence = d3d12::ColorEvidence::Dxgi;
};

// The colour space of the OUTPUT the swapchain presents to, straight from DXGI - no vendor SDK.
//
// This is the evidence that was missing on report 42's rig: AMD (so the nvapi observer never loads)
// plus a swapchain created before our hooks (so there is no creation observation), which left a
// 10-bit buffer with nothing to prove its encoding and the canvas permanently unavailable. The
// display itself knows, and IDXGIOutput6::GetDesc1 reports it on any vendor.
//
// Two ways in, because GetContainingOutput refuses on a composition swapchain - which is exactly
// what a frame-generation or overlay layer hands us: ask the swapchain first, then fall back to the
// monitor under the window. Cached per (window, monitor): this runs inside Present, and the answer
// only changes when the user toggles HDR, which also changes the monitor handle or forces a resize.
bool display_color_space(IDXGISwapChain3* swapchain, HWND hwnd,
                         DXGI_COLOR_SPACE_TYPE* out) noexcept {
    if (!out) return false;

    static std::mutex s_cache_lock;
    static HMONITOR s_cached_monitor = nullptr;
    static DXGI_COLOR_SPACE_TYPE s_cached_space = DXGI_COLOR_SPACE_CUSTOM;
    static bool s_cached_valid = false;

    const HMONITOR monitor =
        hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    {
        std::lock_guard<std::mutex> guard(s_cache_lock);
        if (s_cached_valid && monitor && monitor == s_cached_monitor) {
            *out = s_cached_space;
            return true;
        }
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (swapchain) (void)swapchain->GetContainingOutput(&output);

    if (!output && monitor) {
        // Composition swapchains have no containing output; find the monitor ourselves.
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
                 ++a) {
                Microsoft::WRL::ComPtr<IDXGIOutput> candidate;
                for (UINT o = 0; adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o) {
                    DXGI_OUTPUT_DESC desc{};
                    if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == monitor) {
                        output = candidate;
                        break;
                    }
                    candidate.Reset();
                }
                adapter.Reset();
            }
        }
    }
    if (!output) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6)) || !output6) return false; // pre-Windows-10-1703
    DXGI_OUTPUT_DESC1 desc1{};
    if (FAILED(output6->GetDesc1(&desc1))) return false;

    {
        std::lock_guard<std::mutex> guard(s_cache_lock);
        s_cached_monitor = monitor;
        s_cached_space = desc1.ColorSpace;
        s_cached_valid = true;
    }
    *out = desc1.ColorSpace;
    return true;
}

d3d12::ColorMode dxgi_color_mode(DXGI_COLOR_SPACE_TYPE color_space,
                                  DXGI_FORMAT format) noexcept {
    if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
        format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return d3d12::ColorMode::Hdr10;
    if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 &&
        format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        return d3d12::ColorMode::ScRgb;
    if (color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 &&
        (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_R10G10B10A2_UNORM))
        return d3d12::ColorMode::Sdr;
    return d3d12::ColorMode::Unknown;
}

constexpr d3d12::ColorMode nvapi_color_mode(int32_t hdr_mode,
                                             DXGI_FORMAT format) noexcept {
    if (hdr_mode == static_cast<int32_t>(NvapiHdrMode::UhdaPassthrough) &&
        format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return d3d12::ColorMode::Hdr10;
    if (hdr_mode == static_cast<int32_t>(NvapiHdrMode::Uhda) &&
        format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        return d3d12::ColorMode::ScRgb;
    if (hdr_mode == static_cast<int32_t>(NvapiHdrMode::Off) &&
        (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_R10G10B10A2_UNORM))
        return d3d12::ColorMode::Sdr;
    return d3d12::ColorMode::Unknown;
}

static_assert(nvapi_color_mode(5, DXGI_FORMAT_R10G10B10A2_UNORM) ==
              d3d12::ColorMode::Hdr10);
static_assert(nvapi_color_mode(2, DXGI_FORMAT_R16G16B16A16_FLOAT) ==
              d3d12::ColorMode::ScRgb);

InferredColor infer_color_mode(uint64_t generation, DXGI_FORMAT format, HWND hwnd,
                               IDXGISwapChain3* swapchain) {
    const ColorObservation* dxgi_observation = nullptr;
    for (const auto& item : g_color_observations) {
        if (item.generation == generation && item.known) {
            dxgi_observation = &item;
            break;
        }
    }

    const NvapiColorSnapshot nvapi_observation =
        latest_nvapi_color_for_window(generation, hwnd);
    const uint64_t dxgi_sequence =
        dxgi_observation ? dxgi_observation->event_sequence : 0;
    if (nvapi_observation.valid &&
        nvapi_observation.event_sequence > dxgi_sequence) {
        return {nvapi_color_mode(nvapi_observation.hdr_mode, format),
                d3d12::ColorEvidence::NvidiaNvapi};
    }
    if (dxgi_observation) {
        return {dxgi_color_mode(dxgi_observation->color_space, format),
                d3d12::ColorEvidence::Dxgi};
    }

    // Neither observer had anything, which is the normal state on a non-NVIDIA card whose
    // swapchain predates our hooks. Ask the display. An SDR desktop is proof enough that what we
    // draw will be read as SDR, and an HDR desktop names the encoding outright; only a pairing that
    // makes no sense (dxgi_color_mode -> Unknown) falls through.
    DXGI_COLOR_SPACE_TYPE display_space = DXGI_COLOR_SPACE_CUSTOM;
    if (display_color_space(swapchain, hwnd, &display_space)) {
        const d3d12::ColorMode mode = dxgi_color_mode(display_space, format);
        if (mode != d3d12::ColorMode::Unknown)
            return {mode, d3d12::ColorEvidence::DxgiOutput};
    }

    // An 8-bit buffer cannot be either HDR encoding, so this one is safe as it always was.
    if (format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        format != DXGI_FORMAT_R10G10B10A2_UNORM)
        return {d3d12::ColorMode::Sdr, d3d12::ColorEvidence::AssumedFromFormat};

    // LAST RESORT. This used to `return {}`, which fails closed - correct in the abstract, and in
    // practice it meant report 42's player had no menu at all, for ever, with no setting to
    // override it. A wrong encoding is a menu that looks wrong and can be reported; a refusal is a
    // menu that is not there. So guess, take the guess that is right far more often (an FP16 chain
    // is essentially always scRGB, anything else SDR), and say so in the log every time.
    const d3d12::ColorMode guess = format == DXGI_FORMAT_R16G16B16A16_FLOAT
                                       ? d3d12::ColorMode::ScRgb
                                       : d3d12::ColorMode::Sdr;
    if (g_color_guess_logged_generation != generation) {
        g_color_guess_logged_generation = generation;
        flog("[overlay-v2] colour encoding unproven for generation %llu (buffer %u, display space "
             "%d): drawing as %s. If the overlay looks washed out or over-bright, that is this "
             "guess being wrong - say so and it can be pinned.",
             static_cast<unsigned long long>(generation), static_cast<unsigned>(format),
             static_cast<int>(display_space),
             guess == d3d12::ColorMode::ScRgb ? "scRGB" : "SDR");
    }
    return {guess, d3d12::ColorEvidence::AssumedFallback};
}

bool valid_game_window(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd)) return false;
    return true;
}

bool valid_creation_window(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0)
        return false;
    return GetAncestor(hwnd, GA_ROOT) == hwnd;
}

bool supported_backbuffer_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// Every rejection here is a silent per-Present no-op, which makes a chain that
// validates on one loader and not another impossible to diagnose from a log.
// `reason` names the failing check so the caller can report it once.
bool describe_swapchain(IDXGISwapChain* base,
                        ComPtr<IDXGISwapChain3>& swapchain3,
                        ComPtr<ID3D12Device>& device,
                        uint64_t& generation,
                        void*& device_id,
                        HWND& hwnd,
                        DXGI_SWAP_CHAIN_DESC1& desc,
                        const char*& reason) noexcept {
    reason = "renderer_ineligible";
    if (!base || !dxgi_shadow::renderer_eligible(base) ||
        FAILED(base->QueryInterface(IID_PPV_ARGS(&swapchain3))) || !swapchain3)
        return false;
    reason = "no_d3d12_device"; // rejects D3D11 and foreign helper swapchains
    if (FAILED(base->GetDevice(IID_PPV_ARGS(&device))) || !device)
        return false;

    reason = "getdesc1_failed";
    if (FAILED(swapchain3->GetDesc1(&desc)))
        return false;
    reason = "hwnd_not_a_valid_game_window"; // composition/CoreWindow/hidden
    if (FAILED(swapchain3->GetHwnd(&hwnd)) || !valid_game_window(hwnd))
        return false;

    reason = "buffer_count_or_sample_count";
    if (desc.BufferCount < 2 || desc.BufferCount > 8 || desc.SampleDesc.Count != 1)
        return false;
    reason = "unsupported_format";
    if (!supported_backbuffer_format(desc.Format)) return false;
    reason = "not_a_render_target";
    if ((desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0) return false;
    constexpr UINT kUnsupportedFlags =
        DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT |
        DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY |
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;
    reason = "restricted_or_protected_flags";
    if ((desc.Flags & kUnsupportedFlags) != 0) return false;
    reason = "not_a_flip_model_swapchain";
    if (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        return false;

    generation = dxgi_shadow::generation_token(base);
    device_id = com_identity(device.Get());
    reason = "no_generation_or_device_identity";
    if (!generation || !device_id) return false;

    ComPtr<ID3D12Resource> backbuffer;
    reason = "getbuffer_failed";
    if (FAILED(swapchain3->GetBuffer(swapchain3->GetCurrentBackBufferIndex(),
                                     IID_PPV_ARGS(&backbuffer))) || !backbuffer)
        return false;
    const D3D12_RESOURCE_DESC resource_desc = backbuffer->GetDesc();
    reason = "backbuffer_shape_mismatch";
    if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resource_desc.DepthOrArraySize != 1 || resource_desc.MipLevels != 1 ||
        resource_desc.SampleDesc.Count != 1 ||
        resource_desc.Format != desc.Format ||
        (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
        resource_desc.Width > UINT_MAX)
        return false;
    if (desc.Width == 0) desc.Width = static_cast<UINT>(resource_desc.Width);
    if (desc.Height == 0) desc.Height = resource_desc.Height;
    reason = "backbuffer_size_mismatch";
    if (resource_desc.Width != desc.Width || resource_desc.Height != desc.Height)
        return false;
    // Tiny video/probe/helper chains are never the Elden Ring presentation
    // surface.  Requiring a plausible game canvas makes initial selection less
    // dependent on whichever injected module happens to Present first.
    reason = "canvas_below_640x360";
    if (desc.Width < 640 || desc.Height < 360) return false;
    reason = nullptr;
    return true;
}

bool eligible_shadow_candidate(IDXGISwapChain* base) noexcept {
    if (!base) return false;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGISwapChain1> swapchain1;
    if (FAILED(base->GetDevice(IID_PPV_ARGS(&device))) || !device ||
        FAILED(base->QueryInterface(IID_PPV_ARGS(&swapchain1))) ||
        !swapchain1)
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    HWND hwnd = nullptr;
    if (FAILED(swapchain1->GetDesc1(&desc)) ||
        FAILED(swapchain1->GetHwnd(&hwnd)) || !valid_creation_window(hwnd) ||
        desc.BufferCount < 2 || desc.BufferCount > 8 ||
        desc.SampleDesc.Count != 1 || !supported_backbuffer_format(desc.Format) ||
        (desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0 ||
        (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
         desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL))
        return false;

    constexpr UINT kUnsupportedFlags =
        DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT |
        DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY |
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;
    if ((desc.Flags & kUnsupportedFlags) != 0) return false;

    if (desc.Width < 640 || desc.Height < 360) {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(base->GetBuffer(0, IID_PPV_ARGS(&buffer))) || !buffer)
            return false;
        const D3D12_RESOURCE_DESC resource = buffer->GetDesc();
        if (resource.Width < 640 || resource.Width > UINT_MAX ||
            resource.Height < 360)
            return false;
    }
    return true;
}

bool select_primary(uint64_t generation, HWND hwnd, void* device_identity,
                    uint32_t width, uint32_t height,
                    RetiredSessions& retired, size_t& retired_count) {
    const uint64_t now = now_ticks();
    ObservedSwapchain* observed = nullptr;
    for (auto& item : g_observed_swapchains) {
        if (item.generation == generation) {
            observed = &item;
            break;
        }
    }
    if (!observed) {
        if (g_observed_swapchains.size() >= 16) {
            const auto discard = std::find_if(
                g_observed_swapchains.begin(), g_observed_swapchains.end(),
                [](const ObservedSwapchain& item) {
                    return item.generation != g_primary_generation;
                });
            if (discard != g_observed_swapchains.end())
                g_observed_swapchains.erase(discard);
        }
        g_observed_swapchains.push_back({generation, hwnd, device_identity,
                                         width, height, 0, now});
        observed = &g_observed_swapchains.back();
    }

    const bool same_shape = observed->hwnd == hwnd &&
        observed->device_identity == device_identity &&
        observed->width == width && observed->height == height &&
        now - observed->last_seen_ms <= 500;
    observed->consecutive_presents = same_shape
        ? std::min(observed->consecutive_presents + 1, 1000u) : 1u;
    observed->hwnd = hwnd;
    observed->device_identity = device_identity;
    observed->width = width;
    observed->height = height;
    observed->last_seen_ms = now;

    if (!g_primary_generation) {
        // Sustained foreground presents reject launchers, setup probes, video,
        // and helper swapchains without relying on DLL-name allowlists.
        const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        if (observed->consecutive_presents < 8 || foreground != hwnd) {
            // Both gates are otherwise invisible and retried forever, so a
            // chain that presents fine but never gets selected looks exactly
            // like one that never presents at all.  Report the blocking
            // reason once, and again only if it changes.
            const char* blocker = observed->consecutive_presents < 8
                                      ? "fewer than 8 sustained presents"
                                      : "the game window is not foreground";
            if (g_primary_block_reason != blocker) {
                g_primary_block_reason = blocker;
                flog("[overlay-v2] primary selection blocked: %s "
                     "(presents=%u, swapchain hwnd=%p, foreground root=%p)",
                     blocker, observed->consecutive_presents, hwnd, foreground);
            }
            return false;
        }
        g_primary_block_reason = nullptr;
        g_primary_generation = generation;
        g_primary_hwnd = hwnd;
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
        flog("[overlay-v2] selected primary game swapchain (%ux%u)", width, height);
    } else if (g_primary_generation != generation) {
        // Keep a healthy primary sticky.  A new swapchain is adopted only after
        // the old one has stopped presenting, as happens during a real rebuild.
        auto old = std::find_if(g_observed_swapchains.begin(), g_observed_swapchains.end(),
            [](const ObservedSwapchain& item) {
                return item.generation == g_primary_generation;
            });
        if (old != g_observed_swapchains.end() && now - old->last_seen_ms < 2000)
            return false;
        if (observed->consecutive_presents < 8 ||
            GetAncestor(GetForegroundWindow(), GA_ROOT) != hwnd)
            return false;
        if (g_resize_gate.depth != 0 || g_color_gate.depth != 0)
            return false;
        if (g_session && !g_session->gpu_idle())
            return false;
        retire_current_session(retired, retired_count);
        g_session_rebuild_pending = false;
        g_primary_generation = generation;
        g_primary_hwnd = hwnd;
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
        flog("[overlay-v2] adopted replacement primary swapchain (%ux%u)", width, height);
    }
    return g_primary_generation == generation;
}

// While the menu is open: how many shadowed Presents ran, how many submitted an overlay frame, and
// the furthest checkpoint the last one reached - the backend otherwise leaves a Present in a dozen
// silent places, and "adopted, renderer initialized, nothing on screen" needs to say which one.
// Logged and reset by log_render_stats() on the control thread.
std::atomic<uint32_t> g_diag_open_presents{0};
std::atomic<uint32_t> g_diag_open_submitted{0};
std::atomic<const char*> g_diag_last_stage{nullptr};
std::atomic<uint32_t> g_diag_last_slot{UINT32_MAX};
std::atomic<uint32_t> g_diag_slot_changes{0};

struct PresentStageTrace {
    const char* stage = "entry";
    ~PresentStageTrace() {
        if (!g_visible.load(std::memory_order_relaxed)) return;
        g_diag_open_presents.fetch_add(1, std::memory_order_relaxed);
        g_diag_last_stage.store(stage, std::memory_order_relaxed);
    }
};

void before_present_impl(IDXGISwapChain* base,
                         dxgi_shadow::PresentKind present_kind, UINT flags,
                         const DXGI_PRESENT_PARAMETERS* parameters) {
    PresentStageTrace trace;
    if (g_stopping.load(std::memory_order_acquire) ||
        !g_renderer_ready.load(std::memory_order_acquire) ||
        (flags & DXGI_PRESENT_TEST) != 0)
        return;

    // A proxy can synchronously Present another canonical swapchain while a
    // CustomTalismanEffects render operation is validating COM state.  The shadow layer's
    // identity-keyed recursion guard intentionally treats those as different
    // objects, so reject the cross-identity re-entry here before touching the
    // non-recursive renderer mutex already owned by this thread.
    if (g_present_render_critical)
        return;

    // Declared before the lock so all Session destruction (and its foreign COM
    // Release cascade) happens only after render_lock has been released.
    RetiredSessions retired_sessions{};
    size_t retired_session_count = 0;
    ComPtr<IDXGISwapChain3> swapchain3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::unique_ptr<d3d12::Session> candidate;
    uint64_t generation = 0;
    void* device_id = nullptr;
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    trace.stage = "render lock";
    std::unique_lock render_lock(g_render_mutex, std::try_to_lock);
    if (!render_lock.owns_lock()) return;
    PresentRenderCriticalScope present_critical;

    trace.stage = "generation";
    const uint64_t call_generation = dxgi_shadow::generation_token(base);
    if (!call_generation) return;
    trace.stage = "eligibility";

    // The single most valuable line in the backend log: it splits "our vtable
    // shadow is never invoked" (line absent) from "our validation rejects the
    // chain" (line present, followed by a reject reason).
    if (g_present_reached_logged_generation != call_generation) {
        g_present_reached_logged_generation = call_generation;
        flog("[overlay-v2] shadowed Present reached the overlay for generation %llu",
             static_cast<unsigned long long>(call_generation));
    }

    // Lifetime identity must survive a partial alias installation so resize
    // and device-loss cleanup still work. Rendering eligibility is a separate,
    // stricter gate. If eligibility is ever revoked on the selected chain,
    // immediately withdraw the canvas and retire the session once its own
    // fence proves that destruction is safe.
    if (!dxgi_shadow::renderer_eligible(base)) {
        static uint64_t s_reason_logged_generation = 0;
        if (call_generation != s_reason_logged_generation) {
            s_reason_logged_generation = call_generation;
            flog("[overlay-v2] generation %llu is not renderable: %s",
                 static_cast<unsigned long long>(call_generation),
                 dxgi_shadow::ineligibility_reason(base));
        }
        if (call_generation == g_primary_generation) {
            publish_canvas(g_primary_hwnd, 0, 0, false, false);
            if (g_session) {
                if (g_session->gpu_idle()) {
                    retire_current_session(retired_sessions,
                                           retired_session_count);
                    g_session_rebuild_pending = false;
                } else {
                    g_session_rebuild_pending = true;
                }
            }
            g_retry_on_next_open.store(false, std::memory_order_release);
            g_renderer_healthy.store(false, std::memory_order_release);
        }
        return;
    }
    trace.stage = "resize/color gate";
    if ((g_resize_gate.depth != 0 &&
         call_generation == g_resize_gate.generation) ||
        (g_color_gate.depth != 0 &&
         call_generation == g_color_gate.generation))
        return;

    trace.stage = "session rebuild pending";
    if (g_session_rebuild_pending && g_session && g_session->gpu_idle()) {
        retire_current_session(retired_sessions, retired_session_count);
        g_session_rebuild_pending = false;
        g_retry_on_next_open.store(true, std::memory_order_release);
    }
    if (g_session_rebuild_pending) return;

    trace.stage = "swapchain validation";
    const char* describe_reason = nullptr;
    if (!describe_swapchain(base, swapchain3, device, generation, device_id,
                            hwnd, desc, describe_reason)) {
        if (g_describe_reject_logged_generation != call_generation ||
            g_describe_reject_reason != describe_reason) {
            g_describe_reject_logged_generation = call_generation;
            g_describe_reject_reason = describe_reason;
            flog("[overlay-v2] swapchain validation rejected generation %llu: %s",
                 static_cast<unsigned long long>(call_generation),
                 describe_reason ? describe_reason : "unknown");
        }
        return;
    }
    if (g_describe_reject_reason) {
        g_describe_reject_reason = nullptr;
        flog("[overlay-v2] swapchain validation now passes for generation %llu",
             static_cast<unsigned long long>(call_generation));
    }

    // Queue identity is part of swapchain identity for D3D12.  Do not publish
    // a usable canvas (and therefore do not capture input) until the exact
    // queue supplied to CreateSwapChain* or ResizeBuffers1 is known.
    trace.stage = "queue binding";
    queue = queue_for_swapchain(generation, device_id);
    if (!queue) {
        if (g_queue_missing_logged_generation != generation) {
            g_queue_missing_logged_generation = generation;
            flog("[overlay-v2] no exact queue bound for generation %llu; "
                 "canvas stays unavailable",
                 static_cast<unsigned long long>(generation));
        }
        if (generation == g_primary_generation)
            publish_canvas(hwnd, desc.Width, desc.Height, false, false);
        return;
    }
    trace.stage = "primary selection";
    if (!select_primary(generation, hwnd, device_id, desc.Width, desc.Height,
                        retired_sessions, retired_session_count))
        return;

    trace.stage = "back-buffer index";
    const uint32_t current_slot = swapchain3->GetCurrentBackBufferIndex();
    if (current_slot != g_diag_last_slot.exchange(current_slot, std::memory_order_relaxed))
        g_diag_slot_changes.fetch_add(1, std::memory_order_relaxed);
    if (current_slot >= desc.BufferCount) return;
    bool already_composited = false;
    if (g_pending_composited_buffer.generation != 0) {
        if (g_pending_composited_buffer.generation == generation &&
            g_pending_composited_buffer.slot == current_slot) {
            // The preceding downstream Present did not consume this buffer.
            // Its CustomTalismanEffects pixels are already present; drawing again would
            // compound straight-alpha blending on a retry.
            already_composited = true;
        } else {
            g_pending_composited_buffer = {};
        }
    }

    trace.stage = "color encoding";
    const InferredColor color =
        infer_color_mode(generation, desc.Format, hwnd, swapchain3.Get());
    const d3d12::ColorMode color_mode = color.mode;
    const bool color_known = color_mode != d3d12::ColorMode::Unknown;
    publish_canvas(hwnd, desc.Width, desc.Height,
                   color_mode == d3d12::ColorMode::ScRgb ||
                       color_mode == d3d12::ColorMode::Hdr10,
                   color_known);

    if (!color_known) {
        if (g_color_unknown_logged_generation != generation) {
            g_color_unknown_logged_generation = generation;
            flog("[overlay-v2] primary color encoding is unverified (format %u); canvas remains unavailable",
                 static_cast<unsigned>(desc.Format));
        }
        return; // detected nested R10/FP16 attachment is ambiguous; fail closed
    }

    // A renderer failure is sticky for this session.  Continue lifecycle and
    // canvas observation, but never repeat allocation/recording failures on
    // every Present.  Pending work or a replacement tuple explicitly rearms.
    trace.stage = "renderer unhealthy";
    if (!g_renderer_healthy.load(std::memory_order_acquire)) return;

    trace.stage = "session setup";
    // Warm all private device-child state as soon as the primary tuple and
    // encoding are authoritative. Opening the menu then performs no heap/PSO/
    // backbuffer allocation on its first visible Present.
    if (g_session &&
        !g_session->matches(swapchain3.Get(), queue.Get(), color_mode)) {
        if (!g_session->gpu_idle()) return;
        retire_current_session(retired_sessions, retired_session_count);
        g_session_rebuild_pending = false;
    }
    if (!g_session) {
        candidate = std::make_unique<d3d12::Session>();
        bool initialized = false;
        try {
            initialized = candidate->initialize(
                swapchain3.Get(), queue.Get(), color_mode, color.evidence);
        } catch (...) {
            cte::note_swallowed(__FILE__, __LINE__);
            if (retired_session_count < retired_sessions.size()) {
                retired_sessions[retired_session_count++] =
                    std::move(candidate);
            } else {
                (void)candidate.release();
            }
            throw;
        }
        if (!initialized) {
            if (retired_session_count < retired_sessions.size()) {
                retired_sessions[retired_session_count++] =
                    std::move(candidate);
            } else {
                (void)candidate.release();
            }
            g_retry_on_next_open.store(true, std::memory_order_release);
            g_renderer_healthy.store(false, std::memory_order_release);
            return;
        }
        g_session = std::move(candidate);
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
    }

    if (!g_visible.load(std::memory_order_acquire)) return;

    // A nonblocking Present is explicitly allowed to reject the frame without
    // consuming the current buffer. Never modify it in that mode. Likewise,
    // null parameters make Present1 invalid, whereas null denotes an ordinary
    // full-frame Present in the base interface.
    trace.stage = already_composited ? "same buffer still marked as composited"
                                     : "present flags";
    if ((flags & DXGI_PRESENT_DO_NOT_WAIT) != 0 ||
        (present_kind == dxgi_shadow::PresentKind::Present1 && !parameters) ||
        already_composited)
        return;

    // A generic overlay cannot modify pixels outside Present1's declared dirty
    // regions without violating DXGI's partial-present contract.  Elden Ring
    // uses full presents; unfamiliar incremental chains are passed through.
    if (parameters && (parameters->DirtyRectsCount != 0 ||
                       parameters->pScrollRect != nullptr ||
                       parameters->pScrollOffset != nullptr))
        return;

    trace.stage = "draw packet";
    const auto packet = frame::acquire_frame();
    const auto font = frame::acquire_font_atlas();
    if (!packet || !font || packet->font_generation != font->generation ||
        packet->commands.empty())
        return;
    trace.stage = "packet size vs back buffer";

    // Do not stretch a stale pre-resize UI packet onto a new buffer.  The
    // control thread will publish the correctly sized frame on its next tick.
    const double expected_w_value =
        static_cast<double>(packet->display_size.x) *
        packet->framebuffer_scale.x + 0.5;
    const double expected_h_value =
        static_cast<double>(packet->display_size.y) *
        packet->framebuffer_scale.y + 0.5;
    if (!std::isfinite(expected_w_value) ||
        !std::isfinite(expected_h_value) || expected_w_value < 0.0 ||
        expected_h_value < 0.0 ||
        expected_w_value > std::numeric_limits<uint32_t>::max() ||
        expected_h_value > std::numeric_limits<uint32_t>::max())
        return;
    const auto expected_w = static_cast<uint32_t>(expected_w_value);
    const auto expected_h = static_cast<uint32_t>(expected_h_value);
    if (expected_w != desc.Width || expected_h != desc.Height) return;

    trace.stage = "render";
    const d3d12::RenderResult result =
        g_session->render(swapchain3.Get(), packet.get(), font.get(), color_mode);
    if (result == d3d12::RenderResult::Submitted) {
        trace.stage = "submitted";
        g_diag_open_submitted.fetch_add(1, std::memory_order_relaxed);
        g_pending_composited_buffer = {generation, current_slot};
    } else if (result == d3d12::RenderResult::Skipped) {
        const char* why = d3d12::last_render_skip();
        trace.stage = why ? why : "renderer skipped";
    } else if (result == d3d12::RenderResult::RecoverableFailure) {
        // No command list was submitted for this result.  Rebuild every
        // renderer-owned object on the next explicit open instead of carrying
        // partially committed font/descriptor bookkeeping forward.
        flog("[overlay-v2] renderer recording failed; discarding private session");
        g_session_rebuild_pending = true;
        g_renderer_healthy.store(false, std::memory_order_release);
    } else if (result == d3d12::RenderResult::DeviceLost) {
        const HRESULT reason = g_session->device_removed_reason();
        flog("[overlay-v2] D3D12 device lost while rendering (0x%08X); pass-through",
             static_cast<unsigned>(reason));
        if (FAILED(reason)) {
            retire_current_session(retired_sessions, retired_session_count);
            g_session_rebuild_pending = false;
        } else {
            // Execute may have succeeded even though Signal failed.  Without a
            // fence or confirmed removal there is no proof the GPU is done.
            // Intentionally retain the session for process life.
            (void)g_session.release();
            g_session_rebuild_pending = false;
            flog("[overlay-v2] [WARN] unfenced session quarantined after queue failure");
        }
        publish_canvas(g_primary_hwnd, 0, 0, false, false);
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void safe_before_present(IDXGISwapChain* swapchain,
                         dxgi_shadow::PresentKind present_kind, UINT flags,
                         const DXGI_PRESENT_PARAMETERS* parameters) noexcept {
    try {
        before_present_impl(swapchain, present_kind, flags, parameters);
    } catch (...) {
        cte::note_swallowed(__FILE__, __LINE__);
        // C++ failures are contained before the downstream call.  Structured
        // exceptions are not caught across RAII-held mutexes because doing so
        // would skip destructors and permanently poison the hook path.
        try {
            std::unique_lock lock(g_render_mutex, std::try_to_lock);
            if (lock.owns_lock() && g_session)
                g_session_rebuild_pending = true;
            else if (lock.owns_lock())
                g_retry_on_next_open.store(true, std::memory_order_release);
        } catch (...) {
            note_swallowed_exception(__LINE__);
        }
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void after_present(IDXGISwapChain* swapchain, HRESULT result) noexcept {
    if (g_stopping.load(std::memory_order_acquire)) return;
    if (SUCCEEDED(result)) {
        // The downstream Present consumed the buffer the overlay drew into, so the next Present
        // on the same slot carries a fresh game frame. The marker used to clear only when the
        // swapchain reported a DIFFERENT back-buffer index - behind a layer that keeps reporting
        // the same one (a frame-generation proxy can), every frame after the first was taken for
        // an unconsumed retry and skipped: one overlay frame, then nothing.
        try {
            std::unique_lock lock(g_render_mutex, std::try_to_lock);
            if (lock.owns_lock() && g_pending_composited_buffer.generation != 0 &&
                g_pending_composited_buffer.generation ==
                    dxgi_shadow::generation_token(swapchain))
                g_pending_composited_buffer = {};
        } catch (...) {
            note_swallowed_exception(__LINE__);
        }
        return;
    }
    if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET)
        return;
    if (g_present_render_critical) {
        g_renderer_healthy.store(false, std::memory_order_release);
        g_retry_on_next_open.store(false, std::memory_order_release);
        return;
    }
    try {
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        std::unique_ptr<d3d12::Session> retired_session;
        {
            std::lock_guard lock(g_render_mutex);
            if (generation != g_primary_generation) return;
            if (g_session) {
                const HRESULT reason = g_session->device_removed_reason();
                if (FAILED(reason)) {
                    retired_session = std::move(g_session);
                } else {
                    // Present failed but the device does not confirm removal.
                    // Its most recent submission may still be live; quarantine
                    // it without running a destructor under the renderer lock.
                    (void)g_session.release();
                }
                g_session_rebuild_pending = false;
            }
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            g_pending_composited_buffer = {};
            publish_canvas(g_primary_hwnd, 0, 0, false, false);
        }
    } catch (...) {
        cte::note_swallowed(__FILE__, __LINE__);
        // A removed device is already unusable; pass-through remains active.
    }
}

bool before_resize_impl(IDXGISwapChain* swapchain) {
    if (g_stopping.load(std::memory_order_acquire)) return true;
    if (g_present_render_critical) {
        g_renderer_healthy.store(false, std::memory_order_release);
        g_retry_on_next_open.store(false, std::memory_order_release);
        return false;
    }
    if (!swapchain) return true;
    const uint64_t generation = dxgi_shadow::generation_token(swapchain);
    std::unique_ptr<d3d12::Session> retiring_session;
    {
        std::lock_guard lock(g_render_mutex);
        if (generation == 0 || generation != g_primary_generation)
            return true;

        const bool already_retiring =
            g_resize_gate.depth != 0 &&
            g_resize_gate.generation == generation;
        if (g_resize_gate.depth == 0) g_resize_gate.generation = generation;
        if (g_resize_gate.generation == generation) ++g_resize_gate.depth;
        publish_canvas(g_primary_hwnd, 0, 0, false, false);

        // Concurrent ResizeBuffers calls on one swapchain are invalid host
        // behavior. The first owner may currently hold the only session outside
        // this mutex while draining it, so later calls must report that
        // resources were not released and let DXGI reject the overlap.
        if (already_retiring && !g_session)
            return false;

        if (g_session) {
            retiring_session = std::move(g_session);
            g_session_rebuild_pending = false;
        }
    }

    if (!retiring_session) return true;

    // The generation-keyed resize gate blocks matching Presents and prevents
    // primary replacement while this runs. D3D12/COM resource retirement is
    // intentionally outside g_render_mutex so a proxy Release cannot re-enter
    // a blocking lifecycle callback and deadlock the process.
    if (retiring_session->before_resize())
        return true;

    {
        std::lock_guard lock(g_render_mutex);
        if (!g_stopping.load(std::memory_order_acquire) &&
            generation == g_primary_generation && !g_session) {
            // Keep the live references so the downstream resize fails safely.
            g_session = std::move(retiring_session);
        }
        g_renderer_healthy.store(false, std::memory_order_release);
        g_retry_on_next_open.store(true, std::memory_order_release);
    }
    if (retiring_session) {
        // Coordinator ownership changed while the fence wait was in flight.
        // Completion is unproven, so quarantine instead of destroying.
        (void)retiring_session.release();
    }
    return false;
}

bool safe_before_resize(IDXGISwapChain* swapchain) noexcept {
    try {
        return before_resize_impl(swapchain);
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
        return false;
    }
}

void after_resize(IDXGISwapChain* swapchain, HRESULT result,
                  bool resources_released) noexcept {
    try {
        if (g_present_render_critical) {
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            return;
        }
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        std::lock_guard lock(g_render_mutex);
        if (g_resize_gate.generation == generation &&
            g_resize_gate.depth != 0) {
            --g_resize_gate.depth;
            if (g_resize_gate.depth == 0) g_resize_gate.generation = 0;
        }
        if (generation != g_primary_generation) return;
        if (g_stopping.load(std::memory_order_acquire)) return;
        if (FAILED(result)) {
            if (result == DXGI_ERROR_DEVICE_REMOVED ||
                result == DXGI_ERROR_DEVICE_RESET) {
                g_retry_on_next_open.store(false, std::memory_order_release);
                g_renderer_healthy.store(false, std::memory_order_release);
            }
            return;
        }
        if (!resources_released) {
            // This should be unreachable for a conforming ResizeBuffers call:
            // our retained backbuffer refs should make it fail.  If a wrapper
            // nevertheless reports success, retain the unfenced old session
            // for process life and rediscover the replacement from scratch.
            if (g_session) (void)g_session.release();
            g_session_rebuild_pending = false;
            g_retry_on_next_open.store(false, std::memory_order_release);
            g_renderer_healthy.store(false, std::memory_order_release);
            flog("[overlay-v2] [WARN] resize succeeded with quarantined GPU resources");
            return;
        }
        g_pending_composited_buffer = {};
        g_retry_on_next_open.store(false, std::memory_order_release);
        g_renderer_healthy.store(true, std::memory_order_release);
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void begin_color_space(IDXGISwapChain3* swapchain) noexcept {
    try {
        if (g_present_render_critical) {
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            return;
        }
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation) return;
        std::lock_guard lock(g_render_mutex);
        if (generation == g_primary_generation) {
            if (g_color_gate.depth == 0) g_color_gate.generation = generation;
            if (g_color_gate.generation == generation) ++g_color_gate.depth;
        }
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void complete_color_space(IDXGISwapChain3* swapchain,
                          DXGI_COLOR_SPACE_TYPE color_space,
                          HRESULT result) noexcept {
    try {
        if (g_present_render_critical) {
            g_renderer_healthy.store(false, std::memory_order_release);
            g_retry_on_next_open.store(false, std::memory_order_release);
            return;
        }
        const uint64_t generation =
            dxgi_shadow::generation_token(swapchain);
        if (!generation) return;
        const bool successful =
            !g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result);
        const uint64_t observed_sequence =
            successful ? next_color_event_sequence() : 0;
        std::lock_guard lock(g_render_mutex);
        if (g_color_gate.generation == generation &&
            g_color_gate.depth != 0) {
            --g_color_gate.depth;
            if (g_color_gate.depth == 0) g_color_gate.generation = 0;
        }
        const bool primary = generation == g_primary_generation;
        if (!successful) return;
        ColorObservation* record = color_record(generation);
        record->color_space = color_space;
        record->event_sequence = observed_sequence;
        record->known = true;
        flog("[overlay-v2] observed successful SetColorSpace1 "
             "(generation=%llu, space=%u, primary=%s)",
             static_cast<unsigned long long>(generation),
             static_cast<unsigned>(color_space), primary ? "yes" : "no");
        if (primary) {
            // A mode mismatch retires/rebuilds the private Session on the next
            // Present. CustomTalismanEffects never changes host color state itself.
            const bool hdr =
                color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            const Canvas old = canvas();
            publish_canvas(old.hwnd, old.width, old.height, hdr, old.ready);
        }
    } catch (...) {
        note_swallowed_exception(__LINE__);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

// Adapters from the per-instance vtable layer into the renderer coordinator.
// The shadow module owns exact per-object downstream pointers; these callbacks
// never call DXGI themselves and therefore cannot bypass another hook.
void shadow_before_present(IDXGISwapChain* swapchain,
                           const dxgi_shadow::PresentCall& call) {
    safe_before_present(swapchain, call.kind, call.flags, call.parameters);
}

void shadow_after_present(IDXGISwapChain* swapchain,
                          const dxgi_shadow::PresentCall&, HRESULT result) {
    after_present(swapchain, result);
}

bool shadow_before_resize(IDXGISwapChain* swapchain,
                          const dxgi_shadow::ResizeCall&) {
    if (g_resize_transaction.depth == 0)
        g_resize_transaction.id = next_binding_transaction();
    ++g_resize_transaction.depth;
    return safe_before_resize(swapchain);
}

void shadow_after_resize(IDXGISwapChain* swapchain,
                         const dxgi_shadow::ResizeCall& call, HRESULT result,
                         bool resources_released) {
    struct TransactionEnd {
        ~TransactionEnd() {
            if (g_resize_transaction.depth != 0)
                --g_resize_transaction.depth;
            if (g_resize_transaction.depth == 0)
                g_resize_transaction.id = 0;
        }
    } transaction_end;
    const uint64_t transaction_id = g_resize_transaction.id;
    const uint32_t evidence_depth = g_resize_transaction.depth;
    if (!g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        call.kind == dxgi_shadow::ResizeKind::ResizeBuffers1) {
        ComPtr<IDXGISwapChain3> swapchain3;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) &&
            swapchain3) {
            bind_resize_queues(swapchain3.Get(), call.buffer_count,
                               call.creation_node_masks, call.present_queues,
                               transaction_id, evidence_depth);
        }
    }
    after_resize(swapchain, result, resources_released);
}

void shadow_before_color_space(IDXGISwapChain3* swapchain,
                               DXGI_COLOR_SPACE_TYPE) {
    begin_color_space(swapchain);
}

void shadow_after_color_space(IDXGISwapChain3* swapchain,
                              DXGI_COLOR_SPACE_TYPE color_space,
                              HRESULT result) {
    complete_color_space(swapchain, color_space, result);
}

// -------------------------------------------------------------------------
// Hook functions and immediate-next trampolines

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using NvapiGetDisplayIdByNameFn =
    int(__cdecl*)(const char*, uint32_t*);
using NvapiHdrColorControlFn =
    int(__cdecl*)(uint32_t, NvapiHdrColorDataHeader*);

CreateSwapChainFn g_next_create_swapchain = nullptr;
CreateSwapChainForHwndFn g_next_create_swapchain_for_hwnd = nullptr;

// The creation entry points as they were BEFORE anyone (including us) touched the factory
// vtable. Captured as early as we run, because the value only means something while it is
// still pristine: in the report-21 session we loaded 1.3 s ahead of the other overlay, which
// is the normal order for me3 natives. Used solely to break a proxy loop - see the note above
// create_swapchain_detour. nullptr = we were not early enough, and the loop is then refused
// rather than forwarded.
void* g_pristine_create_swapchain = nullptr;
void* g_pristine_create_swapchain_for_hwnd = nullptr;
NvapiGetDisplayIdByNameFn g_next_nvapi_get_display_id_by_name = nullptr;
NvapiHdrColorControlFn g_next_nvapi_hdr_color_control = nullptr;

// ---- late-adoption state (see the adoption section further down) ----
//
// A loader can inject this mod after the game already created its swapchain
// (Elden Mod Loader delays, manual injectors). The creation detours then never
// run, and without this fallback the canvas could never validate. Adoption
// watches live Present calls through pass-through observer hooks and installs
// the same per-instance shadow on the already-existing swapchain.

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandListsFn =
    void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                             ID3D12CommandList* const*);

// Three observer pairs, one per implementation watched: the probe swapchain's (whatever the
// factory hands out), the native one behind a Streamline proxy, and the one the GAME's own code
// was found calling (discovered at run time - see discover_game_present).
PresentFn g_next_present_observed = nullptr;
Present1Fn g_next_present1_observed = nullptr;
PresentFn g_next_native_present_observed = nullptr;
Present1Fn g_next_native_present1_observed = nullptr;
PresentFn g_next_game_present_observed = nullptr;
Present1Fn g_next_game_present1_observed = nullptr;
ExecuteCommandListsFn g_next_execute_command_lists = nullptr;
ExecuteCommandListsFn g_next_native_execute_command_lists = nullptr;
// Set by arm_adoption when a native Present AND a native ExecuteCommandLists sit behind the probe's
// (a Streamline proxy plus a wrapper queue): adoption then prefers DXGI's own Present, where the
// frame on screen is final (try_adopt_native). Written once before the observers go live.
bool g_native_adoption = false;
std::atomic<uint32_t> g_native_presents_without_queue{0};
constexpr uint32_t kNativeAdoptionPatience = 600; // native presents before falling back

// Implementations already observed, so discovery never proposes one of them again.
void* g_observed_present_targets[4] = {};
// The implementation the game's code calls, packed as address | slot << 56 (slot 8 = Present,
// 22 = Present1). Published once by a Present thread, installed by the control thread.
std::atomic<uint64_t> g_game_present_discovery{0};
std::atomic<uint32_t> g_foreign_presents{0};
bool g_game_present_hooked = false; // overlay/control thread only

enum class AdoptionState : uint32_t {
    Idle = 0,     // never armed; zero overhead on the normal creation path
    Pending,      // observers active, waiting for a candidate + queue evidence
    Adopted,      // a live swapchain was shadowed and bound
    Failed,       // permanently ambiguous or unprovable; stay pass-through
};
std::atomic<uint32_t> g_adoption_state{
    static_cast<uint32_t>(AdoptionState::Idle)};
bool g_adoption_hooks_installed = false; // overlay/control thread only

// The probe swapchain used to resolve vtable code pointers must never be
// shadowed, bound, or counted as a game creation.
thread_local bool g_adoption_probe_in_progress = false;

// Forensics: how often the creation detours actually ran, and where the
// installed patches point today. Read by log_creation_hook_forensics().
std::atomic<uint32_t> g_creation_detour_calls{0};

// Set only when a creation detour observed a game swapchain AND shadowed and
// queue-bound it.  This is the evidence the discovery watchdog needs: without
// it, a visible game window is mistaken for a late load.
std::atomic<bool> g_observed_creation{false};

// This mod intercepts DXGI creation via factory vtable slots (see
// install_factory_slot); the code-patch targets are the fallback.  Forensics
// inspects whichever was actually used.
void** g_forensics_create_swapchain_slot = nullptr;
void** g_forensics_create_swapchain_for_hwnd_slot = nullptr;
void* g_forensics_create_swapchain_target = nullptr;
void* g_forensics_create_swapchain_for_hwnd_target = nullptr;

int __cdecl nvapi_get_display_id_by_name_detour(const char* display_name,
                                                 uint32_t* display_id) {
    const int status =
        g_next_nvapi_get_display_id_by_name(display_name, display_id);
    if (!g_stopping.load(std::memory_order_acquire) &&
        g_nvapi_observer_enabled.load(std::memory_order_acquire) &&
        status == 0 && display_name && display_id) {
        try {
            record_nvapi_display_binding(copy_output_name(display_name),
                                         *display_id);
        } catch (...) {
            cte::note_swallowed(__FILE__, __LINE__);
            // The observer is optional. Never change the game's NVAPI result
            // or poison the generic DXGI backend because diagnostics failed.
        }
    }
    return status;
}

int __cdecl nvapi_hdr_color_control_detour(
    uint32_t display_id, NvapiHdrColorDataHeader* color_data) {
    constexpr uint32_t kEldenRingHdrColorDataV1 = 0x00010028u;
    const bool request_valid =
        color_data && color_data->version == kEldenRingHdrColorDataV1;
    const int32_t command = request_valid ? color_data->command : -1;
    const int32_t hdr_mode = request_valid ? color_data->hdr_mode : -1;

    const int status = g_next_nvapi_hdr_color_control(display_id, color_data);
    if (!g_stopping.load(std::memory_order_acquire) &&
        g_nvapi_observer_enabled.load(std::memory_order_acquire) &&
        status == 0 && request_valid &&
        command == static_cast<int32_t>(NvapiHdrCommand::Set)) {
        try {
            record_nvapi_color_observation(display_id, hdr_mode,
                                           "the game's successful SET");
        } catch (...) {
            cte::note_swallowed(__FILE__, __LINE__);
            // Observation is fail-closed and must never alter host behavior.
        }
    }
    return status;
}

bool install_swapchain_shadow(IDXGISwapChain* swapchain) noexcept {
    const dxgi_shadow::InstallResult result = dxgi_shadow::install(swapchain);
    switch (result) {
    case dxgi_shadow::InstallResult::Installed:
    case dxgi_shadow::InstallResult::AlreadyInstalled:
        return dxgi_shadow::generation_token(swapchain) != 0 &&
               dxgi_shadow::renderer_eligible(swapchain);
    case dxgi_shadow::InstallResult::PartiallyInstalled:
        flog("[overlay-v2] [WARN] swapchain aliases only partially shadowed; fail-closed on missing aliases");
        return false;
    case dxgi_shadow::InstallResult::Collision:
        flog("[overlay-v2] [WARN] concurrent swapchain vtable change; alias left untouched");
        return false;
    case dxgi_shadow::InstallResult::OutOfMemory:
        flog("[overlay-v2] [ERROR] swapchain shadow allocation failed");
        return false;
    case dxgi_shadow::InstallResult::InvalidObject:
        flog("[overlay-v2] [WARN] unsupported swapchain interface; pass-through only");
        return false;
    }
    return false;
}

// ── foreign proxy loops: never call g_next from inside ourselves ────────────────────
// Report 21 (2026-08-04): with ERSS/streamline loaded the game died of stack exhaustion,
// 1752 return addresses deep, alternating two addresses inside create_swapchain_for_hwnd_detour
// (~876 nested re-entries). Mechanism: streamline had already interposed creation when we
// installed our vtable-slot detour, so our saved `g_next` leads into THEIR layer - and their
// layer forwards COM-style through the live factory vtable, whose slot now holds OURS. us ->
// their layer -> vtable -> us, forever. The coexistence design documented above covers
// code-patch hooks; a proxy object that re-dispatches through the vtable was not considered.
//
// A legitimate pass-through can never re-enter our own detour on the same thread, so depth is
// the whole test. On re-entry we must NOT call g_next (that IS the loop) - we call the pristine
// DXGI entry point captured before anyone else hooked, which keeps creation working. If we have
// no snapshot (we were not early enough), one failed creation is survivable; a stack overflow is
// not, so the call is refused instead.
thread_local int g_creation_depth = 0;
std::atomic<bool> g_creation_loop_seen{false};

struct CreationDepthScope {
    CreationDepthScope() { ++g_creation_depth; }
    ~CreationDepthScope() { --g_creation_depth; }
    bool reentered() const { return g_creation_depth > 1; }
    // The snapshot is worth ONE attempt, at the first re-entry only. Deeper than that means the
    // snapshot re-entered us as well - it was captured late and is itself a proxy that
    // re-dispatches through the vtable - and forwarding again would just rebuild the cycle
    // through a different door. Two foreign proxies in a specific order are needed to reach
    // this state (the `pristine != next` test catches the single-proxy case), which is exactly
    // why it gets a structural answer rather than another equality check: at depth > 2 nothing
    // in the chain is trustworthy, so the creation is refused. With this, unbounded recursion
    // is impossible by construction, whatever the load order was.
    bool may_use_snapshot() const { return g_creation_depth == 2; }
};

// A snapshot is only usable if it is NOT the pointer that is already looping. If we were late,
// `g_pristine_*` and `g_next_*` are the same foreign proxy by construction, and calling it would
// rebuild the very cycle we are breaking. Then the only safe answer is to refuse the creation.
// This test alone is NOT sufficient - see CreationDepthScope::may_use_snapshot for the case it
// cannot see (two foreign proxies, so the pointers differ yet the snapshot still loops).
bool usable_snapshot(void* pristine, const void* next) {
    return pristine && pristine != next;
}

void note_creation_loop(const char* which, bool forwarding) {
    if (!g_creation_loop_seen.exchange(true))
        flog("[overlay-v2] [WARN] %s re-entered our own detour - another overlay proxies creation "
             "through the factory vtable. Breaking the loop; %s", which,
             forwarding ? "forwarding to the entry point captured at startup."
                        : "no usable startup snapshot, so this creation is refused.");
}

HRESULT STDMETHODCALLTYPE create_swapchain_detour(
    IDXGIFactory* factory, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** result_swapchain) {
    CreationDepthScope depth;
    if (depth.reentered()) {
        const bool forward = depth.may_use_snapshot() &&
                             usable_snapshot(g_pristine_create_swapchain,
                                             reinterpret_cast<const void*>(g_next_create_swapchain));
        note_creation_loop("CreateSwapChain", forward);
        if (forward)
            return reinterpret_cast<CreateSwapChainFn>(g_pristine_create_swapchain)(
                factory, queue, desc, result_swapchain);
        return DXGI_ERROR_INVALID_CALL;
    }
    if (!g_adoption_probe_in_progress)
        g_creation_detour_calls.fetch_add(1, std::memory_order_relaxed);
    BindingScope binding_scope(g_creation_transaction);
    const HRESULT result =
        g_next_create_swapchain(factory, queue, desc, result_swapchain);
    if (!g_adoption_probe_in_progress &&
        !g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        result_swapchain && *result_swapchain &&
        eligible_shadow_candidate(*result_swapchain) &&
        install_swapchain_shadow(*result_swapchain) &&
        bind_swapchain_queue(*result_swapchain, queue,
                             binding_scope.id(), binding_scope.depth())) {
        g_observed_creation.store(true, std::memory_order_release);
        record_creation_color_default(*result_swapchain, binding_scope.id(),
                                      binding_scope.depth());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE create_swapchain_for_hwnd_detour(
    IDXGIFactory2* factory, IUnknown* queue, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output, IDXGISwapChain1** result_swapchain) {
    CreationDepthScope depth;
    if (depth.reentered()) {
        const bool forward = depth.may_use_snapshot() &&
                             usable_snapshot(g_pristine_create_swapchain_for_hwnd,
                                             reinterpret_cast<const void*>(
                                                 g_next_create_swapchain_for_hwnd));
        note_creation_loop("CreateSwapChainForHwnd", forward);
        if (forward)
            return reinterpret_cast<CreateSwapChainForHwndFn>(
                g_pristine_create_swapchain_for_hwnd)(
                factory, queue, hwnd, desc, fullscreen_desc, restrict_to_output,
                result_swapchain);
        return DXGI_ERROR_INVALID_CALL;
    }
    if (!g_adoption_probe_in_progress)
        g_creation_detour_calls.fetch_add(1, std::memory_order_relaxed);
    BindingScope binding_scope(g_creation_transaction);
    const HRESULT result = g_next_create_swapchain_for_hwnd(
        factory, queue, hwnd, desc, fullscreen_desc, restrict_to_output,
        result_swapchain);
    if (!g_adoption_probe_in_progress &&
        !g_stopping.load(std::memory_order_acquire) && SUCCEEDED(result) &&
        result_swapchain && *result_swapchain &&
        eligible_shadow_candidate(*result_swapchain) &&
        install_swapchain_shadow(*result_swapchain) &&
        bind_swapchain_queue(*result_swapchain, queue,
                             binding_scope.id(), binding_scope.depth())) {
        g_observed_creation.store(true, std::memory_order_release);
        record_creation_color_default(*result_swapchain, binding_scope.id(),
                                      binding_scope.depth());
    }
    return result;
}

struct HookTargets {
    void* create_swapchain = nullptr;
    void* create_swapchain_for_hwnd = nullptr;
    void* nvapi_get_display_id_by_name = nullptr;
    void* nvapi_hdr_color_control = nullptr;
    // The factory's shared vtable, for slot interception (see
    // install_factory_slot). Null falls back to code patching.
    void** factory_vtable = nullptr;
};

// ── DXGI creation interception: vtable slot, not code patch ──
// DEVIATION FROM QUESTPATH, and the reason this mod's overlay works while
// QuestPath is loaded. QuestPath MinHook-patches the first five bytes of
// dxgi!CreateSwapChain(ForHwnd). Two injected DLLs each running their own
// MinHook cannot do that concurrently: MH_CreateHook snapshots the target's
// bytes to build its trampoline, so when both create before either applies,
// both trampolines hold the ORIGINAL bytes and the second MH_ApplyQueued
// simply overwrites the first mod's JMP -- the loser's detour is silently
// orphaned for the rest of the session (observed 2026-07-19: both mods logged
// "hooks installed" in the same millisecond; only QuestPath ever saw a
// creation). MinHook chains correctly ONLY if the second create happens after
// the first patch is already live, which nothing serializes.
//
// A factory vtable slot is a different layer, so it cannot collide with a code
// patch in either load order. All DXGI factory instances of dxgi.dll's factory
// class share this vtable, so replacing the slot intercepts the game's factory
// too, and the previous slot value stays our immediate-next:
//
//   game -> our detour -> dxgi!CreateSwapChainForHwnd (QuestPath's JMP, if it
//           patched) -> QuestPath's detour -> its trampoline -> real function
//
// Both mods observe every creation, whichever loaded first. A single aligned
// pointer store is atomic on x64, so a concurrent caller reads either the old
// or the new slot -- never a torn pointer.
bool install_factory_slot(void** vtable, size_t index, void* detour,
                          void** next_out) noexcept {
    if (!vtable || !detour || !next_out) return false;
    void** slot = vtable + index;
    DWORD previous_protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE,
                        &previous_protection))
        return false;
    // Capture whatever is there now -- possibly another tool's detour, which
    // must keep running as our immediate-next.
    *next_out = *slot;
    *slot = detour;
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), previous_protection, &restored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return *next_out != nullptr;
}

bool is_elden_ring_process() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const wchar_t* name = path;
    for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') name = cursor + 1;
    }
    return lstrcmpiW(name, L"eldenring.exe") == 0;
}

// -------------------------------------------------------------------------
// Late adoption of an already-existing swapchain
//
// The creation detours are the authoritative discovery and queue-association
// path, but they can only ever observe creations that happen after they are
// installed. When this mod is loaded late the game's swapchain (and its
// direct command queue) already exist. Adoption recovers from that with the
// backend's evidence rules intact:
//
//  * The swapchain INSTANCE is found by pass-through observer hooks on the
//    shared IDXGISwapChain::Present/Present1 implementations. They perform no
//    rendering and are never removed once installed (an unknown multi-mod
//    chain must not lose an edge); after adoption resolves they are a single
//    atomic load and a tail call.
//  * The exact queue is first requested from DXGI itself
//    (GetDevice(ID3D12CommandQueue)); measured on Windows 11 23H2 this
//    returns E_NOINTERFACE, but the call is exact if any DXGI build honors
//    it, so it stays as the first choice.
//  * Otherwise an ELDEN-RING-SPECIFIC correlation adapter may promote the
//    device's single active direct queue: presentation requires a direct
//    queue on the swapchain's device, so when exactly one direct queue has
//    ever submitted on that device, the association queue can only be that
//    queue or a queue that never executes -- and a presenting game renders
//    on its association queue every frame. Any second direct queue, or a
//    same-thread submit disagreeing with the candidate, permanently fails
//    adoption closed. Generic (non-Elden-Ring) embeddings of this backend
//    never promote correlation evidence and simply stay pass-through.
//
// Color for an adopted chain cannot use the creation-default rule (the game
// may have called SetColorSpace1 before this mod loaded), so arming issues
// one direct NVAPI GET to seed the existing vendor color observations. On
// systems without NVAPI, R10/FP16 adopted chains keep failing closed exactly
// like a detected wrapper does today.

constexpr uint32_t kAdoptionMinimumQueueCalls = 128;
constexpr uint32_t kAdoptionMinimumPresents = 32;
constexpr uint32_t kAdoptionMaximumInstallAttempts = 64;

void describe_code(const void* p, char* out, size_t cap) noexcept;  // "module+0xRVA"

struct AdoptionQueueEvidence {
    // queue_identity doubles as the slot-published flag: it is stored with
    // release order only after every other field is complete.
    std::atomic<void*> queue_identity{nullptr};
    void* device_identity = nullptr;
    ID3D12CommandQueue* queue = nullptr; // AddRef'd; retained for process life
    // The queue behind a Streamline proxy (streamline_native_queue) and its device; null when the
    // observed queue is not one of its proxies. Retained for process life like `queue`.
    ID3D12CommandQueue* native_queue = nullptr;
    void* native_device_identity = nullptr;
    // d3d12::device_reaches(this queue's device, reach_device) once per presenting device; read and
    // written only by try_adopt_swapchain under g_adoption_try_lock.
    void* reach_device = nullptr;
    const char* reach = nullptr;
    std::atomic<uint32_t> calls{0};
};
constexpr size_t kMaximumAdoptionQueues = 8;
AdoptionQueueEvidence g_adoption_queues[kMaximumAdoptionQueues];
std::atomic<uint32_t> g_adoption_queue_count{0};
std::atomic<bool> g_adoption_queue_overflow{false};
SRWLOCK g_adoption_queue_insert_lock = SRWLOCK_INIT;

// Per-thread submit correlation. The cached interface pointer avoids a
// QueryInterface per ExecuteCommandLists call on the hot pending path.
thread_local ID3D12CommandQueue* g_tls_ecl_cached_pointer = nullptr;
thread_local AdoptionQueueEvidence* g_tls_ecl_cached_slot = nullptr;
thread_local void* g_tls_last_direct_queue_identity = nullptr;
thread_local int g_tls_wrapper_ecl_depth = 0;
thread_local ID3D12CommandQueue* g_tls_native_cached_queue = nullptr;
thread_local bool g_tls_native_cached_direct = false;
thread_local ID3D12CommandQueue* g_tls_last_native_queue = nullptr;
thread_local bool g_tls_last_native_forwarded = false;

AdoptionState adoption_state() noexcept {
    return static_cast<AdoptionState>(
        g_adoption_state.load(std::memory_order_acquire));
}

void set_adoption_state(AdoptionState state) noexcept {
    g_adoption_state.store(static_cast<uint32_t>(state),
                           std::memory_order_release);
}

AdoptionQueueEvidence* record_adoption_queue(
    ID3D12CommandQueue* queue) noexcept {
    void* const identity = com_identity(queue);
    if (!identity) return nullptr;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return nullptr;

    const uint32_t published =
        g_adoption_queue_count.load(std::memory_order_acquire);
    for (uint32_t index = 0; index < published; ++index) {
        if (g_adoption_queues[index].queue_identity.load(
                std::memory_order_acquire) == identity)
            return &g_adoption_queues[index];
    }

    AcquireSRWLockExclusive(&g_adoption_queue_insert_lock);
    AdoptionQueueEvidence* result = nullptr;
    const uint32_t count =
        g_adoption_queue_count.load(std::memory_order_acquire);
    for (uint32_t index = 0; index < count && !result; ++index) {
        if (g_adoption_queues[index].queue_identity.load(
                std::memory_order_acquire) == identity)
            result = &g_adoption_queues[index];
    }
    if (!result) {
        if (count >= kMaximumAdoptionQueues) {
            g_adoption_queue_overflow.store(true, std::memory_order_release);
        } else {
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device) {
                AdoptionQueueEvidence& slot = g_adoption_queues[count];
                slot.device_identity = com_identity(device.Get());
                slot.queue = queue;
                queue->AddRef();
                if (ID3D12CommandQueue* native = streamline_native_queue(queue)) {
                    ComPtr<ID3D12Device> native_device;
                    if (SUCCEEDED(native->GetDevice(IID_PPV_ARGS(&native_device))) &&
                        native_device) {
                        slot.native_queue = native;  // see streamline_native: never Released
                        slot.native_device_identity = com_identity(native_device.Get());
                    }
                }
                slot.queue_identity.store(identity, std::memory_order_release);
                g_adoption_queue_count.store(count + 1,
                                             std::memory_order_release);
                result = &slot;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_adoption_queue_insert_lock);
    return result;
}

void STDMETHODCALLTYPE execute_command_lists_observer(
    ID3D12CommandQueue* queue, UINT count,
    ID3D12CommandList* const* lists) {
    if (adoption_state() == AdoptionState::Pending && queue) {
        if (queue != g_tls_ecl_cached_pointer) {
            g_tls_ecl_cached_pointer = queue;
            g_tls_ecl_cached_slot = record_adoption_queue(queue);
        }
        if (AdoptionQueueEvidence* slot = g_tls_ecl_cached_slot) {
            slot->calls.fetch_add(1, std::memory_order_relaxed);
            g_tls_last_direct_queue_identity =
                slot->queue_identity.load(std::memory_order_relaxed);
        }
    }
    // Everything the native ExecuteCommandLists sees while this is on the stack is the game's own
    // work forwarded by a wrapper queue (native_execute_command_lists_observer).
    ++g_tls_wrapper_ecl_depth;
    g_next_execute_command_lists(queue, count, lists);
    --g_tls_wrapper_ecl_depth;
}

// ── the native ExecuteCommandLists (only when a wrapper queue stands in front of it) ──────
// Per thread: the last native DIRECT queue submitted to, and whether that submission was the game's
// own work passing through a wrapper queue or a layer's direct submission. A frame-generation layer
// fills its present queue itself right before presenting the real swapchain on the same thread -
// that queue is what try_adopt_native binds (see the native-present section below).
// Native queues the game's own work was seen passing into through a wrapper queue. Never the
// queue of a real swapchain behind a frame-generation layer: binding one of these there is exactly
// the 2026-09-11 freeze (overlay work on the game's queue, unsynchronized with the present queue).
std::atomic<void*> g_forwarded_native_queues[4] = {};

void remember_forwarded_queue(void* queue) noexcept {
    for (auto& slot : g_forwarded_native_queues)
        if (slot.load(std::memory_order_relaxed) == queue) return;
    for (auto& slot : g_forwarded_native_queues) {
        void* expected = nullptr;
        if (slot.compare_exchange_strong(expected, queue, std::memory_order_relaxed)) return;
        if (expected == queue) return;
    }
}

bool is_forwarded_queue(void* queue) noexcept {
    for (auto& slot : g_forwarded_native_queues)
        if (slot.load(std::memory_order_relaxed) == queue) return true;
    return false;
}

void STDMETHODCALLTYPE native_execute_command_lists_observer(
    ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (adoption_state() == AdoptionState::Pending && queue) {
        if (queue != g_tls_native_cached_queue) {
            g_tls_native_cached_queue = queue;
            g_tls_native_cached_direct = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        }
        if (g_tls_native_cached_direct) {
            g_tls_last_native_queue = queue;
            g_tls_last_native_forwarded = g_tls_wrapper_ecl_depth > 0;
            if (g_tls_last_native_forwarded) remember_forwarded_queue(queue);
        }
    }
    g_next_native_execute_command_lists(queue, count, lists);
}

// Serializes candidate tracking and the adopt transaction. Contention is
// effectively zero (one game render thread); a losing Present skips a tick.
SRWLOCK g_adoption_try_lock = SRWLOCK_INIT;
struct AdoptionCandidate {
    IDXGISwapChain* instance = nullptr; // continuity key; only dereferenced
                                        // while it is the current caller
    uint32_t consecutive_presents = 0;
    uint64_t last_seen_ms = 0;
    uint32_t install_attempts = 0;
    bool seen_logged = false;
    bool waiting_logged = false;
} g_adoption_candidate;

// The proxy -> native pair from streamline_native_swapchain, asked once per proxy (under
// g_adoption_try_lock, like the candidate).
IDXGISwapChain* g_streamline_proxy = nullptr;
IDXGISwapChain* g_streamline_native = nullptr;

// Whether work on `slot`'s queue reaches `device` (the presenting swapchain's), cached per device.
const char* slot_reaches_locked(AdoptionQueueEvidence& slot, ID3D12Device* device,
                                void* device_identity) noexcept {
    if (slot.reach_device != device_identity) {
        slot.reach_device = device_identity;
        slot.reach = nullptr;
        ComPtr<ID3D12Device> queue_device;
        if (slot.queue && SUCCEEDED(slot.queue->GetDevice(IID_PPV_ARGS(&queue_device))) &&
            queue_device)
            slot.reach = d3d12::device_reaches(queue_device.Get(), device);
    }
    return slot.reach;
}

void adoption_failed_locked(const char* reason) noexcept {
    set_adoption_state(AdoptionState::Failed);
    flog("[overlay-v2] [WARN] swapchain adoption disabled: %s", reason);
}

void try_adopt_swapchain(IDXGISwapChain* swapchain) noexcept {
    if (!TryAcquireSRWLockExclusive(&g_adoption_try_lock)) return;
    try {
        do {
            if (adoption_state() != AdoptionState::Pending ||
                g_stopping.load(std::memory_order_acquire))
                break;
            // Behind a Streamline proxy there are TWO swapchains that could carry the overlay: the
            // proxy the game presents, and the native one it wraps (streamline_native_swapchain).
            // The presenting one comes first (under ERSS-FG its queue is ERSS's wrapper, which
            // reaches the proxy's device - see device_reaches); the native one is only a fallback
            // for a layer whose queue Streamline itself unwraps. The observer sits on the proxy's
            // Present, so only the proxy is ever mapped.
            if (swapchain != g_streamline_proxy) {
                g_streamline_proxy = swapchain;
                g_streamline_native = streamline_native_swapchain(swapchain);
                if (g_streamline_native)
                    flog("[overlay-v2] adoption: the presenting swapchain is a Streamline proxy; "
                         "the native swapchain behind it is a second candidate");
            }
            IDXGISwapChain* const native_swapchain = g_streamline_native;
            // Chains shadowed by the creation path (or a prior adoption) need
            // nothing further; this also makes the shadow's own downstream
            // Present call a no-op here. The exception is a generation whose
            // eligibility was withdrawn for a reason that published NOTHING on
            // the object (unreadable vtable, a lost publish race, a failed
            // allocation): the claims left behind by such an attempt are
            // skipped by the installer, so arming again is safe and is the only
            // way the overlay ever comes back - a game keeps one swapchain for
            // the whole session, so without this a single transient failure at
            // startup used to cost the overlay until the process exited.
            const bool retry_transient = dxgi_shadow::renderer_retryable(swapchain);
            if (dxgi_shadow::generation_token(swapchain) != 0 && !retry_transient)
                break;
            if (retry_transient)
                flog("[overlay-v2] re-arming a swapchain whose shadow failed transiently (%s)",
                     dxgi_shadow::ineligibility_reason(swapchain));
            if (!eligible_shadow_candidate(swapchain)) break;

            AdoptionCandidate& candidate = g_adoption_candidate;
            const uint64_t now = now_ticks();
            if (candidate.instance == swapchain &&
                now - candidate.last_seen_ms <= 500) {
                candidate.consecutive_presents =
                    std::min(candidate.consecutive_presents + 1, 1000u);
            } else {
                candidate.instance = swapchain;
                candidate.consecutive_presents = 1;
                candidate.install_attempts = 0;
            }
            candidate.last_seen_ms = now;
            if (!candidate.seen_logged && candidate.consecutive_presents >= 8) {
                candidate.seen_logged = true;
                flog("[overlay-v2] adoption: found a live game swapchain; "
                     "gathering exact-queue evidence");
            }
            if (candidate.consecutive_presents < kAdoptionMinimumPresents)
                break;

            // First choice: exact retrieval from DXGI. E_NOINTERFACE on every
            // Windows build measured so far, but exact if it ever succeeds.
            ComPtr<ID3D12CommandQueue> queue;
            if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&queue))))
                queue.Reset();
            if (queue) {
                // What the swapchain handed back, against the queues the game was seen submitting
                // on: the same object, or one the observer never saw (then nothing proves the
                // game's frames and this queue are ordered).
                void* const recovered = com_identity(queue.Get());
                bool seen = false;
                const uint32_t published = g_adoption_queue_count.load(std::memory_order_acquire);
                for (uint32_t index = 0; index < published; ++index)
                    seen = seen || g_adoption_queues[index].queue_identity.load(
                                       std::memory_order_acquire) == recovered;
                void** const vtable = *reinterpret_cast<void***>(queue.Get());
                char ecl[MAX_PATH + 32];
                describe_code(vtable ? vtable[10] : nullptr, ecl, sizeof ecl);
                flog("[overlay-v2] adoption: the swapchain returned its queue %p "
                     "(ExecuteCommandLists %s); %s the %u queue(s) the game was seen submitting on",
                     recovered, ecl, seen ? "one of" : "NOT one of", published);
            }
            bool correlated = false;
            IDXGISwapChain* target = swapchain;  // the one the shadow and the binding go on
            if (!queue) {
                if (!is_elden_ring_process()) {
                    adoption_failed_locked(
                        "DXGI does not expose the creation queue and the "
                        "single-queue correlation adapter is Elden Ring "
                        "specific");
                    break;
                }
                if (g_adoption_queue_overflow.load(std::memory_order_acquire)) {
                    adoption_failed_locked(
                        "too many distinct direct command queues to reason "
                        "about");
                    break;
                }
                ComPtr<ID3D12Device> device;
                if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device))) ||
                    !device)
                    break;
                void* const device_identity = com_identity(device.Get());
                void* native_swapchain_device = nullptr;
                if (native_swapchain) {
                    ComPtr<ID3D12Device> native_device;
                    if (SUCCEEDED(native_swapchain->GetDevice(IID_PPV_ARGS(&native_device))) &&
                        native_device)
                        native_swapchain_device = com_identity(native_device.Get());
                }
                AdoptionQueueEvidence* match = nullptr;
                bool match_native_queue = false;      // bind the queue behind the observed one
                IDXGISwapChain* match_target = nullptr;
                bool ambiguous = false;
                const uint32_t published =
                    g_adoption_queue_count.load(std::memory_order_acquire);
                for (uint32_t index = 0; index < published; ++index) {
                    AdoptionQueueEvidence& slot = g_adoption_queues[index];
                    if (!slot.queue_identity.load(std::memory_order_acquire))
                        continue;
                    // The pairs a queue can make, first match wins: the presenting swapchain with
                    // the observed queue (its device the swapchain's, or a wrapper over it); the
                    // native swapchain with the observed queue; the native swapchain with the
                    // queue Streamline hides behind the observed one.
                    IDXGISwapChain* pair_target = nullptr;
                    bool pair_native_queue = false;
                    if (slot_reaches_locked(slot, device.Get(), device_identity)) {
                        pair_target = swapchain;
                    } else if (native_swapchain_device &&
                               slot.device_identity == native_swapchain_device) {
                        pair_target = native_swapchain;
                    } else if (native_swapchain_device && slot.native_queue &&
                               slot.native_device_identity == native_swapchain_device) {
                        pair_target = native_swapchain;
                        pair_native_queue = true;
                    }
                    if (!pair_target)
                        continue;
                    if (match) {
                        ambiguous = true;
                        break;
                    }
                    match = &slot;
                    match_target = pair_target;
                    match_native_queue = pair_native_queue;
                }
                if (ambiguous) {
                    adoption_failed_locked(
                        "multiple direct command queues are active on the "
                        "game device; the overlay cannot prove which one "
                        "presents");
                    break;
                }
                if (!match ||
                    match->calls.load(std::memory_order_relaxed) <
                        kAdoptionMinimumQueueCalls) {
                    if (!candidate.waiting_logged) {
                        candidate.waiting_logged = true;
                        flog("[overlay-v2] adoption: waiting for sustained "
                             "single-queue submit evidence");
                        // What the wait is made of, once: every direct queue seen so far with its
                        // device and submit count, against the swapchain's device (and the native
                        // one behind a Streamline proxy). A queue on "another" device is the
                        // proxy-vs-native mismatch; a matching one below the count is only early.
                        flog("[overlay-v2] adoption evidence: presenting swapchain on device %p, "
                             "native swapchain behind it on device %p, %u direct queue(s) "
                             "observed%s",
                             device_identity, native_swapchain_device, published,
                             g_adoption_queue_overflow.load(std::memory_order_relaxed)
                                 ? ", overflowed" : "");
                        for (uint32_t index = 0; index < published; ++index) {
                            AdoptionQueueEvidence& slot = g_adoption_queues[index];
                            flog("[overlay-v2]   queue %p on device %p: %u submit(s); reaches "
                                 "the presenting device: %s; behind it: %s%p on device %p",
                                 slot.queue_identity.load(std::memory_order_relaxed),
                                 slot.device_identity,
                                 slot.calls.load(std::memory_order_relaxed),
                                 slot.reach ? slot.reach : "no",
                                 slot.native_queue ? "native queue " : "no Streamline native ",
                                 static_cast<void*>(slot.native_queue),
                                 slot.native_device_identity);
                        }
                    }
                    break;
                }
                // The presenting thread's own last direct submit must not
                // contradict the single-queue conclusion.
                if (g_tls_last_direct_queue_identity &&
                    g_tls_last_direct_queue_identity !=
                        match->queue_identity.load(std::memory_order_acquire)) {
                    adoption_failed_locked(
                        "the presenting thread submits on a different direct "
                        "queue than the only observed one");
                    break;
                }
                // A match through the native queue binds THAT queue: the renderer submits on the
                // native swapchain's own device.
                queue = match_native_queue ? match->native_queue : match->queue;
                target = match_target;
                correlated = true;
                if (target != swapchain) {
                    if (!eligible_shadow_candidate(target)) break;
                    flog("[overlay-v2] adoption: the observed queue pairs with the native "
                         "swapchain behind the Streamline proxy");
                } else if (match->reach && std::strcmp(match->reach, "same device") != 0) {
                    // Safe only because this swapchain's Present was called by the game's own
                    // code (observe_present): its buffers are the ones the game renders into
                    // through this very queue. The 2026-09-11 freeze was a wrapper pairing on a
                    // swapchain the game never presented.
                    flog("[overlay-v2] adoption: the game's queue sits on a wrapper device that "
                         "reaches the swapchain's device (%s)", match->reach);
                }
            }
            if (!queue) break;

            BindingScope binding_scope(g_creation_transaction);
            if (!install_swapchain_shadow(target)) {
                if (++candidate.install_attempts >=
                    kAdoptionMaximumInstallAttempts)
                    adoption_failed_locked(
                        "the live swapchain repeatedly refused a vtable "
                        "shadow");
                break;
            }
            if (!bind_swapchain_queue(target, queue.Get(),
                                      binding_scope.id(),
                                      binding_scope.depth(),
                                      /*game_presented=*/target == swapchain)) {
                adoption_failed_locked(
                    "the recovered queue failed exact-identity validation");
                break;
            }
            set_adoption_state(AdoptionState::Adopted);
            flog("[overlay-v2] adopted the live game swapchain (queue "
                 "evidence: %s)",
                 correlated ? "single active direct queue, Elden Ring adapter"
                            : "recovered from the swapchain itself");
        } while (false);
    } catch (...) {
        cte::note_swallowed(__FILE__, __LINE__);
        // Adoption is strictly optional; never let it disturb the host call.
    }
    ReleaseSRWLockExclusive(&g_adoption_try_lock);
}

// ── adoption at DXGI's own Present (frame-generation layers) ────────────────────────────
// Measured 2026-09-11 under ERSS-FG (FSR frame generation): drawing into the swapchain the game
// presents submitted 158 of 158 overlay frames and none reached the screen - ERSS-FG builds the
// frame from command lists it recognises in its ExecuteCommandLists wrapper and hands it to
// amd_fidelityfx_framegeneration, whose present queue writes the REAL swapchain and presents it.
// What is on screen is only ever final at that innermost Present, which is where overlays that work
// under frame generation draw. The earlier freeze drew into that same real swapchain, but through
// the GAME's queue, unsynchronized with the present queue that owns its buffers.
// So here the queue is the one the presenting thread itself submitted to last before this Present,
// provided that submission was not the game's work passing through a wrapper queue, its device is
// the swapchain's, and the pairing holds for kAdoptionMinimumPresents presents in a row: a layer's
// present queue, filled right before the Present it feeds, so the overlay lands after its copy on
// the same queue. Anything else keeps the wait counting; after kNativeAdoptionPatience presents
// without such a queue, adoption falls back to the swapchain the game calls (observe_present).
struct NativeCandidate {
    IDXGISwapChain* swapchain = nullptr;  // continuity keys only
    ID3D12CommandQueue* queue = nullptr;
    uint32_t presents = 0;
    uint32_t install_attempts = 0;
    bool seen_logged = false;
} g_native_candidate;

void try_adopt_native(IDXGISwapChain* swapchain) noexcept {
    ID3D12CommandQueue* queue = g_tls_last_native_queue;
    bool forwarded = g_tls_last_native_forwarded || (queue && is_forwarded_queue(queue));
    if (!TryAcquireSRWLockExclusive(&g_adoption_try_lock)) return;
    try {
        do {
            if (adoption_state() != AdoptionState::Pending ||
                g_stopping.load(std::memory_order_acquire))
                break;
            if (dxgi_shadow::generation_token(swapchain) != 0 &&
                !dxgi_shadow::renderer_retryable(swapchain))
                break;
            // Exact first: a DXGI that hands out its creation queue settles it outright.
            ComPtr<ID3D12CommandQueue> exact;
            if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&exact))) && exact) {
                static bool s_exact_logged = false;
                if (!s_exact_logged) {
                    s_exact_logged = true;
                    flog("[overlay-v2] native present: DXGI returned the swapchain's own queue %p",
                         static_cast<void*>(exact.Get()));
                }
                queue = exact.Get();
                forwarded = false;
            }
            NativeCandidate& candidate = g_native_candidate;
            static uint32_t s_transitions_logged = 0;
            const bool transition = candidate.swapchain != swapchain || candidate.queue != queue ||
                                    (candidate.presents == 0) != (!queue || forwarded);
            if (transition && s_transitions_logged < 12) {
                ++s_transitions_logged;
                flog("[overlay-v2] native present: swapchain %p, presenting thread's last direct "
                     "queue %p%s (thread %lu)", static_cast<void*>(swapchain),
                     static_cast<void*>(queue),
                     !queue ? " (none)" : forwarded ? " (the game's work via a wrapper)" : "",
                     GetCurrentThreadId());
            }
            if (!queue || forwarded) {
                candidate.presents = 0;
                g_native_presents_without_queue.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (candidate.swapchain != swapchain || candidate.queue != queue) {
                candidate.swapchain = swapchain;
                candidate.queue = queue;
                candidate.presents = 1;
                break;
            }
            if (++candidate.presents < kAdoptionMinimumPresents) break;

            ComPtr<ID3D12Device> swap_device;
            ComPtr<ID3D12Device> queue_device;
            if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&swap_device))) || !swap_device ||
                FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || !queue_device ||
                com_identity(swap_device.Get()) != com_identity(queue_device.Get())) {
                candidate = {};
                g_native_presents_without_queue.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (!eligible_shadow_candidate(swapchain)) {
                g_native_presents_without_queue.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (!candidate.seen_logged) {
                candidate.seen_logged = true;
                flog("[overlay-v2] adoption: DXGI's own Present is fed by a layer's direct queue "
                     "%p on the swapchain's device for %u presents in a row; adopting the real "
                     "swapchain there", static_cast<void*>(queue), candidate.presents);
            }
            BindingScope binding_scope(g_creation_transaction);
            if (!install_swapchain_shadow(swapchain)) {
                if (++candidate.install_attempts >= kAdoptionMaximumInstallAttempts)
                    adoption_failed_locked("the real swapchain repeatedly refused a vtable shadow");
                break;
            }
            if (!bind_swapchain_queue(swapchain, queue, binding_scope.id(),
                                      binding_scope.depth(), /*game_presented=*/true)) {
                adoption_failed_locked("the present queue failed exact-identity validation");
                break;
            }
            set_adoption_state(AdoptionState::Adopted);
            flog("[overlay-v2] adopted the real swapchain at DXGI's own Present (queue evidence: "
                 "the layer's present queue, filled on the presenting thread)");
        } while (false);
    } catch (...) {
        cte::note_swallowed(__FILE__, __LINE__);
    }
    ReleaseSRWLockExclusive(&g_adoption_try_lock);
}

// ── only the game's own Present is adopted ─────────────────────────────────────────────
// A swapchain is the game's only if the game's code itself calls its Present. Measured
// 2026-09-11 under ERSS-FG with FSR frame generation: the game calls Present on ERSS-FG's
// swapchain object (eldenring.exe `call [rax+40h]` -> ERSS-FG), which hands the frame to
// amd_fidelityfx_framegeneration, which presents the real DXGI swapchain; the Streamline proxy
// the probe resolves is a separate object below all that. Adopting that proxy drew into buffers the
// frame-generation presenter owns, unsynchronized, and froze the picture on the first overlay frame.
// So an observed Present counts only when its return address lies in the game executable; any
// other caller is a layer between the game and DXGI, and its stack is used to FIND the
// implementation the game calls (discover_game_present), which the control thread then observes
// too. Without layers the game calls DXGI (or a single proxy) directly and nothing changes.

struct ModuleRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
    bool contains(const void* p) const noexcept {
        const auto a = reinterpret_cast<uintptr_t>(p);
        return a >= begin && a < end;
    }
};

ModuleRange module_range(HMODULE module) noexcept {
    ModuleRange range;
    if (!module) return range;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return range;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return range;
    range.begin = reinterpret_cast<uintptr_t>(base);
    range.end = range.begin + nt->OptionalHeader.SizeOfImage;
    return range;
}

bool in_game_executable(const void* p) noexcept {
    static const ModuleRange range = module_range(GetModuleHandleW(nullptr));
    return range.contains(p);
}

bool in_this_module(const void* p) noexcept {
    static const ModuleRange range = module_range(g_hinst);
    return range.contains(p);
}

// Which swapchain method the game's code called, read off the instruction in front of its return
// address: 8 for `call [reg+40h]` (IDXGISwapChain::Present), 22 for `call [reg+0B0h]`
// (IDXGISwapChain1::Present1), 0 for anything else (the same layers also carry e.g.
// GetContainingOutput calls down to DXGI).
int game_call_slot(const uint8_t* ret) noexcept {
    if (ret[-3] == 0xFF && (ret[-2] & 0xF8) == 0x50 && ret[-2] != 0x54 && ret[-1] == 0x40)
        return 8;
    if (ret[-6] == 0xFF && (ret[-5] & 0xF8) == 0x90 && ret[-5] != 0x94 && ret[-4] == 0xB0 &&
        ret[-3] == 0 && ret[-2] == 0 && ret[-1] == 0)
        return 22;
    return 0;
}

// Entry point of the function containing `inside`, from the unwind data its module registered;
// follows chained entries (split function bodies) to the primary one.
void* function_entry(const void* inside) noexcept {
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry =
        RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(inside), &image_base, nullptr);
    for (int depth = 0; entry && depth < 8; ++depth) {
        if (entry->UnwindData & 1u) {  // indirect: points at another RUNTIME_FUNCTION
            entry = reinterpret_cast<PRUNTIME_FUNCTION>(image_base + (entry->UnwindData & ~1u));
            continue;
        }
        const auto* info = reinterpret_cast<const uint8_t*>(image_base + entry->UnwindData);
        constexpr uint8_t kChainInfo = 0x4;
        if (((info[0] >> 3) & kChainInfo) == 0)
            return reinterpret_cast<void*>(image_base + entry->BeginAddress);
        const size_t codes = (static_cast<size_t>(info[2]) + 1) & ~size_t{1};
        entry = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(info + 4 + codes * 2));
    }
    return nullptr;
}

void describe_code(const void* p, char* out, size_t cap) noexcept {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCSTR>(p), &mod) && mod) {
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        snprintf(out, cap, "%s+0x%llX", base ? base + 1 : name,
                 static_cast<unsigned long long>(static_cast<const char*>(p) -
                                                 reinterpret_cast<const char*>(mod)));
    } else {
        snprintf(out, cap, "%p", p);
    }
}

// Called for a Present that some layer (not the game) made, on that Present's thread. Walks the
// stack to the first frame in the game executable; when the instruction there is a Present call,
// the frame above it is inside the implementation the game called, and that implementation is
// published for the control thread to observe. Bounded: every 16th such Present, 256 walks at most.
void discover_game_present() noexcept {
    if (g_game_present_discovery.load(std::memory_order_acquire) != 0) return;
    const uint32_t seen = g_foreign_presents.fetch_add(1, std::memory_order_relaxed);
    if (seen % 16 != 0 || seen > 16u * 256u) return;
    void* frames[62] = {};
    const USHORT count = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 1; i < count; ++i) {
        if (!in_game_executable(frames[i])) continue;
        const int slot = game_call_slot(static_cast<const uint8_t*>(frames[i]));
        const void* callee_frame = frames[i - 1];
        if (!slot || in_game_executable(callee_frame) || in_this_module(callee_frame)) return;
        void* const entry = function_entry(callee_frame);
        if (!entry) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                char callee[MAX_PATH + 32];
                describe_code(callee_frame, callee, sizeof callee);
                flog("[overlay-v2] [WARN] adoption: the game's Present runs through %s, which has "
                     "no unwind entry; that layer cannot be observed", callee);
            }
            return;
        }
        for (void* known : g_observed_present_targets)
            if (known == entry) return;
        const uint64_t packed =
            reinterpret_cast<uint64_t>(entry) | (static_cast<uint64_t>(slot) << 56);
        uint64_t expected = 0;
        if (g_game_present_discovery.compare_exchange_strong(expected, packed,
                                                             std::memory_order_acq_rel)) {
            char callee[MAX_PATH + 32], site[MAX_PATH + 32];
            describe_code(entry, callee, sizeof callee);
            describe_code(frames[i], site, sizeof site);
            flog("[overlay-v2] adoption: the game calls %s through a layer at %s (return to %s); "
                 "observing it", slot == 8 ? "Present" : "Present1", callee, site);
        }
        return;
    }
}

// `native`: the call came through DXGI's own Present implementation (behind a Streamline proxy).
void observe_present(IDXGISwapChain* swapchain, UINT flags, const void* caller,
                     bool native = false) noexcept {
    if (adoption_state() != AdoptionState::Pending || !swapchain ||
        (flags & DXGI_PRESENT_TEST) != 0)
        return;
    const bool native_first =
        g_native_adoption &&
        g_native_presents_without_queue.load(std::memory_order_relaxed) < kNativeAdoptionPatience;
    if (native && g_native_adoption) {
        try_adopt_native(swapchain);
        if (native_first) return;
    }
    if (native_first) return;  // the game-called swapchain waits while the native search runs
    static std::atomic<bool> fallback_logged{false};
    if (g_native_adoption && !fallback_logged.exchange(true))
        flog("[overlay-v2] adoption: no layer present queue after %u native presents; falling "
             "back to the swapchain the game calls", kNativeAdoptionPatience);
    if (in_game_executable(caller))
        try_adopt_swapchain(swapchain);
    else
        discover_game_present();
}

HRESULT STDMETHODCALLTYPE present_observer_detour(IDXGISwapChain* swapchain,
                                                  UINT sync_interval,
                                                  UINT flags) {
    observe_present(swapchain, flags, _ReturnAddress());
    return g_next_present_observed(swapchain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE present1_observer_detour(
    IDXGISwapChain1* swapchain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    observe_present(swapchain, flags, _ReturnAddress());
    return g_next_present1_observed(swapchain, sync_interval, flags,
                                    parameters);
}

HRESULT STDMETHODCALLTYPE native_present_observer_detour(IDXGISwapChain* swapchain,
                                                         UINT sync_interval, UINT flags) {
    observe_present(swapchain, flags, _ReturnAddress(), /*native=*/true);
    return g_next_native_present_observed(swapchain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE native_present1_observer_detour(
    IDXGISwapChain1* swapchain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    observe_present(swapchain, flags, _ReturnAddress(), /*native=*/true);
    return g_next_native_present1_observed(swapchain, sync_interval, flags, parameters);
}

HRESULT STDMETHODCALLTYPE game_present_observer_detour(IDXGISwapChain* swapchain,
                                                       UINT sync_interval, UINT flags) {
    observe_present(swapchain, flags, _ReturnAddress());
    return g_next_game_present_observed(swapchain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE game_present1_observer_detour(
    IDXGISwapChain1* swapchain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    observe_present(swapchain, flags, _ReturnAddress());
    return g_next_game_present1_observed(swapchain, sync_interval, flags, parameters);
}

// Resolve the shared Present/Present1/ExecuteCommandLists implementations
// from a throwaway device + hidden swapchain. Nothing is ever presented or
// submitted, and everything is released before the observer hooks install, so
// the probe can never be seen (or adopted) by the observers. The window is a
// 64x64 tool window, which additionally fails both valid_creation_window and
// the minimum-canvas gates.  Because this mod intercepts CreateSwapChainForHwnd
// through the factory vtable slot, the probe's own creation dispatches into our
// detour; g_adoption_probe_in_progress makes that a pass-through no-op.
// `native_present_target` / `native_present1_target` come back non-null only when the probe is a
// Streamline proxy: the implementations of the native swapchain behind it, which is what the
// layers under the game present in the end (and whose stacks lead discovery to the game's call).
// `native_execute_target` likewise: the ExecuteCommandLists of a queue made on the device underneath
// a wrapper device (found through a fence's owner, d3d12::device_reaches' test), when that differs.
bool resolve_adoption_targets(void** present_target, void** present1_target,
                              void** execute_target, void** native_present_target,
                              void** native_present1_target,
                              void** native_execute_target) noexcept {
    // The probe needs nothing but three implementation addresses, so it should not go through a
    // creation slot anyone intercepts. It first creates its throwaway swapchain FOR COMPOSITION
    // (factory slot 24): neither this backend nor the proxies it has met (ERSS-FG / Streamline,
    // report 21) sit on that slot - they take CreateSwapChain (10) and CreateSwapChainForHwnd
    // (15) - so the creation cannot re-enter anyone's detour, and the swapchain class, hence
    // Present / Present1, is DXGI's own. Measured 2026-09-11: going through slot 15 under ERSS-FG
    // looped, the report-21 guard refused the creation and late adoption was lost - no menu.
    // Wine answers E_NOTIMPL for composition swapchains; there (and anywhere else composition
    // fails) the HWND probe below remains, but only while no creation loop has been seen.
    g_adoption_probe_in_progress = true;
    HWND hwnd = nullptr;
    bool class_registered = false;
    bool ok = false;
    const wchar_t* const kProbeClass =
        coexist::kAdoptionProbeWindowClass;
    do {
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory)
            break;
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(&device))) || !device)
            break;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->CreateCommandQueue(&queue_desc,
                                              IID_PPV_ARGS(&queue))) || !queue)
            break;

        ComPtr<IDXGISwapChain1> probe;
        {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = 64;
            desc.Height = 64;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;           // required for composition
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            const HRESULT hr = factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr,
                                                                      &probe);
            if (FAILED(hr) || !probe) {
                probe.Reset();
                flog("[overlay-v2] adoption probe: composition swapchain unavailable (hr 0x%08lX)",
                     static_cast<unsigned long>(hr));
            }
        }
        if (probe) {
            void** const swapchain_vtable = *reinterpret_cast<void***>(probe.Get());
            void** const queue_vtable = *reinterpret_cast<void***>(queue.Get());
            if (!swapchain_vtable || !queue_vtable) break;
            *present_target = swapchain_vtable[8];    // IDXGISwapChain::Present
            *present1_target = swapchain_vtable[22];  // IDXGISwapChain1::Present1
            *execute_target = queue_vtable[10];       // ID3D12CommandQueue::ExecuteCommandLists
            ok = *present_target && *present1_target && *execute_target;
            if (IDXGISwapChain* native = streamline_native_swapchain(probe.Get())) {
                void** const native_vtable = *reinterpret_cast<void***>(native);
                if (native_vtable && native_vtable[8] != *present_target) {
                    *native_present_target = native_vtable[8];
                    *native_present1_target = native_vtable[22];
                }
            }
            {
                ComPtr<ID3D12Fence> fence;
                ComPtr<ID3D12Device> owner;
                ComPtr<ID3D12CommandQueue> native_queue;
                if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
                    fence && SUCCEEDED(fence->GetDevice(IID_PPV_ARGS(&owner))) && owner &&
                    com_identity(owner.Get()) != com_identity(device.Get()) &&
                    SUCCEEDED(owner->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&native_queue))) &&
                    native_queue) {
                    void** const native_queue_vtable = *reinterpret_cast<void***>(native_queue.Get());
                    if (native_queue_vtable && native_queue_vtable[10] != *execute_target)
                        *native_execute_target = native_queue_vtable[10];
                }
            }
            // Where they landed, so a log shows whether the game's own Present can pass through them
            // (both swapchain kinds are expected to share DXGI's one implementation).
            char a[MAX_PATH + 32], b[MAX_PATH + 32], c[MAX_PATH + 32], d[MAX_PATH + 32],
                e[MAX_PATH + 32];
            describe_code(*present_target, a, sizeof a);
            describe_code(*present1_target, b, sizeof b);
            describe_code(*execute_target, c, sizeof c);
            describe_code(*native_present_target, d, sizeof d);
            describe_code(*native_execute_target, e, sizeof e);
            flog("[overlay-v2] adoption probe: implementations resolved from a composition "
                 "swapchain (no intercepted creation slot involved): Present %s, Present1 %s, "
                 "ExecuteCommandLists %s; native Present behind it %s, native "
                 "ExecuteCommandLists behind it %s", a, b, c,
                 *native_present_target ? d : "(none)", *native_execute_target ? e : "(none)");
            break;
        }

        // HWND probe: the one route through an intercepted slot. Once a creation loop has been seen
        // it would only feed that loop again (report 21's stack bottom was arm_adoption -> here).
        if (g_creation_loop_seen.load(std::memory_order_relaxed)) {
            flog("[overlay-v2] [WARN] skipping the HWND adoption probe: creation re-entry was "
                 "already observed, so another overlay owns that path");
            break;
        }

        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        // The class belongs to this DLL, not the executable. Its per-mod name
        // prevents two delayed backend copies from racing on registration
        // before either one reaches the shared hook-install mutex.
        window_class.hInstance = g_hinst;
        window_class.lpszClassName = kProbeClass;
        class_registered = RegisterClassW(&window_class) != 0;
        if (!class_registered) {
            flog("[overlay-v2] [ERROR] adoption probe class registration "
                 "failed (err %lu)", GetLastError());
            break;
        }
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kProbeClass, L"",
                               WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
                               window_class.hInstance, nullptr);
        if (!hwnd) break;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 64;
        desc.Height = 64;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc,
                                                   nullptr, nullptr,
                                                   &probe)) || !probe)
            break;

        void** const swapchain_vtable =
            *reinterpret_cast<void***>(probe.Get());
        void** const queue_vtable = *reinterpret_cast<void***>(queue.Get());
        if (!swapchain_vtable || !queue_vtable) break;
        *present_target = swapchain_vtable[8];    // IDXGISwapChain::Present
        *present1_target = swapchain_vtable[22];  // IDXGISwapChain1::Present1
        *execute_target = queue_vtable[10]; // ID3D12CommandQueue::ExecuteCommandLists

        // Field diagnostic: whether this DXGI build supports exact queue
        // retrieval. Windows 11 23H2 measures E_NOINTERFACE.
        ComPtr<ID3D12CommandQueue> retrieved;
        flog("[overlay-v2] adoption probe: swapchain->GetDevice("
             "ID3D12CommandQueue) is %s on this system",
             SUCCEEDED(probe->GetDevice(IID_PPV_ARGS(&retrieved)))
                 ? "supported" : "unavailable");
        ok = *present_target && *present1_target && *execute_target;
    } while (false);
    if (hwnd) DestroyWindow(hwnd);
    if (class_registered)
        UnregisterClassW(kProbeClass, g_hinst);
    g_adoption_probe_in_progress = false;
    return ok;
}

// One direct NVAPI GET so an adopted R10/FP16 chain can prove its current
// encoding; the game's own HDR SETs happened before this mod loaded. Calls
// nvapi64 directly (never through the game's hooked wrappers). Absence of
// NVAPI (AMD/Intel) is not an error: those chains keep failing closed.
void query_nvapi_current_hdr(HWND game_window) noexcept {
    constexpr uint32_t kNvapiIdInitialize = 0x0150E828u;
    constexpr uint32_t kNvapiIdGetDisplayIdByDisplayName = 0xAE457190u;
    constexpr uint32_t kNvapiIdDispHdrColorControl = 0x351DA224u;
    constexpr uint32_t kHdrColorDataV1 = 0x00010028u;

    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) {
        flog("[overlay-v2] adoption: nvapi64 is not loaded; HDR-capable "
             "swapchain encodings stay unverified");
        return;
    }
    using QueryInterfaceFn = void*(__cdecl*)(uint32_t);
    const auto query_interface = reinterpret_cast<QueryInterfaceFn>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!query_interface) return;
    using InitializeFn = int(__cdecl*)();
    using GetDisplayIdFn = int(__cdecl*)(const char*, uint32_t*);
    using HdrColorControlFn = int(__cdecl*)(uint32_t, void*);
    const auto initialize =
        reinterpret_cast<InitializeFn>(query_interface(kNvapiIdInitialize));
    const auto get_display_id = reinterpret_cast<GetDisplayIdFn>(
        query_interface(kNvapiIdGetDisplayIdByDisplayName));
    const auto hdr_color_control = reinterpret_cast<HdrColorControlFn>(
        query_interface(kNvapiIdDispHdrColorControl));
    if (!initialize || !get_display_id || !hdr_color_control) return;
    if (initialize() != 0) return; // refcounted; game holds it initialized

    const auto output_name = monitor_output_name(
        MonitorFromWindow(game_window, MONITOR_DEFAULTTOPRIMARY));
    if (output_name[0] == '\0') return;
    uint32_t display_id = 0;
    if (get_display_id(output_name.data(), &display_id) != 0) {
        flog("[overlay-v2] adoption: NvAPI display lookup failed for %s",
             output_name.data());
        return;
    }
    alignas(8) unsigned char color_data[40] = {};
    auto* header = reinterpret_cast<NvapiHdrColorDataHeader*>(color_data);
    header->version = kHdrColorDataV1;
    header->command = static_cast<int32_t>(NvapiHdrCommand::Get);
    if (hdr_color_control(display_id, header) != 0) {
        flog("[overlay-v2] adoption: NvAPI HDR state query failed; encoding "
             "stays unverified");
        return;
    }
    try {
        record_nvapi_display_binding(output_name, display_id);
        record_nvapi_color_observation(display_id, header->hdr_mode,
                                       "the overlay's direct query");
    } catch (...) {
        cte::note_swallowed(__FILE__, __LINE__);
        // Optional evidence only.
    }
}

void* find_nvapi_wrapper(const std::array<uint8_t, 13>& prefix,
                         const std::array<uint8_t, 12>& suffix,
                         size_t function_delta) noexcept {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return nullptr;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + static_cast<size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    constexpr size_t kSignatureSize = 29;
    constexpr size_t kWildcardOffset = 13;
    constexpr size_t kWildcardSize = 4;
    constexpr size_t kMaximumScanBytes = 0x20000;
    const size_t code_offset = nt->OptionalHeader.BaseOfCode;
    if (code_offset >= nt->OptionalHeader.SizeOfImage) return nullptr;
    const size_t scan_size = std::min<size_t>(
        kMaximumScanBytes, nt->OptionalHeader.SizeOfImage - code_offset);
    if (scan_size < kSignatureSize || function_delta > code_offset)
        return nullptr;

    const uint8_t* scan = base + code_offset;
    const uint8_t* unique = nullptr;
    for (size_t offset = 0; offset + kSignatureSize <= scan_size; ++offset) {
        const uint8_t* candidate = scan + offset;
        if (std::memcmp(candidate, prefix.data(), prefix.size()) != 0 ||
            std::memcmp(candidate + kWildcardOffset + kWildcardSize,
                        suffix.data(), suffix.size()) != 0)
            continue;
        const uint8_t* function = candidate - function_delta;
        if (function < base + code_offset) continue;
        if (unique && unique != function) return nullptr;
        unique = function;
    }
    return const_cast<uint8_t*>(unique);
}

void resolve_nvapi_hook_targets(HookTargets& targets) noexcept {
    if (!is_elden_ring_process()) return;
    // No nvapi64 in the process means the observer can never fire, so resolving these targets would
    // only buy two permanent code patches inside eldenring.exe for nothing. That is the normal state on
    // Linux/Proton and on every AMD/Intel machine, and a player report showed the patches going in
    // there anyway. HDR then stays unverified, which the code already handles by failing closed.
    if (!GetModuleHandleW(L"nvapi64.dll")) {
        flog("[overlay-v2] nvapi64 is not loaded; skipping the vendor HDR observer entirely");
        return;
    }

    // These are the stable lazy-resolution interiors emitted by the NVAPI
    // import library in the supported Elden Ring executable. Scanning only the
    // first 128 KiB keeps authoritative DXGI creation hooks on the cold path.
    // Wildcards cover the RIP-relative cache displacement; deriving the entry
    // from the interior remains compatible with an earlier prologue hook.
    constexpr std::array<uint8_t, 13> kDisplayPrefix = {
        0xB9, 0x90, 0x71, 0x45, 0xAE, 0xFF, 0xD0,
        0x48, 0x8B, 0xD8, 0x48, 0x89, 0x05};
    constexpr std::array<uint8_t, 12> kDisplaySuffix = {
        0x48, 0x85, 0xC0, 0x75, 0x07, 0xB8,
        0xFD, 0xFF, 0xFF, 0xFF, 0xEB, 0x48};
    constexpr std::array<uint8_t, 13> kHdrPrefix = {
        0xB9, 0x24, 0xA2, 0x1D, 0x35, 0xFF, 0xD0,
        0x48, 0x8B, 0xD8, 0x48, 0x89, 0x05};
    constexpr std::array<uint8_t, 12> kHdrSuffix = {
        0x48, 0x85, 0xC0, 0x75, 0x07, 0xB8,
        0xFD, 0xFF, 0xFF, 0xFF, 0xEB, 0x47};

    targets.nvapi_get_display_id_by_name =
        find_nvapi_wrapper(kDisplayPrefix, kDisplaySuffix, 0x44);
    targets.nvapi_hdr_color_control =
        find_nvapi_wrapper(kHdrPrefix, kHdrSuffix, 0x43);
    if (!targets.nvapi_get_display_id_by_name ||
        !targets.nvapi_hdr_color_control) {
        targets.nvapi_get_display_id_by_name = nullptr;
        targets.nvapi_hdr_color_control = nullptr;
        flog("[overlay-v2] [WARN] Elden Ring NVAPI HDR observer signature unavailable; vendor HDR remains unverified");
    }
}

// Read the two creation slots off a throwaway factory. All DXGI factories of a process share
// one vtable, so this reads the same slots the game's factory will dispatch through.
void capture_pristine_creation_slots() {
    if (g_pristine_create_swapchain && g_pristine_create_swapchain_for_hwnd)
        return; // first capture wins: a later one could already be somebody's proxy
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory)
        return;
    void** vtable = *reinterpret_cast<void***>(factory.Get());
    if (!vtable)
        return;
    g_pristine_create_swapchain = vtable[10];
    g_pristine_create_swapchain_for_hwnd = vtable[15];
}

bool resolve_hook_targets(HookTargets& targets) {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory)
        return false;

    // Only creation entrypoints are process-wide.  Per-frame interception is
    // installed on the returned swapchain instance, so bootstrap needs no
    // probe HWND, D3D device, command queue, or private swapchain.
    void** vtable = *reinterpret_cast<void***>(factory.Get());
    if (!vtable) return false;
    targets.factory_vtable = vtable;
    targets.create_swapchain = vtable[10];
    targets.create_swapchain_for_hwnd = vtable[15];
    resolve_nvapi_hook_targets(targets);
    return targets.create_swapchain && targets.create_swapchain_for_hwnd;
}

template <typename Fn>
bool queue_hook(void* target, void* detour, Fn& next, const char* name) {
    if (!target || !hooks::create(target, detour,
                                  reinterpret_cast<void**>(&next))) {
        flog("[overlay-v2] [ERROR] failed to create %s hook", name);
        return false;
    }
    return true;
}

bool install_all_hooks(const HookTargets& target) {
    bool ok = true;
    // The MinHook portion of this batch (the NVAPI observer, plus the DXGI
    // code-patch fallback below) is one create->apply transaction and MUST be
    // serialized against any other mod embedding this backend. An interleaved
    // install erases one side's jmp with every call still returning MH_OK --
    // see hooks.hpp. The vtable-slot interception is a different layer and
    // needs no lock, but taking it here covers the whole function uniformly.
    hooks::InstallLock install_lock;

    // Slot interception first (see install_factory_slot for why). Code patching
    // remains the fallback for the case where the vtable page cannot be made
    // writable, which is also the only configuration that can still lose a
    // create/apply race against another MinHook user.
    const bool slots_installed =
        target.factory_vtable &&
        install_factory_slot(target.factory_vtable, 10,
                             reinterpret_cast<void*>(&create_swapchain_detour),
                             reinterpret_cast<void**>(&g_next_create_swapchain)) &&
        install_factory_slot(target.factory_vtable, 15,
                             reinterpret_cast<void*>(&create_swapchain_for_hwnd_detour),
                             reinterpret_cast<void**>(&g_next_create_swapchain_for_hwnd));
    if (slots_installed) {
        flog("[overlay-v2] DXGI creation intercepted via factory vtable slots "
             "(coexists with code-patching overlays)");
        // Forensics inspects the slots we actually replaced, not the (still
        // pristine) code prologue -- see log_creation_hook_forensics.
        g_forensics_create_swapchain_slot = target.factory_vtable + 10;
        g_forensics_create_swapchain_for_hwnd_slot = target.factory_vtable + 15;
    } else {
        flog("[overlay-v2] [WARN] factory vtable interception unavailable; "
             "falling back to code patching");
        ok &= queue_hook(target.create_swapchain,
                         reinterpret_cast<void*>(&create_swapchain_detour),
                         g_next_create_swapchain, "CreateSwapChain");
        ok &= queue_hook(target.create_swapchain_for_hwnd,
                         reinterpret_cast<void*>(&create_swapchain_for_hwnd_detour),
                         g_next_create_swapchain_for_hwnd, "CreateSwapChainForHwnd");
        g_forensics_create_swapchain_target = target.create_swapchain;
        g_forensics_create_swapchain_for_hwnd_target =
            target.create_swapchain_for_hwnd;
    }

    bool nvapi_targets_present = false;
    bool nvapi_observer_ready = false;
    if (target.nvapi_get_display_id_by_name &&
        target.nvapi_hdr_color_control) {
        nvapi_targets_present = true;
        const bool display_hook_created = hooks::create(
            target.nvapi_get_display_id_by_name,
            reinterpret_cast<void*>(&nvapi_get_display_id_by_name_detour),
            reinterpret_cast<void**>(&g_next_nvapi_get_display_id_by_name));
        const bool hdr_hook_created = hooks::create(
            target.nvapi_hdr_color_control,
            reinterpret_cast<void*>(&nvapi_hdr_color_control_detour),
            reinterpret_cast<void**>(&g_next_nvapi_hdr_color_control));
        nvapi_observer_ready = display_hook_created && hdr_hook_created;
        // A partial optional pair may still be enabled by the transaction, but
        // both detours remain exact pass-through unless this gate is true.
        g_nvapi_observer_enabled.store(nvapi_observer_ready,
                                       std::memory_order_release);
    }

    const bool applied = hooks::apply();
    if (!ok || !applied) {
        // Some hooks may already be active (and queued successes could be
        // applied by another subsystem later).  Leave every callback safely
        // pass-through rather than attempting a chain-destructive rollback.
        g_nvapi_observer_enabled.store(false, std::memory_order_release);
        g_stopping.store(true, std::memory_order_release);
        return false;
    }

    if (nvapi_targets_present) {
        if (nvapi_observer_ready) {
            flog("[overlay-v2] NVIDIA NVAPI HDR observer installed");
        } else {
            flog("[overlay-v2] [WARN] NVIDIA NVAPI HDR observer unavailable; generic DXGI backend remains active");
        }
    }
    return true;
}

// -------------------------------------------------------------------------
// Creation-hook forensics

// SEH-guarded copy in a function with no unwindable objects. The inspected
// addresses are hooked code and normally readable; a peer unmapping its
// module is the case this guards against.
bool safe_read_bytes(const void* source, void* destination,
                     size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// MinHook does not always write a jump straight to the detour.  When the
// target and the detour are more than +-2 GB apart it writes the rel32 jump to
// a VirtualAlloc'd relay page, which then does `jmp qword ptr [rip+0]` to the
// real detour.  That page belongs to no module, so naively reporting the rel32
// destination accuses a peer mod of overwriting a hook that is in fact intact
// -- and which loader placed this DLL near dxgi.dll decides whether it
// happens at all.  Follow the chain a couple of hops before concluding
// anything.
const void* follow_jump_chain(const void* destination) noexcept {
    for (int hop = 0; hop < 4 && destination; ++hop) {
        uint8_t bytes[16]{};
        if (!safe_read_bytes(destination, bytes, sizeof(bytes))) break;
        const auto* code = static_cast<const uint8_t*>(destination);
        if (bytes[0] == 0xE9) {
            int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 1, sizeof(displacement));
            destination = code + 5 + displacement;
            continue;
        }
        if (bytes[0] == 0xFF && bytes[1] == 0x25) {
            int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 2, sizeof(displacement));
            const void* slot = code + 6 + displacement;
            void* next = nullptr;
            if (!safe_read_bytes(slot, &next, sizeof(next))) break;
            destination = next;
            continue;
        }
        break;
    }
    return destination;
}

void log_patch_owner(const char* name, const char* patch_kind,
                     const void* destination) noexcept {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH]{};
    const wchar_t* base_name = L"unmapped memory";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(destination), &module) &&
        module && GetModuleFileNameW(module, path, MAX_PATH)) {
        base_name = path;
        for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
            if (*cursor == L'\\' || *cursor == L'/') base_name = cursor + 1;
        }
    }
    flog("[overlay-v2] forensics: %s prologue is a %s into %ls at %p -- "
         "a mod that hooked after this one (legitimate if it chains back) "
         "or one that overwrote this mod's hook",
         name, patch_kind, base_name, destination);
}

// Code-patch mode (fallback): inspect the dxgi entrypoint's prologue.
void log_creation_target_forensics(const char* name, void* target,
                                   const void* detour) noexcept {
    if (!target) return;
    uint8_t bytes[16]{};
    if (!safe_read_bytes(target, bytes, sizeof(bytes))) {
        flog("[overlay-v2] forensics: %s target %p is unreadable", name,
             target);
        return;
    }
    const auto* code = static_cast<const uint8_t*>(target);
    if (bytes[0] == 0xE9) { // rel32 jmp -- what MinHook writes
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        const void* destination = code + 5 + displacement;
        if (destination != detour && follow_jump_chain(destination) == detour) {
            flog("[overlay-v2] forensics: %s patch is intact (through "
                 "MinHook's relay at %p) -- the chain still enters this "
                 "mod's detour",
                 name, destination);
            return;
        }
        if (destination == detour) {
            flog("[overlay-v2] forensics: %s patch is intact -- the jump "
                 "still enters this mod's detour",
                 name);
        } else {
            log_patch_owner(name, "relative jump", destination);
        }
        return;
    }
    if (bytes[0] == 0xFF && bytes[1] == 0x25) { // jmp [rip+disp32]
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 2, sizeof(displacement));
        const void* slot = code + 6 + displacement;
        void* destination = nullptr;
        if (safe_read_bytes(slot, &destination, sizeof(destination))) {
            if (destination == detour ||
                follow_jump_chain(destination) == detour)
                flog("[overlay-v2] forensics: %s patch is intact -- the "
                     "indirect jump still enters this mod's detour",
                     name);
            else
                log_patch_owner(name, "absolute indirect jump", destination);
        } else
            flog("[overlay-v2] forensics: %s prologue is an indirect jump "
                 "through unreadable memory", name);
        return;
    }
    flog("[overlay-v2] forensics: %s prologue bytes %02X %02X %02X %02X "
         "%02X are not a recognized detour patch -- this mod's hook was "
         "overwritten or removed",
         name, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
}

// Slot mode (default for this mod): inspect the factory vtable entry we
// replaced.  A slot that no longer points at our detour was taken by another
// tool -- legitimate if that tool captured our pointer as its immediate-next.
void log_creation_slot_forensics(const char* name, void** slot,
                                 const void* detour) noexcept {
    if (!slot) return;
    void* current = nullptr;
    if (!safe_read_bytes(slot, &current, sizeof(current))) {
        flog("[overlay-v2] forensics: %s vtable slot %p is unreadable", name,
             static_cast<void*>(slot));
        return;
    }
    if (current == detour) {
        flog("[overlay-v2] forensics: %s factory vtable slot still points at "
             "this mod's detour (intact)",
             name);
        return;
    }
    if (follow_jump_chain(current) == detour) {
        flog("[overlay-v2] forensics: %s factory vtable slot chains back to "
             "this mod's detour (intact)",
             name);
        return;
    }
    log_patch_owner(name, "factory vtable slot", current);
}

} // namespace

void capture_creation_entrypoints() {
    capture_pristine_creation_slots();
}

bool install_hooks() {
    if (g_installed.load(std::memory_order_acquire))
        return g_renderer_ready.load(std::memory_order_acquire);
    // Last-resort capture. The one that MATTERS happens far earlier, from the mod thread
    // (capture_creation_entrypoints): by the time install_hooks runs, another overlay may already
    // own these slots - in report 21 it hooked 11.8 s before this point. Capturing here anyway
    // costs nothing (first capture wins) and covers the case where nobody called us early.
    capture_pristine_creation_slots();
    if (!hooks::init()) {
        flog("[overlay-v2] [ERROR] MinHook initialization failed");
        return false;
    }

    // Pin the DLL.  A later injector can legitimately build a trampoline that
    // enters our detour; unloading our code from the middle of that chain is
    // not safely reversible without cooperation from every participant.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&install_hooks), &pinned) ||
        pinned != g_hinst) {
        flog("[overlay-v2] [ERROR] could not pin hook module; refusing interception");
        return false;
    }

    HookTargets targets;
    if (!resolve_hook_targets(targets)) {
        flog("[overlay-v2] [ERROR] DXGI/D3D12 hook target discovery failed");
        return false;
    }
    dxgi_shadow::Callbacks callbacks{};
    callbacks.before_present = &shadow_before_present;
    callbacks.after_present = &shadow_after_present;
    callbacks.before_resize = &shadow_before_resize;
    callbacks.after_resize = &shadow_after_resize;
    callbacks.before_color_space = &shadow_before_color_space;
    callbacks.after_color_space = &shadow_after_color_space;
    if (!dxgi_shadow::configure_callbacks(callbacks)) {
        flog("[overlay-v2] [ERROR] swapchain callback publication failed");
        return false;
    }
    if (!install_all_hooks(targets)) {
        dxgi_shadow::clear_callbacks();
        flog("[overlay-v2] [ERROR] hook transaction failed; backend unavailable");
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    flog("[overlay-v2] DXGI creation hooks installed; Present uses per-instance shadows");

    // Queue discovery is the only part that cannot be recovered after the
    // game creates its swapchain, so hooks go live before shader compilation.
    // Present callbacks remain pass-through until this worker finishes; this
    // avoids both a creation race and shader compilation on a Present thread.
    if (!d3d12::prepare_shaders()) {
        g_renderer_healthy.store(false, std::memory_order_release);
        flog("[overlay-v2] [ERROR] renderer shader preparation failed; interception remains pass-through");
        return false;
    }
    g_renderer_ready.store(true, std::memory_order_release);
    flog("[overlay-v2] renderer shaders prepared; swapchain rendering enabled");
    return true;
}

bool hooks_installed() {
    return g_installed.load(std::memory_order_acquire);
}

bool observed_swapchain_creation() {
    return g_observed_creation.load(std::memory_order_acquire);
}

void log_creation_hook_forensics() {
    if (!g_installed.load(std::memory_order_acquire)) {
        flog("[overlay-v2] forensics: creation hooks were never installed");
        return;
    }
    flog("[overlay-v2] forensics: this mod's DXGI creation detours have run "
         "%u time(s) since load",
         g_creation_detour_calls.load(std::memory_order_relaxed));
    if (g_forensics_create_swapchain_slot ||
        g_forensics_create_swapchain_for_hwnd_slot) {
        // Default path: interception is via factory vtable slots.
        log_creation_slot_forensics(
            "CreateSwapChain", g_forensics_create_swapchain_slot,
            reinterpret_cast<const void*>(&create_swapchain_detour));
        log_creation_slot_forensics(
            "CreateSwapChainForHwnd", g_forensics_create_swapchain_for_hwnd_slot,
            reinterpret_cast<const void*>(&create_swapchain_for_hwnd_detour));
    } else {
        // Fallback path: interception is a code patch on the dxgi entrypoint.
        log_creation_target_forensics(
            "CreateSwapChain", g_forensics_create_swapchain_target,
            reinterpret_cast<const void*>(&create_swapchain_detour));
        log_creation_target_forensics(
            "CreateSwapChainForHwnd",
            g_forensics_create_swapchain_for_hwnd_target,
            reinterpret_cast<const void*>(&create_swapchain_for_hwnd_detour));
    }
}

void arm_adoption(HWND game_window_hint) {
    // Overlay/control thread only (matches g_adoption_hooks_installed use).
    if (!g_installed.load(std::memory_order_acquire) ||
        !g_renderer_ready.load(std::memory_order_acquire) ||
        g_stopping.load(std::memory_order_acquire))
        return;
    if (adoption_state() == AdoptionState::Pending) return;

    if (g_adoption_hooks_installed) {
        // Re-arm: the observers are resident; only the search state resets.
        // Covers a replacement swapchain being missed while the creation hook
        // is dead (the same condition that required adoption initially).
        AcquireSRWLockExclusive(&g_adoption_try_lock);
        g_adoption_candidate = {};
        ReleaseSRWLockExclusive(&g_adoption_try_lock);
        set_adoption_state(AdoptionState::Pending);
        query_nvapi_current_hdr(game_window_hint); // HDR may have changed
        flog("[overlay-v2] swapchain adoption re-armed");
        return;
    }

    void* present_target = nullptr;
    void* present1_target = nullptr;
    void* execute_target = nullptr;
    void* native_present_target = nullptr;
    void* native_present1_target = nullptr;
    void* native_execute_target = nullptr;
    if (!resolve_adoption_targets(&present_target, &present1_target,
                                  &execute_target, &native_present_target,
                                  &native_present1_target, &native_execute_target)) {
        set_adoption_state(AdoptionState::Failed);
        flog("[overlay-v2] [ERROR] adoption probe could not resolve the "
             "Present/ExecuteCommandLists implementations; late adoption "
             "unavailable");
        return;
    }
    {
        // Same serialization contract as every other hook batch (hooks.hpp).
        hooks::InstallLock install_lock;
        bool ok = true;
        ok &= queue_hook(present_target,
                         reinterpret_cast<void*>(&present_observer_detour),
                         g_next_present_observed, "Present observer");
        ok &= queue_hook(present1_target,
                         reinterpret_cast<void*>(&present1_observer_detour),
                         g_next_present1_observed, "Present1 observer");
        ok &= queue_hook(
            execute_target,
            reinterpret_cast<void*>(&execute_command_lists_observer),
            g_next_execute_command_lists, "ExecuteCommandLists observer");
        if (native_present_target && native_present1_target) {
            ok &= queue_hook(native_present_target,
                             reinterpret_cast<void*>(&native_present_observer_detour),
                             g_next_native_present_observed, "native Present observer");
            ok &= queue_hook(native_present1_target,
                             reinterpret_cast<void*>(&native_present1_observer_detour),
                             g_next_native_present1_observed, "native Present1 observer");
            if (native_execute_target) {
                ok &= queue_hook(native_execute_target,
                                 reinterpret_cast<void*>(&native_execute_command_lists_observer),
                                 g_next_native_execute_command_lists,
                                 "native ExecuteCommandLists observer");
                g_native_adoption = true;
            }
        }
        g_observed_present_targets[0] = present_target;
        g_observed_present_targets[1] = present1_target;
        g_observed_present_targets[2] = native_present_target;
        g_observed_present_targets[3] = native_present1_target;
        ok &= hooks::apply();
        if (!ok) {
            // Partial installs stay resident as pass-through; adoption never
            // activates them, mirroring the creation-batch failure policy.
            set_adoption_state(AdoptionState::Failed);
            flog("[overlay-v2] [ERROR] adoption hook transaction failed; "
                 "late adoption unavailable");
            return;
        }
    }
    g_adoption_hooks_installed = true;
    set_adoption_state(AdoptionState::Pending);
    query_nvapi_current_hdr(game_window_hint);
    flog("[overlay-v2] swapchain adoption armed: watching live presents for "
         "the game swapchain");
}

void log_render_stats() {
    const uint32_t presents = g_diag_open_presents.exchange(0, std::memory_order_relaxed);
    const uint32_t submitted = g_diag_open_submitted.exchange(0, std::memory_order_relaxed);
    const uint32_t slot_changes = g_diag_slot_changes.exchange(0, std::memory_order_relaxed);
    const char* stage = g_diag_last_stage.load(std::memory_order_relaxed);
    flog("[overlay-v2] menu open: %u shadowed present(s), %u overlay frame(s) submitted, "
         "back-buffer index changed %u time(s) (now %u); last present stopped at: %s",
         presents, submitted, slot_changes,
         g_diag_last_slot.load(std::memory_order_relaxed), stage ? stage : "(none yet)");
}

void service_adoption() {
    // Overlay/control thread only. Installs, once, the observer on the Present implementation the
    // game's own code was found calling (discover_game_present publishes it from a Present thread;
    // hooks are never created from inside somebody else's Present).
    if (!g_adoption_hooks_installed || g_game_present_hooked ||
        g_stopping.load(std::memory_order_acquire))
        return;
    const uint64_t packed = g_game_present_discovery.load(std::memory_order_acquire);
    if (!packed) return;
    g_game_present_hooked = true; // one attempt: a failed install stays pass-through
    void* const target = reinterpret_cast<void*>(packed & 0x00FFFFFFFFFFFFFFull);
    const int slot = static_cast<int>(packed >> 56);
    bool ok = true;
    {
        hooks::InstallLock install_lock;
        if (slot == 8)
            ok &= queue_hook(target, reinterpret_cast<void*>(&game_present_observer_detour),
                             g_next_game_present_observed, "game Present observer");
        else
            ok &= queue_hook(target, reinterpret_cast<void*>(&game_present1_observer_detour),
                             g_next_game_present1_observed, "game Present1 observer");
        ok &= hooks::apply();
    }
    char where[MAX_PATH + 32];
    describe_code(target, where, sizeof where);
    if (ok)
        flog("[overlay-v2] adoption: now observing the game's own %s at %s",
             slot == 8 ? "Present" : "Present1", where);
    else
        flog("[overlay-v2] [ERROR] could not observe the game's own Present at %s; the overlay "
             "stays unavailable behind this layer", where);
}

Canvas canvas() {
    Canvas result;
    for (;;) {
        const uint64_t before = g_canvas.sequence.load(std::memory_order_acquire);
        if (before & 1u) continue;
        result.hwnd = reinterpret_cast<HWND>(
            g_canvas.hwnd.load(std::memory_order_relaxed));
        result.width = g_canvas.width.load(std::memory_order_relaxed);
        result.height = g_canvas.height.load(std::memory_order_relaxed);
        result.hdr = g_canvas.hdr.load(std::memory_order_relaxed);
        result.ready = g_canvas.ready.load(std::memory_order_relaxed);
        const uint64_t after = g_canvas.sequence.load(std::memory_order_acquire);
        if (before == after) return result;
    }
}

void set_visible(bool value) {
    if (value) {
        // A recoverable allocation/recording failure is sticky while the menu
        // remains open. A deliberate close/reopen is the only user-visible
        // retry boundary, so a bad frame cannot create a per-Present loop.
        const bool was_visible = g_visible.load(std::memory_order_acquire);
        if (!was_visible) g_packet_skip_warning_logged = false;
        if (!was_visible &&
            g_retry_on_next_open.exchange(false, std::memory_order_acq_rel))
            g_renderer_healthy.store(true, std::memory_order_release);
        g_visible.store(true, std::memory_order_release);
    } else {
        g_visible.store(false, std::memory_order_release);
        frame::clear_frame();
    }
}

bool visible() {
    return g_visible.load(std::memory_order_acquire);
}

void publish_draw_data(const ImDrawData* draw_data) {
    const uint64_t font_generation = g_font_generation.load(std::memory_order_acquire);
    const frame::frame_publish_result result = frame::publish_draw_data(
        draw_data, kFontTextureToken, font_generation);
    if (result.status == frame::publish_status::published_with_skips &&
        !g_packet_skip_warning_logged) {
        g_packet_skip_warning_logged = true;
        flog("[overlay-v2] [WARN] frame %llu skipped %u callbacks and %u textures",
             static_cast<unsigned long long>(result.generation),
             result.skipped_user_callbacks, result.skipped_unsupported_textures);
    }
    if (result.was_published() && !result.has_draw_commands &&
        g_visible.load(std::memory_order_acquire)) {
        // Input must never remain captured for a menu that cannot produce even
        // one supported draw command. The control thread observes unhealthy,
        // closes within its bounded grace period, and rearms on an explicit
        // later open after the underlying frontend problem is corrected.
        g_retry_on_next_open.store(true, std::memory_order_release);
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void publish_font_atlas(ImFontAtlas* atlas) {
    if (!atlas) return;
    const frame::font_publish_result result = frame::publish_font_atlas(atlas);
    if (result.was_published()) {
        // GetTexDataAsRGBA32 builds a dirty atlas, and ImGui's build path resets
        // TexID to null. Assign the frontend token only after that build. Doing
        // this before publication makes every later draw command look like an
        // unsupported null texture even though the pixels copied correctly.
        atlas->SetTexID(kFontTextureToken);
        g_font_generation.store(result.generation, std::memory_order_release);
    } else {
        flog("[overlay-v2] [ERROR] failed to publish ImGui font atlas");
        // The frontend atlas may already contain new UVs. Never let a later
        // renderer retry pair those frames with the previously published GPU
        // atlas after this publication failed.
        g_font_generation.store(0, std::memory_order_release);
        frame::clear();
        g_renderer_healthy.store(false, std::memory_order_release);
    }
}

void clear_draw_data() { frame::clear_frame(); }

bool renderer_healthy() {
    return g_renderer_healthy.load(std::memory_order_acquire);
}

void shutdown() {
    g_stopping.store(true, std::memory_order_release);
    g_renderer_ready.store(false, std::memory_order_release);
    g_visible.store(false, std::memory_order_release);
    dxgi_shadow::clear_callbacks();
    frame::clear();

    // Callback publication is now null and g_stopping makes a previously
    // captured callback pass-through. Wait for the one possible renderer
    // owner, then either retire or intentionally quarantine its resources.
    // Destruction is deliberately outside both backend mutexes because COM
    // Release on a third-party proxy is foreign, potentially re-entrant code.
    std::unique_ptr<d3d12::Session> retired_session;
    {
        std::lock_guard lock(g_render_mutex);
        publish_canvas(nullptr, 0, 0, false, false);
        retired_session = std::move(g_session);
        g_session_rebuild_pending = false;
        g_primary_generation = 0;
        g_primary_hwnd = nullptr;
        g_color_unknown_logged_generation = 0;
        g_present_reached_logged_generation = 0;
        g_describe_reject_logged_generation = 0;
        g_describe_reject_reason = nullptr;
        g_primary_block_reason = nullptr;
        g_queue_missing_logged_generation = 0;
        g_resize_gate = {};
        g_color_gate = {};
        g_pending_composited_buffer = {};
        g_observed_swapchains.clear();
        g_color_observations.clear();
        g_nvapi_color_cache = {};
    }
    g_nvapi_observer_enabled.store(false, std::memory_order_release);
    {
        std::lock_guard lock(g_nvapi_mutex);
        g_nvapi_display_bindings.clear();
        g_nvapi_color_observations.clear();
    }
    g_nvapi_state_revision.store(0, std::memory_order_release);

    if (retired_session && !retired_session->before_resize()) {
        // Shutdown cannot make an unfenced GPU submission safe. The DLL is
        // pinned and hooks are pass-through, so retaining the session until
        // process teardown is the least invasive option.
        (void)retired_session.release();
    }

    std::vector<QueueBinding> retired_bindings;
    {
        std::lock_guard lock(g_registry_mutex);
        retired_bindings.swap(g_queue_bindings);
    }
    // Hooks remain resident and become pass-through via g_stopping.  Do not
    // call hooks::deinit(); it could sever a later-installed detour chain.
}

} // namespace cte::overlay::present
