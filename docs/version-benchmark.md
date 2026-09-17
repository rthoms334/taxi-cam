# 0.9.8 versus `feature/render-performance`

Comparison of published **Taxi Cam 0.9.8** (`v0.9.8-build.38`, `f84be75`) with this tree (`feature/render-performance`, `ad29ab7`). The helper is `tools/benchmark/Compare-TaxiCamVersions.ps1`. It does not install binaries, stop MSFS, or write calibration.

This is not a live simulator frame-time measurement. Extra camera views remain the dominant intended cost.

## Verdict

**No expected performance benefit versus 0.9.8.**

The established-frame hot paths are byte-identical. The only runtime files that differ are the AA / graphics-size restore path. That fix keeps the owned pair after Off / TAA / DLSS; it does not change per-frame memory queries, hooks, capture, or composition when the pair is already live.

Live MSFS FPS, render-thread time, and GPU occupancy were not measured here. Local GPU and UI validation would not establish those numbers either.

0.9.8 already contains the earlier “reduce main-thread memory overhead” work (`dc49446`). Measuring *that* change requires 0.9.5 versus 0.9.8, not this branch versus 0.9.8.

## What actually changed

| Area | Versus 0.9.8 | Effect on a steady TAXI-on session |
| --- | --- | --- |
| `src/camera/local_memory.cpp` | Unchanged | None |
| `src/graphics/metadata_batch_cache.hpp` | Unchanged | None |
| `src/bridge/d3d12_bridge.cpp` | Unchanged | None |
| `src/graphics/scene_runtime.cpp` | Unchanged | None |
| `src/graphics/scene_capture_*.cpp` | Unchanged | None |
| `src/camera/body_pose_provider.cpp` | Unchanged | None |
| `src/camera/probe.cpp` | Restore after a verified size / AA change | Work only on that recovery; idle and established pulses unchanged |
| `src/bridge/bridge_main.cpp` | Re-arm `CaptureProgress` on `stop_sequence`; log formatting | Same 25 ms loop; no new GPU work |

`main` also has release-gating commits after 0.9.8. Those do not ship in the EXE or DLL.

## Isolated timings

The memory-query and metadata-batch tests print diagnostic wall times. Those sources are identical to 0.9.8, so a Windows rerun can only show machine noise.

This host is Linux and cannot compile the pinned Windows toolchain. Isolated timings were not run. On a Windows checkout with `bootstrap.ps1` already applied:

```powershell
./tools/benchmark/Compare-TaxiCamVersions.ps1 -BaselineRef v0.9.8-build.38 -RunIsolated
```

Worktrees and stdout stay under ignored `build/benchmark/`. The installed `%LOCALAPPDATA%\Taxi Cam\app` copies are not touched.

Query-count contracts (196 → 2 VirtualQuery calls, batched metadata lookups) are the regression. Printed milliseconds are not an FPS claim.

## Live Diagnostics

Use two already-running sessions. Do not replace the installed DLL while MSFS holds it.

1. Park on the ground, TAA, one supported aircraft, camera rate 15.
2. Copy `%LOCALAPPDATA%\Taxi Cam\bridge.log` after 30 s TAXI off and 30 s TAXI on for 0.9.8.
3. Repeat with this build after a later install (MSFS closed).
4. Compare:

```powershell
./tools/benchmark/Compare-TaxiCamVersions.ps1 `
  -BaselineLog path\to\0.9.8-bridge.log `
  -CandidateLog path\to\this-build-bridge.log
```

Useful fields: `probe_ms` (last observer callback), `query_ms` / `read_ms` / `aa_ms`, `captured` / `composed` / `stamps` slopes. If `composed` rises and `stamps` does not, delivery admission is the limiter, not capture. Displayed FPS still has to be read from the simulator; these counters exclude MSFS scene GPU time.

The parser fixtures under `tests/diagnostics/fixtures/` (`*.txt`, not live `bridge.log` files) are synthetic. They prove field extraction, not a live improvement.

## Command

```powershell
./tests/diagnostics/benchmark_compare_test.ps1
./tools/benchmark/Compare-TaxiCamVersions.ps1 -BaselineRef v0.9.8-build.38
```

Default baseline is `v0.9.8-build.38`; default candidate is `HEAD`.
