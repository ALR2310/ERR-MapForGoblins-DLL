# NVIDIA Smooth Motion x ImGui DX12 Present-hook Overlay Crash - Diagnosis and Fix Plan

Status: actionable. Crash is a GPU/driver-state conflict between our DX12 Present-hook
overlay (`src/goblin_overlay.cpp`) and NVIDIA Smooth Motion (driver-level frame
generation), NOT a data/textId bug. The same player on the same build does not crash
with Smooth Motion OFF. Faulting stack is 100% `eldenring.exe` + `nvwgf2umx.dll`, AV
write to 0x0 in the game's UI text memcpy, zero MapForGoblins frames; our log ends at
`[OVERLAY] atlas 400x280 built ...` (the first-open atlas upload), so the fault lands
on the game's NEXT render right after our first-open GPU burst.

Evidence: `scratch/user_reports/2/eldenring.exe.19036.dmp` +
`scratch/user_reports/2/MapForGoblins_2026-06-29.log`.

---

## 0. Why Smooth Motion breaks us (mechanism, in one paragraph)

Smooth Motion is a driver-side present interposer (module `nvpresent64.dll`, optical
flow via `nvofapi64.dll`) injected into the game at the SAME layer as ReShade/RTSS. It
wraps the game's `IDXGISwapChain` in an NVIDIA `NvPresent` COM wrapper, forces the game
to render off-screen, owns the real back-buffers, and calls `Present` MULTIPLE TIMES
PER GAME FRAME from a driver pacer thread - i.e. Present becomes re-entrant and
cross-thread, and there can be multiple command queues / multiple async presents.

Every assumption in our `hkPresent` is now false: Present is no longer once-per-frame on
the render thread; the `IDXGISwapChain*` we get may be the NvPresent wrapper (wrong
vtable / wrong back-buffers); and a blocking fence stall inside Present now stalls the
driver's pacer thread while the game's render thread is also submitting. We submit
foreign `ExecuteCommandLists` and full fence stalls on the GAME's DIRECT command queue
(`g_command_queue`, captured in `hkExecuteCommandLists`), in the middle of a Present the
driver is mid-interpolating. The driver's notion of "current back-buffer / fence
timeline" desyncs; the game's next text render writes through a null/stale presentation
pointer -> AV write to 0x0 inside the game, no MFG frames on the stack.

Prior art that matches exactly:
- RTSS/Afterburner (Unwinder): "original hooks do not support concurrent multithreaded
  Present()"; fix shipped = force Detours hooking + an SM-aware alternate D3D12
  command-queue detection path, auto-enabled when `nvpresent64.dll` is present.
- Dalamud (FFXIV, ImGui Present-hook overlay) PR #2849 "Add NVIDIA Smooth Motion
  Compatibility": unwrap the NvPresent swapchain, split build-draw-data from
  composite, gate render during resize. Near-exact precedent for us.
- NVIDIA Streamline DLSS-G guide: frame-gen "takes over frame presenting", "calls
  Present on a different thread", overlays "must not make assumptions about swap-chain
  and command queues", and FG must be OFF around resize/fullscreen (deadlock-prone).
- hudhook / UniversalHookX (mature DX12 ImGui hooks): reuse the game queue but NEVER
  full-stall per frame - they use N frame-contexts with per-slot allocators + a
  deferred fence wait that only blocks if that slot is still in flight, and a
  non-blocking `try_lock` Present that bails to `oPresent` on contention.
- osu!lazer #35278, ImGui #7207 / #7847: same family (Present-hook + uncoordinated
  queue submit / freeing upload buffer before fence = AV / device-removed).

---

## 1. Root cause, ranked (most -> least likely)

Each item is tied to a specific op in `src/goblin_overlay.cpp` and the prior-art
mechanism it triggers.

### R1 (PRIMARY) - First-open atlas/logo upload submits on the GAME queue with per-upload throwaway-fence full stalls, INSIDE Present
`upload_rgba` (~1371), called twice via `upload_atlas_and_logo` (~1679) from
`try_upload_atlas` (~1697) at the top of `render` (~1718). Per call it does:
`g_frames[0].allocator->Reset()` + `g_command_list->Reset()` (1422-1423) ->
`g_command_queue->ExecuteCommandLists` on the GAME queue (1447) -> creates a brand-new
`ID3D12Fence`, `Signal(fence,1)` on the GAME queue, `WaitForSingleObject(ev,1000)` full
GPU drain (1449-1461). So the first F10 frame wedges THREE foreign `ExecuteCommandLists`
(atlas copy, logo copy, then `submit_frame`'s overlay draw) + THREE full fence stalls
into one Present, between the game's last submit and `oPresent`.
Mechanism: RTSS "concurrent multithreaded Present + foreign D3D12 queue submission";
Streamline "any op that can cause Present to deadlock must turn FG off". This is the
last-log-line trigger and is unique to first-open (steady state does only one
submit+wait), which is exactly why it crashes on first F10 and not every frame.
**Top suspect by a wide margin.**

### R2 - Per-overlay-frame full `wait_gpu()` stall on the GAME queue, inside Present
`submit_frame` (~1328) ends with `g_command_queue->ExecuteCommandLists` (1359) then
`wait_gpu()` (1360); `wait_gpu` (~1313) does `Signal(g_fence,++v)` on the GAME queue +
`WaitForSingleObject(g_fence_event,1000)`. A hard CPU<->GPU serialize every overlay
frame on the queue the driver is pacing. Mechanism: hudhook/UniversalHookX both avoid
this; it advances a fence timeline the driver synchronizes against and fights FG pacing.
Secondary (would recur every frame, not just first-open) but must be removed.

### R3 - All overlay work goes on the captured GAME `g_command_queue` instead of a private queue
`hkExecuteCommandLists` (~1847) latches the first DIRECT queue into `g_command_queue`;
ops 1447, 1359 submit on it. Under SM there can be multiple queues and the one we
captured may not be the one SM expects; interleaving our submissions into the
interpolated timeline is the structural root that makes R1/R2 dangerous. Mechanism: RTSS
"SM-compatible D3D12 command-queue detection"; NVIDIA "don't assume queues".

### R4 - We cache + transition the swapchain's back-buffers; under SM the swapchain may be the NvPresent wrapper
`init_dx12` (~1156) caches `sc->GetBuffer(i)` into `g_frames[i].render_target` + RTVs
(1191-1196); `submit_frame` transitions PRESENT->RT->PRESENT on the "current"
back-buffer (1340-1356). Under SM the `sc` handed to `hkPresent` can be the NvPresent
wrapper (wrong back-buffers), and the buffer index the app sees may not be what the
driver presents. Cached resource pointers can go stale across an FG buffer rotation.
Contributing, not the standalone trigger.

### R5 - ImGui DX12 backend first-frame font upload piles onto the first-open burst
`ImGui_ImplDX12_Init` (1291) + first `ImGui_ImplDX12_NewFrame`/`RenderDrawData` lazily
upload the font atlas on first open, on our heap/queue - more first-open GPU traffic in
the exact crash window. Minor amplifier of R1.

### R6 - `hkResizeBuffers` stalls the GAME queue during FG-triggered resizes
`seh_resize_teardown` (~1825) calls `wait_gpu()` then `teardown_dx12()`. SM toggles /
alt-enter / HDR changes trigger ResizeBuffers and the driver may resize internal
interpolation buffers concurrently; NVIDIA says FG must be off around resize. Not on the
crash path (crash is on open) but the same anti-pattern; fix alongside.

---

## 2. How to reproduce on our dev machine

### 2.1 Hardware / driver prerequisites
- GPU: RTX 40 or RTX 50 series (NOT 20/30 - unsupported).
- Driver: RTX 50 since 572.16 (Jan 2025); RTX 40 since the Aug-2025-era Game Ready
  driver (preview 590.10/590.26). Install the latest Game Ready Driver AND the latest
  NVIDIA App (both required).
- HAGS (Hardware-Accelerated GPU Scheduling) must be ON (Windows Settings > Display >
  Graphics > Default graphics settings). Hard driver requirement for this FG class.

### 2.2 Enable Smooth Motion for Elden Ring
NVIDIA App > Graphics > pick `eldenring.exe` under Program Settings (or Global) >
"Driver Settings" group > Smooth Motion = ON. Works for DX12 (Elden Ring qualifies).
Do NOT also enable DLSS Frame Generation (competing tech). Ensure the game is not
hard VSync/frame-cap-locked so an interpolated frame can be inserted.

### 2.3 Confirm it is actually active (in-process detection)
Smooth Motion loads its hook runtime lazily and can arrive AFTER startup, so check at
startup AND again right before first overlay submit:
```cpp
// Primary signal = the SM hook/runtime module. Secondary = optical-flow runtime.
static bool smooth_motion_active()
{
    return GetModuleHandleW(L"nvpresent64.dll") != nullptr   // primary
        || GetModuleHandleW(L"nvofapi64.dll")  != nullptr;   // secondary corroborator
}
```
Note: `nvwgf2umx.dll` is ALWAYS present on any NVIDIA D3D game - it is NOT an SM marker
(it just happens to be in our crash stack). Key only on `nvpresent64.dll`.
Streamline-based FG would instead show `sl.interposer.dll` / `sl.common.dll`.

We do not have SM hardware locally, so the canonical path is an instrumented test DLL
(below) shipped to the reporting player; detection lets us also auto-log whether SM was
live in any future report.

---

## 3. Instrumented test build - one DLL, each mitigation an INDEPENDENT toggle

Goal: a single special build the SM player runs, where every candidate mitigation is an
independent on/off flag, so the matrix in section 4 isolates which one (or combination)
stops the crash. Each flag is read from the ini (schema-driven, see
`src/goblin_config_schema.cpp` + `mfg_inigen`) under a new `[debug_smoothmotion]`-style
group, AND mirrored to a runtime hotkey-cyclable state for players who can't edit ini
between runs. Each flag emits a one-time `[SM-TEST]` log line proving it took effect, and
a startup line dumps the full flag state + `smooth_motion_active()`.

Add a small struct (file-local in `goblin_overlay.cpp`) populated from config at
`setup()`/first Present, so the hot path just reads atomics:
```cpp
struct SmTest {
    std::atomic<bool> own_queue{false};     // F1: dedicated overlay queue
    std::atomic<bool> no_frame_wait{false}; // F2: drop per-frame wait_gpu, ring instead
    std::atomic<bool> upload_off_present{false}; // F3: pre-create/defer atlas upload
    std::atomic<bool> auto_disable{false};  // F4: detect SM -> overlay off (toast fallback)
    std::atomic<bool> nonblocking_present{false}; // F5: try_lock + reentrancy guard
    std::atomic<bool> unwrap_swapchain{false};    // F6: NvPresent unwrap + per-call GetBuffer
    std::atomic<bool> no_resize_stall{false};     // F7: don't stall queue in ResizeBuffers
} g_sm;
```
Log on apply, e.g. `spdlog::info("[SM-TEST] flags own_queue={} no_frame_wait={} ...")`.

### F1 - `sm_own_queue` : dedicated overlay command queue (addresses R1+R2+R3+R6 at root)
Sketch: in `init_dx12`, after `GetDevice`, create our own DIRECT queue
`g_overlay_queue = g_device->CreateCommandQueue(DIRECT)`. Define a helper
`ID3D12CommandQueue* overlay_queue() { return g_sm.own_queue ? g_overlay_queue : g_command_queue; }`
and replace ALL three `g_command_queue->ExecuteCommandLists`/`Signal` sites
(`upload_rgba` 1447/1453, `submit_frame` 1359, `wait_gpu` 1318) with `overlay_queue()`.
We still read the back-buffer/RTV for the current frame, but the GPU work that writes it
runs on a queue the driver is NOT pacing -> our submits/stalls can no longer interleave
with FG's interpolation submissions or advance the game-queue fence timeline.
Proof line: `[SM-TEST] F1 dedicated overlay queue created` + `[SM-TEST] submit on overlay queue`.
Teardown: release `g_overlay_queue` in `teardown_dx12`.

### F2 - `sm_no_frame_wait` : remove per-frame full stall, use N-in-flight ring (addresses R2)
Sketch: add `UINT64 FrameContext::fence_value` to the struct (115-120). In
`submit_frame`, BEFORE reusing `g_frames[idx]`, only wait if that slot's recorded
`fence_value` hasn't completed: `if (f.fence_value && g_fence->GetCompletedValue() <
f.fence_value) wait_on(f.fence_value);`. After `ExecuteCommandLists`, do
`Signal(g_fence, ++g_fence_val)` and store `f.fence_value = g_fence_val` - but DO NOT
block (skip the `wait_gpu()` at 1360). With `g_buffer_count` (2-3) slots a full
swapchain cycle has elapsed so the guard almost never blocks. Gate: when
`g_sm.no_frame_wait` is false, keep the old `wait_gpu()` so the flag is a clean A/B.
Also requires per-frame allocator already exists (we have one per buffer) - but note
`submit_frame` currently resets `f.allocator` which is correct for the ring; just stop
the trailing stall.
Proof line: `[SM-TEST] F2 ring sync (no per-frame stall)` once; optionally a debug
counter of "ring wait actually blocked N times".

### F3 - `sm_upload_off_present` : do the atlas/logo upload ONCE, off the Present hot path (addresses R1+R5)
Sketch: split `build_atlas_rgba()` (pure CPU, already separate) from upload. When the
flag is set, run `upload_atlas_and_logo` from a one-shot worker kicked right after
`init_dx12` succeeds (or on the first Present but BEFORE `g_menu_open` gating), using a
SINGLE batched command list for atlas+logo (one `ExecuteCommandLists`, one fence wait)
on the overlay queue (F1) / or a transient COPY queue, and fence-wait on the WORKER
thread, not in Present. `render`/`try_upload_atlas` only ever CHECK `g_atlas_ready`
(atomic) and skip drawing icons until it flips. This removes the in-Present
`CreateCommittedResource`+`Map`+`ExecuteCommandLists`+`WaitForSingleObject` burst that
the dump fingered. Keep the font SRV in the permanent `g_srv_heap` (already slot 0).
Proof line: `[SM-TEST] F3 atlas upload off-Present on worker, batched` + the existing
`atlas ... built` line now appearing BEFORE first menu draw / not on the present thread.

### F4 - `sm_auto_disable` : detect SM -> disable the GPU overlay, fall back to toast (safety net)
Sketch: at `setup()` and again at first `hkPresent`, if `smooth_motion_active()`, set a
flag that makes `hkPresent` skip `render(sc)` entirely (just `oPresent`) and route UI to
the existing toast-cycle fallback; surface a one-line on-screen warning. This is the
RTSS-style "key off the FG module and degrade". Guarantees no-crash even if every other
flag fails; confirms attribution (overlay-off-under-SM = no crash).
Proof line: `[SM-TEST] F4 Smooth Motion detected -> GPU overlay suppressed`.

### F5 - `sm_nonblocking_present` : re-entrancy-safe, non-blocking Present (addresses the multithread/re-entrant Present)
Sketch: wrap the render section of `hkPresent` (1816-1817) in `std::try_lock` on a
`g_render_mtx` + a thread-recursion guard (`thread_local bool in_present`). If the lock
isn't acquired or we're re-entered (driver's extra Present), skip render and call
`oPresent` immediately - never block, never render twice. This is the hudhook `try_lock`
pattern and directly handles SM's multiple/off-thread Present calls.
Proof line: `[SM-TEST] F5 present try_lock active` + a counter `present render skipped
(contended/reentrant) N`.

### F6 - `sm_unwrap_swapchain` : unwrap NvPresent + re-acquire back-buffer per call (addresses R4)
Sketch: in `hkPresent`/`init_dx12`, inspect the first few vtable entries of `sc`; if the
function pointers resolve (via `GetModuleHandleEx` from address) to a module whose name
contains `NvPresent`/`nvpresent`, unwrap to the real `IDXGISwapChain` before use (Dalamud
PR #2849 approach). Minimum viable variant even without full unwrap: stop caching
`render_target` across the swapchain's life - in `submit_frame` call
`sc->GetBuffer(GetCurrentBackBufferIndex())` each frame and create/refresh the RTV on
demand (hudhook does this), so an FG buffer rotation can't leave us on a stale resource.
Proof line: `[SM-TEST] F6 swapchain unwrapped (NvPresent)` or `[SM-TEST] F6 per-call
GetBuffer`.

### F7 - `sm_no_resize_stall` : don't stall the game queue in ResizeBuffers (addresses R6)
Sketch: in `seh_resize_teardown` (1825) gate the `wait_gpu()` behind a "resize in
progress" flag and, when `g_sm.no_resize_stall`, replace the game-queue stall with a
wait on the overlay queue (F1) or just `teardown_dx12()` after a non-game-queue flush;
also set a flag so `hkPresent` skips render while a resize is in progress.
Proof line: `[SM-TEST] F7 resize teardown without game-queue stall`.

Validation flag (always on in the test build): `sm_log_gpu_ops` adds ordered flushed log
lines around EACH GPU op so the LAST surviving line localizes the trigger if a config
still crashes: `atlas copy submitted` / `atlas fence signalled` / `atlas fence waited`
(per upload), `overlay CL submitted` / `overlay wait_gpu done` (submit_frame). Per the
crash analysis the current dump prints the full combined `atlas ... built` (after BOTH
uploads + SRVs), so the fault is a LATER game frame - these lines confirm whether a
given mitigation moved or removed the trigger.

---

## 4. Player test protocol (matrix)

All runs: Smooth Motion ON, same save, same spot, open the overlay with F10 once, wait
~5s, move the camera, close, repeat open/close 3x. Record: crash? which `[SM-TEST]` /
`[OVERLAY]` line was last? After each run send the new `MapForGoblins_<date>.log` (and
any fresh `%LOCALAPPDATA%\CrashDumps` minidump).

Baseline and single-flag isolation (each flag alone, all others OFF):

| # | Config (SM ON) | If NO crash means | If still crashes means |
|---|----------------|-------------------|------------------------|
| 0 | all flags OFF (current behavior) | (won't happen) reproduces? if not, env differs | confirms baseline repro; proceed |
| 1 | F4 auto_disable only | overlay-off-under-SM is safe -> confirms it's OUR GPU work, not data | crash is outside our render entirely (unlikely) |
| 2 | F1 own_queue only | the GAME-queue submission was the root (R1/R2/R3) | queue isn't the only factor; combine |
| 3 | F3 upload_off_present only | the first-open in-Present upload burst (R1) was the trigger | trigger is steady-state submit/stall, not upload |
| 4 | F2 no_frame_wait only | the per-frame stall (R2) mattered most | stall wasn't the sole cause |
| 5 | F5 nonblocking_present only | re-entrant/off-thread Present was the killer | Present concurrency wasn't the sole cause |
| 6 | F6 unwrap_swapchain only | we were using the wrong (wrapped) swapchain/back-buffer (R4) | wrapper wasn't the issue |

Combination runs (only if no single flag fully fixes it):

| # | Config (SM ON) | Reads as |
|---|----------------|----------|
| 7 | F1 + F3 | dedicated queue + off-Present upload = the predicted minimal fix |
| 8 | F1 + F2 + F3 | minimal robust set (queue + ring + off-Present upload) |
| 9 | F1 + F2 + F3 + F5 + F6 | full hudhook/Dalamud-class hardening |
| 10 | F4 (fallback) | guaranteed-safe shipping floor if 7-9 still flake |

Interpretation rule: the FIRST row that yields zero crashes across 3 open/close cycles
(and across a couple of sessions) identifies the necessary mitigation set. If F4 is the
only zero-crash row, ship F4 as the floor and keep iterating on F1/F3 offline.

---

## 5. Recommended implementation order + the one-shot first try

### The single most-likely one-shot fix: F1 + F3 together (test row 7)
- F1 (dedicated overlay command queue) removes every interaction where we submit to or
  stall the driver-paced GAME queue - the structural root of R1/R2/R3/R6.
- F3 (one-time, batched, off-Present atlas/logo upload) removes the specific first-open
  burst the dump fingered (R1/R5).
Together they eliminate exactly the behavior - foreign submit + full stall on the game
queue during a re-entrant Present, concentrated on first open - that turns Smooth
Motion's Present interception into a game-side AV. Both are also pure wins with SM OFF
(no per-frame stall on the present path, no in-Present uploads), so they are safe to ship
even though we cannot reproduce locally. Pair with F2 (ring sync) since F1 makes the
per-frame `wait_gpu` pointless anyway.

### Order
1. F1 dedicated overlay queue (highest leverage, neutralizes R1/R2/R3/R6 root).
2. F3 atlas/logo upload off Present, batched, one fence (removes the dump's trigger).
3. F2 drop per-frame `wait_gpu`, N-in-flight ring with per-slot `fence_value`.
4. F4 SM-detect auto-disable as the always-available safety floor (ship-able immediately
   on its own as a stopgap hotfix while 1-3 are validated).
5. F5 non-blocking/re-entrancy-safe Present (try_lock + recursion guard).
6. F6 NvPresent unwrap + per-call `GetBuffer` (defensive; full Dalamud-class robustness).
7. F7 no game-queue stall in ResizeBuffers.

Ship plan: cut the instrumented test DLL with all of F1-F7 as toggles + the GPU-op
logging, default all OFF (= current behavior) so it is a safe build, and have the SM
player walk the matrix. Fold the winning set into a normal release. If a same-day hotfix
is needed before the matrix completes, ship F4 (detect-and-disable) alone - it cannot
crash and degrades to the toast fallback.

### Concrete code touch-points (all in `src/goblin_overlay.cpp` unless noted)
- `FrameContext` struct (115-120): add `UINT64 fence_value` for F2.
- globals (122-128): add `ID3D12CommandQueue* g_overlay_queue` for F1; `std::mutex
  g_render_mtx` for F5; the `SmTest g_sm` block.
- `init_dx12` (1156): create `g_overlay_queue` (F1); keep font SRV in slot 0 (F5/R5 ok).
- `upload_rgba` (1371) / `upload_atlas_and_logo` (1679) / `try_upload_atlas` (1697):
  route to `overlay_queue()` (F1); move to one-shot batched worker (F3).
- `submit_frame` (1328) + `wait_gpu` (1313): `overlay_queue()` (F1); ring sync (F2);
  per-call `GetBuffer` option (F6).
- `hkPresent` (1752): try_lock + recursion guard (F5); SM-detect gate + `smooth_motion_active()`
  (F4); skip render during resize (F7).
- `seh_resize_teardown` / `hkResizeBuffers` (1825/1838): non-game-queue teardown (F7).
- ini schema: add the `sm_*` keys to `src/goblin_config_schema.cpp` (mfg_inigen
  regenerates the ini; DLL `ensure_ini` migrates at runtime). Mirror to a hotkey cycle
  for between-run toggling without ini edits.

---

## Sources (most load-bearing)
- NVIDIA Streamline DLSS-G Programming Guide (Present interception, extra async queue,
  off-screen render, "overlays must not assume queues", deadlock around resize,
  eUseDXGIFactoryProxy).
- Dalamud PR #2849 "Add NVIDIA Smooth Motion Compatibility" (NvPresent unwrap,
  Step/Render split, resize gating) - near-exact ImGui Present-hook precedent.
- guru3D RTSS 7.3.7 / Afterburner 4.6.6 notes ("original hooks don't support concurrent
  multithreaded Present()", Detours forced + alternate D3D12 queue detection keyed on
  `nvpresent64.dll`).
- hudhook DX12 backend + UniversalHookX (reuse game queue but N frame-contexts +
  deferred per-slot fence wait, no per-frame stall; try_lock non-blocking Present;
  per-call GetBuffer).
- ImGui #7207 / #7847 / #8511 (Present-hook GPU crash; fence before freeing upload
  buffer; font SRV in a permanent heap).
- osu!lazer #35278 (AV in swapchain/COM under SM); guru3D Smooth Motion threads;
  Hardware Times (SM is driver-level, hooks present in-process).
- NVIDIA support a_id 5621 (enable SM in NVIDIA App); HAGS requirement.

---

## IMPLEMENTED (2026-06-30): `smooth_motion_fix` test switch

One ini key `smooth_motion_fix` (uint8, default 1) in the Overlay section selects ONE
candidate fix - no "off" (test build, every value is an attempt). Logs `[SM-FIX] mode N: ...`.
- **1** = isolated GPU: icon-atlas upload runs on a private command queue/allocator/fence
  (`g_ovl_*`, never the game queue) + per-frame full `wait_gpu()` replaced by an N-in-flight
  ring (per-slot `FrameContext::fence_value`). Kills R1+R2.
- **2** = wrapper-aware: `submit_frame` re-acquires the back buffer via `sc->GetBuffer(idx)` +
  rebuilds the RTV each frame; `hkPresent` skips a re-entrant Present (`g_in_present` CAS).
  Addresses R4 + multithreaded Present.
- **3** = both 1+2.
- **4** = `sm_detected()` (GetModuleHandleW("nvpresent64.dll")) -> skip the GPU overlay render
  entirely (control + guaranteed no-crash floor).
Touch points: src/goblin_overlay.cpp (globals + helpers ~162, init_dx12 private queue,
upload_rgba queue select, submit_frame ring + per-call RTV, hkPresent guard/skip, setup log,
teardown release); ini key in src/goblin_config_schema.cpp + src/goblin_config.hpp.
Also reverted the GetAsyncKeyState->key-state-table change (it broke F10 on some setups);
hotkeys read GetAsyncKeyState again.
Player protocol: Smooth Motion ON, set value 1, open/close F10 a few times; if still crashes
try 2, 3, 4; report which stops it (and the `[SM-FIX] mode N` log line).
