# Runtime reference

Use this page to look up values and diagnose the current runtime. For the explanation of how images move from MSFS cameras to the PFD, start with [Architecture](architecture.md).

## Settings and files

The companion saves settings to:

~~~text
%LOCALAPPDATA%\Taxi Cam\profiles\<aircraft-key>.ini
~~~

The selected aircraft ID (`profile`) and automatic selection (`automatic`, default 1) are saved in `settings.ini` under `[aircraft]`. The keys are `fbw-a380x`, `ini-a350-900` and `ini-a350-1000`; each has its own calibration file.

The INI contains `[service]`, `[display]`, `[nose]`, `[tail]` and `[guides]` sections. **Save changes** writes the current adjustments. Loading uses the saved profile first; if no profile exists, it uses that aircraft's defaults. Only the A380 profile imports `taxi-camera-mounts.cfg` beside the companion.

Saves validate the complete settings object, flush a temporary UTF-16 file and replace the INI atomically.

| Saved field | Default | Range or purpose |
| --- | --- | --- |
| `enabled` | 1 | Enable the camera service |
| `follow_taxi` | 1 | Read aircraft TAXI controls; 0 uses manual preview |
| `auto_detect` | 1 | Detect the PFD pair from draw activity |
| `camera_rate` | 15 | Integer 15–60, activation limit per camera |
| `single_camera` | 0 | Render only the nose for a performance test |
| `automatic_exposure` | 1 | Adjust exposure from ambient light |
| `exposure` | −8.8 | Manual EV, −16 to +4 |
| `speed_red`, `speed_green`, `speed_blue` | Profile colour | Normalized RGB, 0�1, edited with the colour picker |
| `night_boost` | 4 | Maximum automatic boost, 0–8 EV |
| `calibration_budget` | 4096 | 64–16384 identification-draw batches per window |

Boolean settings use 0 or 1. Numeric values must be finite. The single-camera test does not supply the two images required for the normal PFD composition.

The following controls are session-only and start cleared:

| Field | Purpose |
| --- | --- |
| `manual_mask` | Choose displays for manual preview |
| `calibration_mask` | Choose displays for animated target identification |
| `scene_test` | Run camera scenes independently of PFD assignment |
| `left_id`, `right_id` | Assign current display texture IDs |
| `route_request` | Identify a new explicit assignment request |

Side masks are **0 off, 1 left, 2 right, 3 both**. Using manual preview disables automatic TAXI control in the UI; re-enable **TAXI buttons** on Overview to return to aircraft control.

Calibration identifies the destination texture using an animated pattern. Its write allowance renews every 50 ms in the native adapter. Reaching the limit skips additional pattern writes for that window without disabling calibration.

Source: [settings schema](../standalone/protocol.hpp), [persistence](../standalone/settings_store.hpp), [write budget](../src/write_budget.hpp).

## Camera mounts

Each mount stores six values: **right, up, forward, pitch, yaw, lens**.

| Aircraft / view | Right m | Up m | Forward m | Pitch ° | Yaw ° | Lens rad |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A380 nose | 0 | −1.75 | 26.950668984 | −17.5 | 0 | 1.24 |
| A380 tail | 0 | 18 | −25 | −32 | 0 | 1.02 |
| A350-900 nose | 0 | -2 | 16 | -15 | 0 | 0.55 |
| A350-900 tail | 0 | 10 | -33 | -15 | 0 | 0.62 |
| A350-1000 nose | 0 | -2 | 19.81 | -15 | 0 | 0.55 |
| A350-1000 tail | 0 | 10 | -36.17 | -15 | 0 | 0.62 |

Positions are relative to the aircraft datum. Positive pitch looks up; positive yaw turns right. A larger lens value widens the field of view.

The A350-900 mounts use the accepted calibration. The -1000 retains the same height, pitch, yaw and lens with model-specific longitudinal offsets, and still needs a live check. Saved mounts override the defaults above.

Position components are bounded to ±500 m, pitch to ±89°, yaw to ±180° and lens to 0.05–1.55 radians. Both mounts are saved in the profile.

Source: [aircraft defaults](../profiles/catalog.hpp), [mount transforms](../native-camera/aircraft_mounts.hpp).

## Display geometry

Pixel coordinates start at the top left. Row ranges below are inclusive. The table gives the A380 defaults and common working-canvas contract. The A350 uses 1644 x 1024 EFIS targets, 774 x 251 / 774 x 496 sources and a 774 x 751 content rectangle inside an 806 x 763 destination within the PFD area, preserving the central separator. See [Aircraft integration](aircraft-profiles.md).

| Element | Geometry |
| --- | --- |
| PFD texture | 768 × 1024, five mips, RGBA8 for automatic detection |
| Composed image | 768 × 763 |
| Camera border (both aircraft) | 16 target pixels left/right, 12 top, 0 bottom; inner height 751 |
| Nose pane | Working rows 0-254; native source 736 x 251 |
| Tail pane | Working rows 259-762; native source 736 x 496 |
| Visible divider | Black working rows 251-262 (12 pixels) |
| Preserved aircraft display | Rows 763–1023 |
| Ground-speed panel (A380 and A350) | Origin (16, 12); 8-pixel internal padding; height 36; width 72 / 88 / 104 for 1 / 2 / 3 digits (`--` uses 88) |
| Stable output buffer | Row pitch 3072 bytes; total 2,343,936 bytes |

The logical gap between panes is four working-image rows. The visible divider covers four additional working rows of each pane. The whole 768 x 763 composition, including GS and guides, maps into the inner bordered area; working-image coordinates scale with it. Native source dimensions above match the resulting pane sizes to the nearest pixel.

The **Reference guides** page edits `nose_dot`, `tail_upper`, `tail_corner` and `tail_inner`. Each point is stored in `[guides]` as `<point>_x` and `<point>_y`. Saved coordinates are normalized: X ranges from 0 to 0.5 and Y from 0 to 1; the UI displays these as 0-50% and 0-100%. X is measured from the left edge and Y from the top of the relevant camera pane. The right point mirrors X about the pane centre. Missing keys use that aircraft profile's shipped coordinates.

**Apply live** publishes the current edit without saving. **Save changes** writes it to the active profile. **Reset guide positions** restores only that profile's shipped guide points; camera mounts and display settings are retained. Guide changes take effect on subsequent composed frames using the same scene resources. These positions remain relative to the image and do not automatically track wheels when camera mounts change.

The nose dots have a 4.5-pixel radius; tail brackets use a two-pixel distance threshold in the working image. Their positions should be calibrated against the visible tyres at the chosen camera framing before being promoted to shipped defaults.

Ground speed is rounded to whole knots in the range 0–999. Invalid or stale data displays `--`.

Source: [compositor and guides](../src/camera_compositor_d3d12.hpp), [output buffer](../src/scene_frame_output.hpp).

## PFD-copy diagnostics

The bridge log reports `PFD copy admission` alongside the normal camera counters. `rt_metadata` counts selected-target RT exits seen in native barrier metadata; `rt_callbacks` counts those admitted by the recording/pass checks. `pending_matches` shows same-recording PFD evidence. `view_resolved` also includes verified selected targets whose earlier draw was recorded elsewhere. `attempts`, `rejected`, `state_skips` and `reason` distinguish missing/conflicting typed-view evidence from output or recording rejection. These are cumulative observations, not completed GPU-frame counts.

Camera composition can succeed while PFD copies are refused. When images exist but no PFD copy has yet been recorded, the app reports that it is waiting for a verified copy opportunity.

## Exposure

With valid ambient-light data, the exposure controller computes:

~~~text
darkness = clamp(log2(4000 / max(ambient, 1)) / log2(4000), 0, 1)
targetEV = clamp(manualEV + nightBoost * darkness, -16, 4)
~~~

Automatic adjustment moves toward the target at one EV per second. Stale lighting returns the target to manual EV. Ambient uses the SimConnect variable's numeric scale, not lux.

The shader applies exposure and tone mapping to `R11G11B10_FLOAT` camera inputs. Other supported formats use their sampled-colour path.

Source: [exposure controller](../src/display_exposure.hpp), [colour shader](../src/camera_compositor_d3d12.hpp).

## IPC and timing

The companion writes settings and a heartbeat; the bridge writes status. Both use a mutex-protected Windows memory mapping named for the MSFS process:

~~~text
Mutex:   Local\380TaxiCamera.Control.<MSFS_PID>
Mapping: Local\380TaxiCamera.Data.<MSFS_PID>
~~~

The header contains `magic`, `version`, `bytes`, `owner_pid` and `owner_heartbeat`. Magic is `0x54415849`, protocol version is `4` and size must equal `sizeof(Shared)`. The payload is the native C++ `Settings` and `Status` layout, so the EXE and DLL must be shipped as a compatible pair.

The mapping carries values, IDs and bounded text. It carries no camera pixels or native object pointers. Routine access tries the mutex without blocking. A busy mutex retains the last validated settings only until their original heartbeat expires; a failed read never extends that deadline. Heartbeat age is measured after the read. An abandoned mutex immediately invalidates the bridge cache and clears enable and heartbeat rather than consuming a partial write.

| Operation | Interval or bound |
| --- | --- |
| Companion connection/settings loop | 200 ms delay |
| Startup header/Windows-loader preflight retry | 1 second; at most 60 attempts; no retry after a remote load/start may have begun |
| Bridge control/output-service loop | 25 ms delay |
| Companion heartbeat acceptance | At most 5000 ms old |
| Aircraft identity sampling / freshness | 1000 ms / 3000 ms |
| Aircraft pose and GS updates | SimConnect `SIM_FRAME` |
| GS and TAXI sample freshness | 500 ms |
| Ambient-light sampling / freshness | 500 ms / 1500 ms |
| Held TAXI intent after an invalid gap starts | Less than 2000 ms |
| PFD discovery | 1000 ms; three qualifying windows to confirm |
| Capture-recovery check | 250 ms |
| Diagnostic log snapshot | 5000 ms and each control/scene-stop transition |

Camera scheduling alternates activation pulses and closed intervals. These loop delays and the rate setting do not guarantee completed image FPS.

Capture recovery requires at least two seconds without composition progress, continuing source draws and a qualifying unknown-source-state condition. It permits three retries spaced by two seconds. Ten seconds of healthy capture progress can replenish the retry budget.

The above-60-knots cutoff remains inhibited while an OFF acknowledgement is pending. An accepted push event is not repeated while waiting for that acknowledgement.

Source: [IPC](../standalone/protocol.hpp), [control loop](../standalone/bridge_main.cpp), [recovery policy](../native-camera/scene_recovery.hpp), [speed cutoff](../native-camera/taxi_speed_cutoff.hpp).

## Diagnostics

The bridge writes a status snapshot to the companion and appends metadata to:

~~~text
%LOCALAPPDATA%\Taxi Cam\bridge.log
~~~

Control-transition logs include companion connectivity, cached-read contention count, requested scene state, TAXI validity/grace expiry, scene stop reason and recovery attempts. Output changes and readiness-wait episodes are also logged immediately when observed by the control loop. Camera entry IDs, per-view readiness, draw counts, unknown command lists and invalid recording counts distinguish a control disconnect, temporary pending view and GPU capture-state loss.
When an established camera entry is pending or its inspection changes during a read, the observer retains the pair and waits for fresh validation. It closes only independently validated ready views and makes no pose, resize or activation calls against unavailable views. A timeout does not authorize removing an unavailable camera. Completed images remain subject to resource-generation checks.

A primary-resolution change also retains the pair. Recovery requires the same owned IDs, both render gates observed closed across distinct manager updates, mode2 entries, and existing bitmap dimensions equal to the configured panes. Only the 24 camera-size bytes and projection are restored; no output allocator or entry deletion is called. A refused or partial restoration remains paused, with a restart message instead of repeated recreation.

OBS Game Capture's [D3D11On12 capture path](https://github.com/obsproject/obs-studio/blob/master/plugins/win-capture/graphics-hook/d3d12-capture.cpp) copies a wrapped backbuffer on an application queue. A null-buffer [SetPredication call](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication) disables conditional execution and does not invalidate capture evidence. A non-null predicate still invalidates injection for the entire recording, including after a later null call, until a successful native Reset. Hardware and WARP validation exercise the D3D11On12 copy sequence followed by fresh camera capture and both PFD pixel checks; this is separate from live OBS/MSFS validation.


Use the counters in pipeline order:

| Field | Stage represented |
| --- | --- |
| `graphics_ready` | Native graphics observation initialized |
| `scene_ready` | Both engine camera-ready flags are set |
| `captures` | Snapshot copy operations recorded |
| `composed` | Paired compositions submitted |
| `stamps` | Camera draws recorded into PFD command lists |

Private-code mismatch diagnostics include the failed RVA and byte count. Code that moves also fails the current fixed-address profile; the runtime does not guess a new address or call an unverified match.

A counter measures work at its stage, not frames visibly presented. For example, increasing compositions with zero stamps points to display routing or PFD draw eligibility.

`probe_ms`, `query_ms` and `read_ms` describe the last serviced inspection callback, not every simulator frame. `inspections` counts serviced callbacks; its change over a log interval gives their frequency. Skipped callbacks leave the last timings visible. `clear_states` counts observed application graphics-state resets. These measurements exclude MSFS scene rendering and GPU time.

| Symptom | Inspect |
| --- | --- |
| No bridge status | Companion connection, executable path/structure and bridge startup |
| Camera compatibility check failed | Reported code RVA, image bounds, activation data or manager layout; the camera remains disabled |
| Scenes not ready | Camera lifecycle and fresh aircraft/camera telemetry |
| Scenes ready, zero captures | Scene-to-texture match, source state and queue observation |
| Captures increase, no compositions | Both feeds, current scene identity and GPU completion |
| Compositions increase, no PFD draws | Assigned targets, active side and restorable graphics state |
| PFD draws increase, wrong display | Target identification, left/right assignment and display layer |
| Camera inhibited below 60 knots | Pending TAXI OFF acknowledgement |

Status also contains the active side mask, target IDs, hook failures, applied exposure and GS. A status GS of −1 means unavailable. The candidate list contains up to 16 IDs, cumulative draw counts, dimensions, mip counts and formats for the selected profile.

`probe_cpu_ms` and `probe_max_ms` measure camera-observer CPU time. They exclude engine rendering and GPU time. The ten `stage_ms` values are manager, pool, lifecycle, entries, view 1, view 2, handoff, pose, activation and publication.

For installation and local build commands, see the [README](../README.md). For automated validation and downloadable assets, see [Releases](releases.md).

### Aircraft identity and assignment diagnostics

Status includes `active_profile`, `detected_profile`, `identity_sample_ms`, `aircraft_type` and `aircraft_path`. The log records identity changes, including paths which do not match a supported adapter. A zero detected profile means unknown, ambiguous or stale identity, never an instruction to use the A380.

The PFD assignment lists refresh when candidate allocation IDs change. Opening a list freezes its contents until it closes, preserving selection while drawing continues. Entries show dimensions, mips and format; their allocation IDs are session-only. Automatic assignment stays automatic unless a texture is explicitly chosen.

After a TAXI telemetry gap exceeds the existing two-second intent grace, output is hidden and render gates are suspended. Existing owned views remain allocated through TAXI OFF, cutoff, service pause and companion disconnection. Aircraft/profile changes and native identity failures retain their separate cleanup guards. Fresh ON resumes that pair; this does not remove native memory/lifetime guards or establish the cause of a simulator crash.
