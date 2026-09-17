# 0.9.8 versus `feature/performance-improvements`

Comparison of published **Taxi Cam 0.9.8** (`v0.9.8-build.38`, `f84be75`) with `feature/performance-improvements` (`a7d6c26`). The helper is `tools/benchmark/Compare-TaxiCamVersions.ps1`. It does not install binaries, stop MSFS, or write calibration.

This is not a live simulator frame-time measurement. Extra camera views remain the dominant intended cost when TAXI is on.

## Verdict

**Source changes that can reduce standing overhead exist. A live FPS win versus 0.9.8 is unmeasured.**

This is the performance-work branch (`a7d6c26`: reduce camera overhead and migrate night exposure). It is not the AA restore branch and not the documentation-only audit.

Expected benefit, if any, is largest when cameras are parked. After warmup, PFD-only hook work is skipped, command-list lookups can hit a thread-local cache, and companion candidate rows refresh once per second. With TAXI on at the default 15, a fresh ground-speed sample below 0.5 kt now holds both extra-view gates closed after the first successful pair; rendering resumes at 1.0 kt. That is the intended ON-path cut. While taxiing above that resume threshold, MSFS still renders the two extra views at the selected rate. Lowering the rate to 5 or 10 can cut that moving-activation work; it is a setting, not an automatic gain. Live occupancy of the extra views remains unmeasured on this host.

Optional `TAXI_CAM_GPU_TIMING=1` and `TAXI_CAM_GRAPHICS_DIAGNOSTICS=1` add instrumentation. Keep those off for an A/B against 0.9.8.

Night-boost migration to 8 EV is a preference change, not a frame-time change.

## What actually changed versus 0.9.8

| Area | Change | Likely effect |
| --- | --- | --- |
| `src/bridge/d3d12_bridge.cpp` | Idle observation generation; skip PFD-only setters/draws; thread-local list cache | Lower render-thread lock traffic when OFF after warmup |
| `src/bridge/bridge_main.cpp` | Demand-gate graphics observation; PFD inventory at 1 Hz | Less registry work on the 25 ms loop when idle |
| `src/graphics/scene_capture_manager.cpp` | Combined source-draw observe; capture enable; optional GPU timestamps | Fewer manager lookups per draw; timestamps off by default |
| `src/graphics/scene_runtime.cpp` / `scene_frame_output.cpp` | Optional private-list GPU timing | Measurement only unless `TAXI_CAM_GPU_TIMING=1` |
| `src/camera/render_schedule.hpp` | Rate floor 5 instead of 15 | User can cut activation pulses |
| `src/camera/still_frame_hold.hpp` / `probe.cpp` | Parked TAXI-on hold after first pair | Extra MSFS views can drop while GS < 0.5 kt |
| `src/app/companion.cpp` | Rate UI 5–60 | Same |
| `src/camera/local_memory.cpp` | Unchanged | No observer-memory change |
| `src/camera/body_pose_provider.cpp` | Unchanged | Parked SimConnect cadence unchanged |
| `src/graphics/scene_capture_d3d12.cpp` | Unchanged | Application-list copy path unchanged |
| Night boost default / migration | 4 → 8 EV once | Display preference only |

`main` after 0.9.8 also has release-gating commits. Those do not ship in the EXE or DLL.

## Isolated timings

`local_memory` sources match 0.9.8. A Windows rerun of those tests can only show machine noise. The hook cache and idle-bypass work is not exercised by that suite.

This host is Linux and cannot compile the pinned Windows toolchain. Isolated timings were not run. A connected Windows worker (`taxi-cam-pc`) is the intended host for those timings and for a parked TAXI-on `bridge.log` sample; this Linux run could not attach a subagent to that worker. On a Windows checkout with `bootstrap.ps1` already applied:

```powershell
./tools/benchmark/Compare-TaxiCamVersions.ps1 -BaselineRef v0.9.8-build.38 -CandidateRef origin/feature/performance-improvements -RunIsolated
```

Worktrees stay under ignored `build/benchmark/`. Installed `%LOCALAPPDATA%\Taxi Cam\app` copies are not touched.

## Live measurement

Use two already-running sessions. Do not replace the installed DLL while MSFS holds it. Leave GPU timing and graphics diagnostics unset.

1. Park on the ground, TAA, one supported aircraft, camera rate 15.
2. Capture 30 s TAXI off and 30 s TAXI on with 0.9.8, then with this build after a later install (MSFS closed).
3. Repeat at rate 5 if you want the new lower-rate path.
4. Compare `bridge.log` snapshots:

```powershell
./tools/benchmark/Compare-TaxiCamVersions.ps1 `
  -BaselineLog path\to\0.9.8-bridge.log `
  -CandidateLog path\to\this-build-bridge.log
```

The branch also ships `tools/performance/capture.ps1` for PID-explicit CPU/IPC samples. Use that for process CPU ms/s; use `bridge.log` for `probe_ms` and capture/compose/stamp slopes. Displayed FPS still has to come from the simulator or an attached PresentMon CSV. Neither helper proves GPU occupancy of the extra views.

Useful `bridge.log` fields: `probe_ms`, `query_ms`, `read_ms`, `aa_ms`, `captured` / `composed` / `stamps`. If `composed` rises and `stamps` does not, delivery admission is the limiter. Parser fixtures under `tests/diagnostics/fixtures/` are synthetic.

## Command

```powershell
./tests/diagnostics/benchmark_compare_test.ps1
./tools/benchmark/Compare-TaxiCamVersions.ps1 -BaselineRef v0.9.8-build.38 -CandidateRef origin/feature/performance-improvements
```

Default baseline is `v0.9.8-build.38`; default candidate is `HEAD`.
