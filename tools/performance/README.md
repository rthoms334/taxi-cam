# On-demand performance capture

This helper records process and thread user/kernel CPU counters for an explicit MSFS 2024 PID, OS foreground transitions, and fresh Taxi Cam IPC snapshots. It does not inject code, read arbitrary process memory, change settings, move focus, or start/stop the simulator or a trace service. It is not part of the running product.

Build with the repository's pinned compiler, already installed by the usual bootstrap:

```powershell
./tools/performance/build.ps1 -Test
```

The self-tests create only their own process's IPC mapping and mutex. They cover wrong-PID refusal, valid and malformed IPC, stale/future heartbeats, a busy writer, an abandoned mutex, focus departure/return, and CPU identity. No simulator capture runs during tests.

Choose the simulator PID explicitly. Load the intended aircraft and wait until both cameras and display routing are working. Keep aircraft, cockpit view, graphics settings, weather, traffic and motion comparable. After switching to the desired TAXI state, allow it to settle before starting each capture. A previously created, ready parked pair with completed startup warmup is required for an OFF baseline. An unready aircraft, diagnostic scene or calibration mode or unmatched profile is refused. An OFF capture rejects foreground render demand even when missing display assignments make the delivered mask zero, and rejects advancing capture/composition/stamp counters. ON requires those delivery counters to advance. Native render-gate state is not exposed in IPC; these checks do not prove that the engine did no hidden rendering.

```powershell
# Profile IDs: 1 FBW A380; 2 ini A350-900; 3 ini A350-1000; 4 ini A380.
# Masks: 0 OFF; 1 left PFD; 2 right PFD; 3 both PFDs.
./tools/performance/capture.ps1 -SimulatorProcessId 12345 -ExpectedProfile 4 -ExpectedMask 0 -Phase off-before
./tools/performance/capture.ps1 -SimulatorProcessId 12345 -ExpectedProfile 4 -ExpectedMask 3 -Phase on
./tools/performance/capture.ps1 -SimulatorProcessId 12345 -ExpectedProfile 4 -ExpectedMask 0 -Phase off-after
```

`-AllowUnloaded` (mask 0 only) records the **unloaded baseline** for the hook standing-overhead comparison: the bridge DLL must be absent from the simulator process for the whole window (a loaded bridge refuses the capture, one appearing mid-window invalidates it), the companion is optional, and no IPC readiness, mask or delivery-counter validation runs because nothing publishes status. Start the simulator with Auto-connect off, load the same cockpit, capture with `-AllowUnloaded`, then Connect and capture the ordinary OFF window for the pair.

Each command waits five seconds by default so the operator can return to MSFS. It never changes foreground itself. Keep the simulator foreground for the complete interval. Use `-Seconds` (1–120), `-IntervalMilliseconds` (100–1000; default 250), or `-DelaySeconds` (0–30) when needed. Sampling more frequently adds overhead. All comparisons should use the same helper and interval. The 30-second default is a sampling window, not proof that the scene is stable.

Default output is a new ignored `build/performance-captures/` directory. `-OutputDirectory` must name a new directory. Previous evidence is never overwritten. The helper verifies the target executable name, the loaded bridge path/base/size, process creation identity, and the companion identified by IPC. SHA256 hashes identify the three backing files before and after sampling; they are not hashes of relocated process memory. File hashing runs outside the CPU interval. The build receipt also binds the sampler to the protocol/source headers used to compile it; rebuild after they change.

`samples.jsonl` contains metadata, raw samples and a summary. `summary.json` contains process CPU milliseconds per second and thread CPU milliseconds accumulated over observed intervals, keyed by thread ID **and creation time**. One fully occupied CPU corresponds to approximately 1,000 CPU ms/s; values are not normalized across logical processors. Thread descriptions help identify simulator roles, but do not prove that a particular bridge callback executes there. Process totals include short-lived threads that polling may miss; per-thread sums need not reproduce process totals exactly. Counters are sampled sequentially, with individual sample timestamps.

The IPC mapping uses `FILE_MAP_READ`. A zero-time mutex acquisition permits a consistent copy; acquire/release requires mutex synchronization and modify-state rights but never writes the shared mapping. Busy or abandoned locks yield an invalid sample, never cached status. Both companion and bridge heartbeats must be at most five seconds old and not in the future. Settings, owner, aircraft/session identity, readiness and the requested mask must remain valid. Any invalid sample or settings/session change invalidates the capture. Foreground events are observed with a WinEvent hook, including a departure and return between regular samples; changing foreground windows within MSFS also invalidates the interval. This catches OS focus changes, not cockpit-camera changes inside the same window.

Exit codes are 0 for a valid sampling interval, 1 for refusal/setup failure, and 2 for a completed but invalid interval. Inspect the `issues` array before comparing results. Do not silently discard an invalid middle sample or use a refused capture as an OFF baseline. Successful local tests or a valid capture do not establish a performance improvement.

`probe_elapsed_ms`, its maximum and stage values are last serviced **elapsed callback** timings copied from IPC. They can remain unchanged while the observer is idle; they are neither thread CPU nor whole-frame/GPU timings. Fresh status does not make those retained values a new measurement. CPU counters do not provide FPS, GPU duration, frametime percentiles or attribution to individual functions. Use a separate, explicitly authorized ETW/WPR trace when call stacks are needed.

`build/tools/performance/ipc-poll.exe <pid> <seconds> <out.csv> [poll_ms]` is a read-only, lock-free poller of the IPC status block: it never opens the IPC mutex (so it cannot invalidate a concurrent `capture.ps1` window), rejects torn reads by double-read compare, and writes one CSV row per bridge heartbeat with `taxi_mask`, delivery counters, `hook_failures`, `probe_ms` and the ten per-stage timings of the last serviced inspection. Consecutive identical rows are the same inspection. It exists to measure per-inspection observer cost split by opening/closing pulse and to detect one-tick `taxi_mask` dropouts; it is not part of the running product.

An existing completed PresentMon CSV can be attached with `-PresentMonCsv path.csv`. The wrapper copies and hashes only that explicitly supplied file; it never downloads or launches PresentMon. A changing input is flagged. This attachment alone establishes neither timestamp overlap nor the correct simulator swapchain. Check PID, capture time range, dropped events and the CSV's frame definitions independently before calculating FPS or frametime statistics.
