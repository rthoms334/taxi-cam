# Performance audit

Static review of the current tree on `feature/render-performance`. This is a code-path and contract audit, not a live Microsoft Flight Simulator 2024 measurement. Hardware compositor and capture tests prove pixel and ordering contracts; they do not establish cockpit frame time, stutter, or GPU occupancy inside the simulator. Confirm every ranked item against Diagnostics (`probe_cpu_ms`, `stage_ms`, `query_ms`, `read_ms`, `aa_ms`, PFD copy counters) with TAXI off versus on at the same cockpit view.

The extra camera views are the dominant intended cost. Taxi Cam asks MSFS to render a nose pane and a tail pane, then copies and composites those images onto the PFD. Lowering **Camera frame rate** (15–60) reduces activation opportunities; it does not remove manager-update, hook, or telemetry work.

## What runs where

| Thread / engine | Work | Frame impact if it stalls |
| --- | --- | --- |
| MSFS camera-manager update | Probe observer, pose, AA, gate pulses, then the original update | Direct: the original manager update runs only after the observer returns |
| MSFS render / submit | D3D12 hooks, capture insertion, PFD copy or stamp, queue before/after | Direct: added GPU work and any lock held across a native call |
| Bridge worker | Control, discovery, composition `service()`, status, logging | Indirect: contends for the same mutexes the render path uses |
| SimConnect worker | Body stream, TAXI, identity, on-ground, lighting | Indirect: lock churn with probe/bridge readers |
| Companion UI / IPC | Settings, heartbeat, status copy | None for MSFS frames if the bridge lock stays non-blocking |

Pixels stay on the GPU. The companion receives counters and status, not image frames.

## Cost model

Work falls into four bands. Do not add the bands together as if they were independent FPS.

1. **MSFS scene renders for the two owned views.** This is the large cost. Taxi Cam only limits how often a render gate opens. Content, lighting, and upscalers belong to the simulator.
2. **Observer CPU on the manager-update thread.** Private memory queries, pose, AA, and native calls complete before the original update. Diagnostics already split this into `stage_ms`.
3. **Hook and capture work on render/submit threads.** List lookup, source-state tracking, full-pane copies, barriers, and PFD injection.
4. **Private composition.** One 768×763 draw plus patch redraws on Taxi Cam's queue, ordered onto the shared fence timeline.

## Already well guarded

These are not defects. Several look expensive until the contract is included.

| Guard | Where | Why it exists |
| --- | --- | --- |
| Idle observer | `ProbeInspectionGate` | Established, suspended, closed, ready pair does no native or memory work |
| 250 ms inspection throttle | `probe.cpp` | Periodic path is not every manager update |
| Fused pair inspection | `observer()` | One read-only transaction for manager + both views |
| Close-only inspection | `close_owned_pair()` | Gate close without output/handoff refresh |
| Alternating schedule | `RenderSchedule` | One open pulse, then a closed interval; UI rate changes cannot burst |
| No cache across frames | architecture / probe | Field values and page proofs die after a write, native call, or later frame |
| Scene pose, not SimConnect | `capture_pose()` | Public telemetry is a plausibility guard, never the mount transform |
| Packet and VRAM caps | capture manager | 16 packets, 256 MiB aggregate, 128 MiB per packet |
| Unrelated submit bypass | `before_submission()` | Fully observed lists with no camera/PFD/source work skip the submission lock |
| Metadata batch cache | `d3d12_bridge.cpp` | Nested barrier callbacks reuse the list record without re-locking |
| `maybe_selected()` | `d3d12_bridge.cpp` | Lock-free reject of non-PFD resources |
| `pfd_dirty` deferral | `after_draw()` | Does not composite between individual HTML/glyph draws |
| Consumer fence timeline | capture manager | Output is not overwritten while a tracked PFD read still needs it |
| Query-scope block | stamp path | Camera draws cannot add samples to simulator queries |
| Calibration write budget | `WriteBudget` | Identification clears are capped per 50 ms window |
| Non-blocking IPC | bridge mailbox | Busy companion mutex keeps last valid settings; it does not block MSFS |
| Tail rate limit | `record_queue_tail()` | Default 15 Hz, clamped to the camera-rate setting |
| Stale-frame discard | `scene_runtime::service()` | Older device-timeline frames never compose |
| No GPU readback on the live path | capture / compositor | Map/readback is test-only |
| Re-entrancy guards | `OwnedWork`, `ScopedBypass` | Private GPU work is not re-observed as MSFS work |
| LVar names once | body-pose provider | Per-frame taxi reads are numeric, not string lookups |

`ID3D12CommandQueue::Wait` in `begin_transaction()` is a GPU queue wait on an already-queued timeline Signal. It does not block the CPU submit thread waiting for GPU completion. Treating it as a CPU stall is incorrect.

Refreshing every admitted PFD patch on each composition is required. MSFS can replay a previously recorded copy from a stable buffer address. Skipping a patch because "the format did not change" can leave a replayed copy showing a previous aircraft, exposure, or ground-speed value.

## Findings

Severity is the likely effect on simulator frame time or GPU bandwidth if the path is active. It is not a crash ranking. Items marked **contract** cannot be removed; they can only be narrowed.

### 1. Observer work before the original manager update — Critical, contract

**Where:** `src/camera/probe.cpp` `observer()`, installed on manager vtable slot 15.

The thunk delivers RCX, runs the observer, then tail-jumps to the original update. Pose application and gate opens must happen in that window so the engine consumes the new transform on the same pulse. That is correct. It is also the highest-risk CPU path Taxi Cam owns: every serviced callback delays the camera manager.

Active cameras still pay, at scheduled pulses:

- fused or full pair inspection (pool, entries, both views, handoff)
- `capture_pose()`: aircraft metadata + scene-node transform + public plausibility check
- `prepare_owned_view_aa()` and a confirming `inspect_entry()`
- native `apply_pose` / `activate_entry`

Idle OFF is already cheap. The expensive case is TAXI on at 15–60 activations per camera per second. At low simulator update rates the schedule can still require open/close work on every manager update; the rate setting is an opportunity cap, not completed FPS.

**Do not:** move pose writes off the observer thread, cache page proofs across frames, or blend SimConnect into the mount.

**Can change:**

- Keep `aa_applied` per owned entry ID and confirm flags with the existing 16-byte writable-span read instead of a full `inspect_owned_view` after a successful write.
- After a fused inspection that proves generation, dimensions, and resource identity unchanged, skip the non-pose stages until the next gate edge. Pose still runs on an opening pulse.
- Export a hard SLA on `observer_last_ms` / `observer_max_ms` in the companion (warn above a measured budget; do not drop required guards when the budget is exceeded).
- Count re-entrant skips (`observing.test_and_set`) so overload is visible.

### 2. Shared `scene_runtime` mutex on PFD injection and composition — High

**Where:** `src/graphics/scene_runtime.cpp` — one `std::mutex` for `copy_patch`, `stamp_at_recording_end`, `service`, `snapshot`, exposure, and ground speed.

`copy_patch` runs on the MSFS recording thread at a proved PFD copy opportunity. `service()` runs on the bridge thread about every 25 ms, holds the same lock through `poll_completed_frames`, compositor record, patch draws, and private submit. A composition that takes longer than a recording callback will stall PFD injection and increment `state_skips`.

`register_consumer_recording()` takes the capture-manager mutex while that runtime lock is held. Submission already documents the opposite order (discover lists before taking `submission_mutex_`). The copy path is the remaining cross-thread pinch.

**Can change:** publish an immutable patch descriptor (buffer pointer, footprint, generation) after composition; let `copy_patch` read it without the composition lock. Keep lifetime on the fence timeline. Do not publish a buffer that a later `prepare()` can free.

### 3. Registry recursive mutex on hook lookups — High

**Where:** `src/bridge/d3d12_bridge.cpp` `Registry::mutex`, `find_list()`, `Targets::record()`, `stage_pfd()`, `drain_pfds()`, `graphics_status()`, `discover_pfds()`.

`after_draw` already uses atomics for selected-draw counts. `find_list` still takes the recursive mutex on most hook entries. Barrier batches use `MetadataBatchCache`; draw, OM, Close, and copy paths often miss that cache.

`graphics_status()` copies the registry under that lock. The bridge loop calls it more than once per 25 ms tick (`warm_readiness`, mask computation, status publish). `discover_pfds()` (1 Hz) walks and erases resource/list/RTV maps under the same lock.

**Can change:**

- Thread-local `(list*, generation) → List*` validated by the recording counter; fall back to the locked map on miss or Reset.
- One `GraphicsStatus` snapshot per bridge iteration.
- Event-driven discovery on create/retire; skip the 1 Hz `erase_if` sweep when no retirement occurred.
- Keep `refresh_selected` as a pointer cache (already present) and stop scanning `resources` unless the cached pointer is dead or the route ID changed.

### 4. Full-pane capture copies on the application list — High

**Where:** `src/graphics/scene_capture_d3d12.cpp` `record_copy_source`, `record_render_target_source`, `record_render_target_source` enhanced path.

Snapshots are `CopyResource` of the camera output (about 736×251 and 736×496, or the A350 774-wide panes). That is the right size: these are the native view allocations, not the main window. The cost is still two extra full-texture copies plus, on the RT-exit path, transition to `COPY_SOURCE` and back on the **application** command list.

The enhanced path uses `D3D12_BARRIER_SYNC_ALL` around the copy. That is broader than a copy-only sync. Prefer the whole-image copy path when MSFS already copies the texture; that path inserts no extra source barrier.

Downscaling at capture would shrink bandwidth and also change compositor sampling, guides, and any future lighting work. Do not do it as a silent default.

**Can change:** keep preferring copy-path capture; if PIX shows RT-exit as the live path, try a narrower enhanced sync (`COPY` / `RENDER_TARGET`) behind a debug-layer and pixel-oracle run. Leave pane resolution at the profile size.

### 5. Private composition and every admitted patch every pair — Medium

**Where:** `src/graphics/scene_frame_output.cpp` `prepare()`, `prepare_patches()`.

Each completed nose+tail pair:

1. Records the 768×763 compositor.
2. Copies that image into the stable 2.3 MiB row-major buffer.
3. For every admitted patch slot (up to eight): RT transition, full-rectangle stamp draw, copy to a stable typed buffer.

`set_inputs` skips SRV writes when resources and formats match (`S_FALSE`), but `same_object()` uses two `QueryInterface(IUnknown)` calls per comparison, including the alias checks that run even on the unchanged path. `scene_format()` also calls `GetDesc()` on each pending resource.

Patch refresh is required when composition content changed. It is waste when neither snapshot identity, exposure, ground speed, nor guides changed and `service()` still records. That is uncommon while taxiing (speed and scene change) and common when parked with cameras on.

**Can change:** skip `prepare()` when both snapshot tokens, exposure, GS, and composition are unchanged and the previous submit is still current. Always refresh after profile or guide changes. Cache `GetDesc().Format` on the capture packet. Compare compositor inputs by the already-retained COM pointers before calling `same_object()`.

### 6. OM descriptor snapshots while a PFD is pending — Medium

**Where:** `d3d12_bridge.cpp` `Targets::record()`.

When any side has pending PFD work, each relevant `OMSetRenderTargets` copies bound RTVs and the DSV into private heaps **before** the native call. That is required: MSFS may reuse the CPU handles immediately. Heaps are created once per list.

PFD HTML/glyph passes set OM frequently. Snapshotting every bind is CPU and device-copy traffic on the render thread.

**Can change:** snapshot only when the bound target is a selected PFD, or when the handle set changed since the last snapshot on that recording. Keep the before-forward copy; never restore from a borrowed handle.

### 7. Bridge worker at 25 ms with repeated locked snapshots — Medium

**Where:** `src/bridge/bridge_main.cpp` `run_impl()`.

The worker `Sleep(25)` whether cameras are idle or live. A typical iteration can call `scene_snapshot()` more than once, `graphics_status()` more than once, `get_aircraft_identity()` more than once, and `sample_body_pose()` on the recovery and prewarm paths. Each snapshot takes the probe or registry mutex.

`log_status()` opens, appends, and closes `%LOCALAPPDATA%\Taxi Cam\bridge.log` on every line. Status changes and the 5 s heartbeat can emit several lines (PFD, scope, draw, retention, copy). That is syscall traffic on the same process as the hooks, not on the render thread, but it coincides with TAXI transitions.

`pfd_inventory()` builds and sorts a vector under the registry lock every status publish that copies candidates.

**Can change:**

- One stack-local `ProbeSnapshot` / `GraphicsStatus` / identity / pose per iteration; pass them into helpers.
- Wait on a settings-changed event with a 100–250 ms idle cap; keep 25 ms only while a side is active, warmup is running, or recovery is pending.
- Hold one append handle for the process lifetime.
- Refresh the 16-slot candidate list only when resource IDs change or the companion opens PFD routing.

### 8. SimConnect polling after cameras are parked — Medium

**Where:** `src/camera/body_pose_provider.cpp` worker.

Body pose uses a `SIM_FRAME` stream. In addition, while the worker is alive:

| Request | Interval |
| --- | --- |
| `CameraGet` + TAXI LVars | 50 ms (20 Hz) |
| `SIM ON GROUND` | 250 ms |
| Ambient lighting | 500 ms |
| Aircraft type + path | 1000 ms |

`CameraGet` is for pose plausibility and calibration, not the mount. TAXI at 20 Hz is for button follow. On-ground at 4 Hz exists for background warmup. The worker keeps this cadence after gates close, as long as the provider is not shut down.

Readers take a shared SRWLOCK; the worker takes exclusive on each packet. The bridge calls several getters per 25 ms tick.

**Can change:** drop `CameraGet` / TAXI to a slow watchdog while gates have been closed for a few seconds and no warmup is running; restore on the next ON edge. Request on-ground only during prewarm. Publish a seqlock POD snapshot so probe/bridge readers do not take the SRWLOCK. Keep LVar definitions at startup.

### 9. Queue-tail capture scan — Medium

**Where:** `scene_capture_manager.cpp` `record_queue_tail()`, `source_candidate()`.

Source candidates are a 512-slot array (`MaximumDevices * 128`). Tail capture walks the array after a source-touched submit. Barrier/draw helpers call `source_candidate()` with a linear scan. Rate limiting prevents over-capture; the CPU walk still runs.

Tail also issues an extra `ExecuteCommandLists` for a private list on the same receipt. That is the fallback when MSFS does not make a whole-image copy. Prefer the copy path when it exists.

**Can change:** index candidates by resource pointer for lookup. Keep the 512 cap.

### 10. Compositor pixel shader — Low–medium

**Where:** `src/graphics/camera_compositor_d3d12.hpp` `ps_main`.

One fullscreen triangle at 768×763. Ground-speed glyphs unroll 16 segment-distance tests per glyph. Guide tests run `reference_guide()` for every pixel when guides are enabled, then discard most pixels. HDR feeds add Reinhard and sRGB encode.

This is bounded and private. It is not the main-view cost. The GS panel could clip before the glyph loop (it already returns early for pixels outside the box). Guide tests could run only in small panes around the stored points.

### 11. Companion 200 ms status pump — Low

**Where:** `src/app/companion.cpp` connection worker.

Every 200 ms the worker copies the full `Settings` + `Status` block (about 2–3 KiB plus 16 candidates) and `PostMessageW` to the UI. Hidden-tray idle does not need 5 Hz invalidation.

**Can change:** post only when heartbeat, message, IDs, or counters change; 500 ms when the window is hidden.

### 12. Calibration budget does not cover copies or stamps — Low (policy)

`WriteBudget` applies only to identification clears. Live PFD copies and stamps are intentionally unlimited so a proved opportunity is not dropped. That is the right default. Add a separate diagnostic counter if copies per recording ever exceed one per side; do not reuse the calibration limit for camera delivery.

## Thread and lock map

```text
Manager update ── observer ── runtime.mutex (probe) ── memory queries / native calls
                         └── scene_handoff mutex (publish / stop)

Render / Close ── find_list (registry recursive mutex)
              └── copy_patch / stamp (scene_runtime mutex)
                    └── register_consumer (capture manager mutex)

Execute        ── observe_unknown_lists (registry)
              └── submission_mutex + manager mutex
                    └── queue Wait (GPU) / Signal / optional tail Execute

Bridge 25 ms   ── scene_snapshot (probe mutex)
              └── graphics_status / discover_pfds (registry)
              └── service() (scene_runtime mutex, then private submit)
              └── mailbox try_lock (IPC)

SimConnect     ── exclusive SRWLOCK on packets
Bridge/probe   ── shared SRWLOCK on getters
```

Known safe order: list discovery before `submission_mutex_`. Unsafe hold: `scene_runtime` mutex across composition while a hook needs `copy_patch`. Recursive registry mutex plus `graphics_status` from the bridge is the other contention pair.

## Memory and allocation

Hot GPU paths do not heap-allocate per draw. Resource and list `shared_ptr` creation is on first observe. Patch textures are lazy and reused.

Recurring allocations that can be removed without changing contracts:

- `std::string` status messages in the observer on failure and some waiting paths
- `scene_snapshot()` copies those strings under the probe mutex
- `pfd_inventory()` vector + sort under the registry mutex
- `log_status()` path string construction and per-line file open

Process-lifetime `new Runtime` / `new SceneCaptureManager` is intentional so hooks never free observer state.

## Settings that already cut load

| Setting | Effect |
| --- | --- |
| Camera frame rate 15–60 | Caps activation pulses and tail-capture interval |
| Single-camera test | Nose only; does not produce a normal two-image PFD |
| Service off / TAXI off | Closes gates, retains the pair, idle observer |
| Auto exposure off | No lighting-driven EV chase; compositor still records if frames arrive |

Raising the rate toward 60 increases observer, capture, and composition work. It cannot bypass pose freshness or GPU completion.

## What this environment did not measure

This workspace is not a Windows MSFS session. The following remain unverified:

- Wall time of a live `observer()` pulse (`probe_cpu_ms` / `stage_ms`)
- Whether live PFD delivery is mostly `copy_patch` or Close-time stamp (`preferred_copy_*` vs `recording_end_draws`)
- Whether capture is copy-path or RT-exit / queue-tail
- Mutex wait time on the render thread during `service()`
- FPS delta TAXI off vs on at 15 and 60
- DLSS vs TAA cost for the owned views (TAA is stable on the A350; DLSS lower-view motion is unresolved and is not a measured FPS comparison)

Local GPU tests (`build.ps1 -Validate`, scene-capture and compositor scripts) check pixels, fences, and reuse. They are not a substitute for those numbers.

## Live measurement plan

Use an already-running session. Do not overwrite calibration or replace the installed DLL while MSFS holds it.

1. Park on the ground, TAA, one supported aircraft, default 15 fps.
2. Record Diagnostics for 30 s with TAXI off (idle observer, hooks still present).
3. TAXI on, same view, 30 s. Note `probe_cpu_ms`, max, `query_ms`, `read_ms`, `aa_ms`, captures, composed, stamps, `preferred_copy_stamps`, `recording_end_draws`, `tail_status`.
4. Repeat at 30 and 60.
5. Repeat with the settings window closed vs open.
6. PIX or the vendor queue viewer: extra `CopyResource` on MSFS lists, private composition queue, fence waits.
7. Compare render-thread time, not only displayed FPS.

A useful split: if `composed` rises and `stamps` does not, delivery admission is the limiter, not capture. If `probe_cpu_ms` jumps with TAXI on, the observer is the limiter. If captures rise on `unknown_source_state` / tail, the RT-exit or tail path is the limiter.

## Remediation order

Change GPU and observer contracts only with a validation receipt and a live note that the item remains unverified in the simulator until observed.

1. **Observe first.** Capture the live counters above so later diffs have a baseline.
2. **Shrink lock hold time.** Cache one graphics/probe snapshot per bridge tick; publish patch descriptors for `copy_patch`; thread-local list cache. No change to what is written on the GPU.
3. **Cut redundant observer work that the contracts already allow.** AA-once per entry; skip unchanged fused stages between gate edges; keep pose on opening pulses.
4. **Slow parked telemetry and logging.** Demand-gate `CameraGet` / on-ground; persistent log handle; event-wait when idle.
5. **GPU path last.** Prefer copy-path capture; consider narrower enhanced sync; skip composition when inputs are identical. Re-run compositor and capture pixel tests after each change.

Do not implement a general "cache last pose for N ms" or "skip patch redraw" without a proof that replayed MSFS copies still show the current composition. Those collide with documented lifetime rules.

## Code index

| Area | Primary files |
| --- | --- |
| Observer / pose / schedule | `src/camera/probe.cpp`, `probe_inspection_gate.hpp`, `render_schedule.hpp`, `local_memory.cpp` |
| Telemetry | `src/camera/body_pose_provider.cpp` |
| Bridge loop / log | `src/bridge/bridge_main.cpp` |
| D3D hooks / PFD | `src/bridge/d3d12_bridge.cpp` |
| Capture / tail / fences | `src/graphics/scene_capture_manager.cpp`, `scene_capture_d3d12.cpp`, `scene_source_state.cpp` |
| Composition / patches | `src/graphics/scene_runtime.cpp`, `scene_frame_output.cpp`, `camera_compositor_d3d12.hpp` |
| PFD stamp / copy | `src/graphics/pfd_stamp_d3d12.cpp`, `write_budget.hpp` |
| IPC | `src/shared/protocol.hpp`, `src/app/companion.cpp` |

Related: [Architecture](architecture.md), [Runtime reference](runtime-reference.md), [Capture manager](scene-capture-manager.md), [Hooks](hooks.md).
