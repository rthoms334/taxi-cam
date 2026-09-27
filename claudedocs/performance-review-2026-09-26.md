# Performance review: iniBuilds A380 at OMDB, 26 September 2026

Branch `feature/taxi-cam-performance-587079`. Evidence comes from the installed 0.9.44 build's `bridge.log` and `bridge.log.1` (process 3372, 16:48–19:00 local). The changes below were validated locally (`build.ps1 -Validate`, hardware and WARP). **None of them has been measured in the simulator yet.**

## 1. What the flight log shows

Each figure is the median of that phase's 5-second hook-timing records. "ms/s" means CPU time per second of wall-clock time; on one thread, 1,000 ms/s would be a whole core.

| Phase | Sim frame rate | Bridge on render thread | Bridge on main thread | Bridge, all threads | Waits on bridge locks |
|---|---|---|---|---|---|
| Cruise, cameras off (above 60 kt) | ~38 fps | ~22 ms/s | ~0.8 ms/s | 60–80 ms/s | 6.7 ms/s; longest single wait 33 ms |
| On the ground, cameras off | 18–26 fps | ~18 ms/s | ~0.8 ms/s | ~60 ms/s | 2.5 ms/s; longest 23 ms |
| Taxi, cameras on (rate 10) | 15–21 fps | ~25 ms/s | ~16 ms/s | 100–200 ms/s | 8.4 ms/s; longest 12.7 ms |

- **Cost per call:**
  - Draw: 0.10–0.15 µs, about 70,000 per second on the render thread.
  - `Reset`: 4.1–4.7 µs, 2,000–4,000 per second.
  - Camera update that opens a view: about 1.5 ms on the main thread.
  - Camera update that closes a view: about 0.4 ms on the main thread.
- **Extra views are the largest cost.** Draw calls rose from about 10,700 per frame with the cameras off to about 14,800 with them on. Each extra view is about 8,000 draws, and one is rendered on half of all frames.
- **The rate setting cannot help at this frame rate.** Every camera pulse is followed by a closed update, so below about 20 fps two feeds pulse on every other frame at any rate of 5 or more. At 18 fps, rate 10 and the old parked floor of 5 behave identically: each camera updates about 4.5 times a second.
- **Where the time goes:** the bridge's own CPU time comes to about 1 ms per frame on each of the main and render threads while the cameras are on. The rest is MSFS rendering the extra views. The lock waits are the part most likely to be felt as hitches, because a single wait can take a whole frame.
- **Camera dropouts:** "Fresh public flight readiness is unavailable" happened four times during taxi. Each time the pair parked for about 5 s.

## 2. The trip wire

The presentation watchdog trips when no frames have been presented **and** simulator frame telemetry has stopped, both for at least 3 s. That means the whole simulator has stopped, not just rendering. It then disarms the cameras; it does not cause the stall.

| Trip | Time | Circumstances |
|---|---|---|
| 1–3 | 13:49–16:48 | Rotated out of the log. Only the counter survived. |
| 4 | 18:01 | In cruise with the cameras off. `session_ready=0`; telemetry went stale for about 10 s; recovered 13 s later. |
| 5 | 18:58 | While the flight was ending, just before the retained cameras were closed for the aircraft change. |

- There was no GPU driver reset around either trip.
- GSX's Couatl process crashed at 18:29, which is unrelated to both.
- Trips at load and exit are expected.
- A 3 s freeze in cruise with the cameras off is most likely the simulator itself. Taxi Cam's own lock waits are bounded (100 µs–5 ms budgets) and are far shorter than 3 s.
- Because the log rotates at 8 MB, earlier watchdog records are lost. Keeping trip records in a separate small log would make future trips traceable.

## 3. Changes on this branch

| Commit | What changed | Where it helps | Expected effect (not yet measured live) |
|---|---|---|---|
| `3adc434` | Draw, barrier and copy hooks read their list's identity from a small per-thread cache, checked against a per-shard change count, instead of taking a shared SRW lock and doing a hash lookup. Hot flags moved off cache lines that other threads write. | Render thread and workers, on every draw | Roughly 10–40 ns saved per draw; at ~70k draws/s, about 1–3 ms/s on the render thread |
| `fbb91f9` | The add-diffuse and depth-stencil output slots, which are diagnostic only, are read at most once a second instead of on every camera pulse. | Main thread | 12 fewer `ReadProcessMemory` calls per open (about 40 µs); about 0.4 ms/s |
| `66364e4` | While idle, `Reset` no longer takes `observation_mutex`. Idle `OMSetRenderTargets` skips the registry lock when an RTV-handle filter proves no tracked view is bound. The capture manager's `successful_reset` only runs its collection sweep when the list held packets. Render-pass invalidations skip the manager lock when no target can be a camera source. Registry flags were separated onto their own cache lines. | Every simulator thread, cameras on or off | Removes most acquisitions of the three global locks behind the lock waits; shorter `Reset` |
| `41188f9` | Parked floor: 2 per camera, range 1–60, one-time migration of the old default of 5. Unused 160-byte per-draw stage clear skipped. | Frames while parked at the gate | At 18 fps, about 20% of frames carry an extra view instead of 50%, roughly 60% fewer extra renders while parked |

Validation: `build.ps1 -Validate` passed on hardware and WARP (receipt version 0.9.48, bridge SHA-256 `01FB7074…`). `smoke-test.ps1` passed on the exact DLL. Nothing is installed.

- **Tests added:**
  - A cross-thread test of the identity cache: 2,000 alternating rounds.
  - A check that holds the registry lock on another thread and requires an idle, untracked binding to finish without waiting.
  - A render-schedule check of the 18 fps finding.
  - Migration tests for the parked floor.

## 4. Levers that need your decision or a live test

### Dynamic tail rate (built after this review, on your go-ahead)
At about 18 fps a lower tail *rate* changes nothing, because pulses are capped by frame count. The saving comes from skipping some of the tail's turns: nose, tail, nose, idle. Each skipped turn consumes a full slot (the pulse and its closed update), which gives about 25% fewer extra renders. The nose keeps its full cadence and the tail runs at about 2.3 updates a second.

| Situation | Cameras |
|---|---|
| Rolling straight below 24 sim fps | Nose priority |
| Heading change of 3°/s or more | Equal at once |
| Straight again | Priority returns after 2 s continuously below 1.5°/s |
| Sim above 27 fps | Equal |
| Parked | 2-per-second floor |
| Telemetry missing or stale | Equal |
| Setting off | Equal |

- **Inputs:** heading comes from the cached telemetry pose; the frame rate is the SIM_FRAME sample rate over 2 s windows. Neither needs new memory reads.
- **PMDG 777:** each wing camera skips every other turn.
- **Where to see and change it:**
  - Setting: `dynamic_tail` in the profile INI, or **Diagnostics → Dynamic tail rate**. This required IPC protocol 16.
  - `bridge.log`: `nose_priority=`, `turn_dps=`, `sim_fps=` and `rate_limit=nose_priority`.
- **Still to check live:** how a 2.3 Hz tail feels on straight segments, and whether 3°/s catches the start of turns early enough.
- **Nose framing.** The saved A380 nose camera is pitched −17.5° with a lens of 1.24 rad (vertical field of view), so it sees 18° above the horizon and about 128° across: the whole OMDB skyline. Pitching down to about −30°, or a lens near 1.0, would cut distant objects and so draw calls per nose render. Mounts can be changed live, so this can be compared without a new build.

### Needs reverse engineering and a live test
- **Far clip.** Taxi Cam sets only the field of view. `update_view` passes three camera floats at +0x5F0, +0x5F4 and +0x5F8 to a callee; two may be the near and far planes. A read-only live sample would show whether a far clip is available.
- **Per-view distance factors** at P+0xA8 and +0xAC. `update_view` recomputes them from the field of view and quality settings, and they may scale landscape and instancing distances.
- **Clouds per camera** (sibling branch). A/B frame time. Turning clouds off also removes their shadowing.

### Considered and not done
- **Direct memory reads under a vectored exception handler** instead of `ReadProcessMemory`: the largest main-thread saving (about 3 µs per read), but a process-wide handler can let another crash handler see an access violation first, and touching a guard page mutates engine state.
- **Lazy per-list `Reset` clearing:** on an X3D CPU the state is L3-resident, so the saving is about 0.1 µs per Reset. It isn't worth a dirty flag in every setter.
- **Merging main-thread reads into spans** (aircraft chain, scene pose, manager identity, entry table): about 0.1 ms per open in total, with substantial test-fixture rework.
- **Turning SSAO, SSR or SSSSS off per view:** already off on taxi views.

## 5. How to measure the effect in the simulator

Use the same airport, gate, weather and traffic for each run.

1. Cameras off for 2 minutes, then cameras on at the gate for 2 minutes.
2. Taxi the same route with the cameras on.
3. Compare in `bridge.log`:
   - `Hook timing:` `self_ms`, `wait100=` (count, total, maximum, expired) and `updates=` (simulator frames);
   - `Hook timing threads:` render-thread and `WinMain` self time;
   - `camera_manager=` µs per update.
4. Optionally use `tools/performance/capture.ps1` with a PresentMon CSV for frame-time percentiles.

The table in section 1 is the baseline to compare against.
