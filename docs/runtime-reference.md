# Runtime reference

Use this page to look up values and diagnose the current runtime. For the explanation of how images move from MSFS cameras to the PFD, start with [Architecture](architecture.md).

## Graphics requirements

The graphics implementation uses standard Direct3D 12 interfaces without NVIDIA- or AMD-specific APIs. Device creation requests feature level `12_0`, and bridge initialization also requires the simulator's device to expose `ID3D12Device10` with the expected native interface identity. The DirectX API version, GPU feature level and available runtime interfaces are distinct requirements. A generic DirectX 12 label is insufficient; keep Windows and graphics drivers current. NVIDIA and AMD are intended targets, but cross-vendor live simulator compatibility has not been established.

Setup checks the presence and x64 headers of required DLLs, but does not load simulator code or probe the GPU. Xbox/MS Store installations can protect executable contents, so setup checks that `FlightSimulator2024.exe` exists; the launcher validates the loaded AMD64 image when connecting. These prerequisite checks do not establish live simulator compatibility.

## Automatic startup

Setup can register Taxi Cam in the simulator's `exe.xml`, or leave startup unchanged when **Launch manually** is selected. The usual Steam path is `%APPDATA%\Microsoft Flight Simulator 2024\exe.xml`; the Microsoft Store path is `%LOCALAPPDATA%\Packages\Microsoft.Limitless_8wekyb3d8bbwe\LocalCache\exe.xml`.

Changing the simulator installation clears an inherited XML choice. An explicitly supplied or edited choice is preserved. Without an explicit path or saved choice for the same simulator, discovery uses a file only when exactly one candidate exists. If both candidates exist, select the intended file; Setup will not guess which one belongs to the simulator.

Setup preserves unrelated add-ons and comments, backs up the original bytes and updates a single Taxi Cam entry. It can repair a recognized launch-only document with a copied `Type="SimConnect"` / `Filename=SimConnect.xml` header. Real or mixed SimConnect configurations, ambiguous headers and globally disabled/manual startup remain refused. Concurrent edits and physical-path redirection are checked before startup can be reported as configured.

When startup cannot be configured and the original file is verified unchanged, installation can finish with a manual-start notice and `setup-diagnostics.log`. Rerun Setup with automatic startup selected to retry. A concurrent edit or uncertain partial write follows the existing rollback checks. Successful XML configuration does not by itself verify that MSFS launches the companion. Start Taxi Cam before loading a flight; see [startup timing](reshade-compatibility.md#startup-timing).

## Settings window controls

| Control | Location and behaviour |
| --- | --- |
| **Keyboard shortcuts…** | **Overview → Flight-deck control**; configure left, right and both-display toggles. Defaults are Ctrl + Shift + L / R / B. |
| **TAXI buttons** | Beside the shortcut editor button; follow cockpit TAXI controls where supported. Disabled for the iniBuilds A380, whose buttons are INOP. Shortcuts work with this setting on or off. |
| **Donate** | Sidebar, above the bug-report icon; opens the PayPal donation page in the default browser. |
| **Report a bug** | Sidebar bug icon or tray menu; opens the GitHub issue form. |
| Version number | Bottom of the sidebar; opens the Taxi Cam GitHub repository in the default browser. |

Saved shortcuts survive upgrades. Use **Reset shortcuts**, then **Save changes** in the editor to adopt the current defaults without changing aircraft calibration. See [Keyboard shortcuts](keyboard-shortcuts.md) for conflicts, button synchronization and session behaviour.

## Settings and files

The companion saves settings to:

~~~text
%LOCALAPPDATA%\Taxi Cam\profiles\<aircraft-key>.ini
~~~

The selected aircraft ID (`profile`) and automatic selection (`automatic`, default 1) are saved in `settings.ini` under `[aircraft]`. The keys are `fbw-a380x`, `ini-a350-900`, `ini-a350-1000` and `ini-a380`; each has its own calibration file. Keyboard combinations are saved separately in `hotkeys.ini` and apply to all aircraft.

Selecting a profile manually turns off **Auto aircraft**. The loaded aircraft must still match before camera or calibration writes are enabled. When a fresh supported identity differs, status names both the detected aircraft and selected profile, and directs manual users to enable **Auto aircraft** or select the matching profile on Overview.

The INI contains `[service]`, `[display]`, `[nose]`, `[tail]` and `[guides]` sections. **Save changes** writes the current adjustments. Loading uses the saved profile first; if no profile exists, it uses that aircraft's defaults. Only the FBW A380 profile imports `taxi-camera-mounts.cfg` beside the companion.

Saves validate the complete settings object, flush a temporary UTF-16 file and replace the INI atomically.

Night-boost preference revision 1 sets `night_boost` to 8 once for each existing profile, including legacy imports, when that profile is first loaded. The migration patches only `night_boost` and `night_boost_revision` in a temporary copy and atomically replaces the profile. Calibration, unknown INI keys, other display preferences and the globally selected aircraft are preserved. New profiles also default to 8. Later user edits retain the revision marker and are respected on subsequent loads. If the migration cannot be saved, 8 applies in memory and persistence is retried on the next load; valid calibration still loads. Installer and ZIP upgrades use this same companion migration.

| Saved field | Default | Range or purpose |
| --- | --- | --- |
| `enabled` | 1 | Enable the camera service |
| `follow_taxi` | 1; iniBuilds A380: 0 | Read aircraft TAXI controls; 0 uses manual control |
| `auto_detect` | 1 | Detect the PFD pair using the profile's policy |
| `camera_rate` | 15 | Integer 5–60, activation limit per camera |
| `single_camera` | 0 | Render only the nose for a performance test |
| `automatic_exposure` | 1 | Adjust exposure from ambient light |
| `exposure` | −11.5 for all aircraft | Daytime EV, −16 to +4 |
| `speed_red`, `speed_green`, `speed_blue` | 22 / 255, 109 / 255, 19 / 255 for all aircraft (`#166D13`) | Normalized RGB, 0–1, edited with the colour picker |
| `night_boost` | 8 | Maximum automatic boost, 0–8 EV; existing profiles migrate once |
| `night_boost_revision` | 1 on save or migration | Per-profile migration marker; not a user control |
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

Side masks are **0 off, 1 left, 2 right, 3 both**. Using a manual preview disables automatic TAXI control in the UI; re-enable **TAXI buttons** on Overview to return to aircraft control. [Keyboard shortcuts](keyboard-shortcuts.md) preserve that setting and synchronize the selected cockpit TAXI buttons where supported. The iniBuilds A380 uses manual control because its cockpit TAXI buttons are INOP.

PFD routing selections apply immediately. Each side may use an explicit current texture ID or **Automatic assignment**. Automatic detection preserves an explicitly selected side and identifies the other from the same confirmed pair. Selecting the same nonzero ID for both sides shows an error and preserves the accepted assignment. Changing aircraft profile starts fresh target discovery; ordinary loss of both resource identities still requires an explicit assignment before side order can be trusted.

Calibration identifies the destination texture using an animated pattern. Switching either calibration target on disables automatic TAXI control; switching the last target off restores **TAXI buttons** automatically on aircraft with working buttons. The iniBuilds A380 remains in manual control. Its write allowance renews every 50 ms in the native adapter. Reaching the limit skips additional pattern writes for that window without disabling calibration.

Source: [settings schema](../src/shared/protocol.hpp), [persistence](../src/app/settings_store.hpp), [write budget](../src/graphics/write_budget.hpp).

## Camera mounts

Each mount stores six values: **right, up, forward, pitch, yaw, lens**.

| Aircraft / view | Right m | Up m | Forward m | Pitch ° | Yaw ° | Lens rad |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| FBW A380 nose | 0 | −1.75 | 26.950668984 | −17.5 | 0 | 1.24 |
| FBW A380 tail | 0 | 18 | −25 | −32 | 0 | 1.02 |
| iniBuilds A380 nose | 0 | 2.2 | 16 | −17.5 | 0 | 1 |
| iniBuilds A380 tail | 0 | 18 | −34 | −32 | 0 | 1 |
| A350-900 nose | 0 | -2 | 16 | -15 | 0 | 0.55 |
| A350-900 tail | 0 | 10 | -33 | -15 | 0 | 0.62 |
| A350-1000 nose | 0 | -2 | 19.81 | -15 | 0 | 0.55 |
| A350-1000 tail | 0 | 10 | -36.17 | -15 | 0 | 0.62 |

Positions are relative to the aircraft datum. Positive pitch looks up; positive yaw turns right. A larger lens value widens the field of view.

The A350-900 mounts use the default visual calibration. The -1000 retains the same height, pitch, yaw and lens with model-specific longitudinal offsets. Its live alignment remains unverified. Saved mounts override the defaults above.

Position components are bounded to ±500 m, pitch to ±89°, yaw to ±180° and lens to 0.05–1.55 radians. Both mounts are saved in the profile.

The mount transform comes from the active aircraft model's scene node. Fresh telemetry provides a separate plausibility guard (within 25 m and 15° after coordinate calibration), without smoothing or driving the mount. A failed model identity, stale generation, changed snapshot or pose disagreement keeps the camera gates closed. Camera controls and saved mount values are unchanged. The A350 views are stable with TAA. Slight lower-view movement while taxiing with DLSS and a brief fixed-fin intrusion remain unresolved.

The integration disables the simulator's per-view AA effect on the two owned taxi views while preserving the main-view AA setting. A required global override, stale owned view or failed flag verification retains the closed camera pair. TAA is stable on the A350; DLSS stability remains incomplete. No general performance comparison between the modes has been established.

Source: [aircraft defaults](../src/profiles/catalog.hpp), [mount transforms](../src/camera/aircraft_mounts.hpp), [scene pose reader](../src/camera/aircraft_scene_pose.hpp).

## Display geometry

Pixel coordinates start at the top left. Row ranges below are inclusive. The table gives the A380 defaults and common working-canvas contract. The A350 uses 1644 x 1024 EFIS targets, initial 774 x 251 / 774 x 496 sources and a 774 x 751 content rectangle inside an 806 x 763 destination within the PFD area, preserving the central separator. See [Aircraft integration](aircraft-profiles.md).

| Element | Geometry |
| --- | --- |
| FBW A380 PFD texture | 768 × 1024, five mips, RGBA8 for automatic detection |
| iniBuilds A380 PFD texture | 768 × 1024, one mip; automatic detection requires the complete eight-display group described in [Aircraft integration](aircraft-profiles.md#inibuilds-a380-configuration) |
| Composed image | 768 × 763 |
| Camera border (all profiles) | 16 target pixels left/right, 12 top, 0 bottom; inner height 751 |
| Nose pane | Working rows 0-254; native source 736 x 251 |
| Tail pane | Working rows 259-762; native source 736 x 496 |
| Visible divider | Black working rows 251-262 (12 pixels) |
| Preserved aircraft display | Rows 763–1023 |
| Ground-speed panel (A380 and A350) | Origin (16, 12); 8-pixel internal padding; height 36; width 96 / 112 for 1 / 2 digits (`--` uses 112) |
| Stable output buffer | Row pitch 3072 bytes; total 2,343,936 bytes |

The logical gap between panes is four working-image rows. The visible divider covers four additional working rows of each pane. The whole 768 x 763 composition, including GS and guides, maps into the inner bordered area; working-image coordinates scale with it. Native source dimensions above match the resulting pane sizes to the nearest pixel.

The **Reference guides** page edits `nose_dot`, `tail_upper`, `tail_corner` and `tail_inner`. Each point is stored in `[guides]` as `<point>_x` and `<point>_y`. Saved coordinates are normalized: X ranges from 0 to 0.5 and Y from 0 to 1; the UI displays these as 0-50% and 0-100%. X is measured from the left edge and Y from the top of the relevant camera pane. The right point mirrors X about the pane centre. Missing keys use that aircraft profile's shipped coordinates.

**Apply live** publishes the current edit without saving. **Save changes** writes it to the active profile. **Reset guide positions** restores only that profile's shipped guide points; camera mounts and display settings are retained. Guide changes take effect on subsequent composed frames using the same scene resources. These positions remain relative to the image and do not automatically track wheels when camera mounts change.

A380 nose markers are filled 14-by-14-pixel magenta squares in the working image. A350 nose dots are amber circles with a 6-pixel radius (12-pixel diameter). Saved centres are retained. Tail brackets use a two-pixel distance threshold. Their positions should be calibrated against the visible tyres at the chosen camera framing before being promoted to shipped defaults.

The GS box uses thin antialiased lettering with two character spaces between the white `GS` label and the coloured speed value. Ground speed is truncated to whole knots (for example, 12.9 displays as 12). Invalid, stale or overflowing values display `--`. The existing 60-knot automatic cutoff controls camera activation independently. A separate parked still-frame hold closes both extra-view gates below 0.5 kt after the first successful pair and resumes at 1.0 kt; see [Architecture](architecture.md#3-position-and-render-the-two-cameras).

Source: [compositor and guides](../src/graphics/camera_compositor_d3d12.hpp), [output buffer](../src/graphics/scene_frame_output.hpp).

## Camera warmup

Warmup requires an enabled, connected service, the current supported aircraft session, valid TAXI and body-pose data, and fresh `SIM ON GROUND` and ground-speed samples confirming on-ground operation at 0–0.5 knots. The on-ground subscription is optional and separate from the body-pose packet: missing data prevents background warmup, without disabling ordinary TAXI requests.

Each session gets one background attempt. Shader preparation and native camera/calibration readiness may take up to a two-minute overall ceiling. Once both views are ready, a separate five-second rendering budget begins. Three new combined frames must complete on the GPU before warmup parks the pair; queued work and output from an earlier session do not count. Foreground activation takes over immediately. Stale or ineligible data parks the attempt until fresh eligible data resumes it, retaining both deadlines and any queued creation request. Failure or timeout ends background work for that session. No display or TAXI state is written by warmup.

`Prewarm phase` reports preparation, rendering, a parked pause or the final outcome, with elapsed time, retained entry IDs, cumulative native creation count, output availability and GPU-completed warmup pairs. Ordinary OFF/ON cycles should reuse the same IDs. The recurring `Camera retention` line also reports `created_total`, owned `snapshot_bytes`, quarantined packet count and the warmup phase. These counters help detect unwanted recreation and snapshot-pool growth; they do not measure all simulator heap or VRAM use.

## PFD-copy diagnostics

The `Camera retention` log includes `patch_requests`, the number of distinct admitted format/size/content slots, and `patch_draws`, the cumulative number of private patch draws recorded during composition. Repeated matching requests reuse a slot. Both remain zero until a validated copy opportunity requests a patch. After admission, every requested slot is refreshed on later compositions so previously recorded copies remain current across profile changes. `patch_draws` includes prepared work that may be discarded; it is not a completed-frame count.

The bridge log reports `PFD copy admission` alongside the normal camera counters. `rt_metadata` counts selected-target RT exits seen in native barrier metadata; `rt_callbacks` counts those admitted by the recording/pass checks. `pending_matches` shows same-recording PFD evidence. `view_resolved` also includes verified selected targets whose earlier draw was recorded elsewhere. `attempts`, `rejected`, `state_skips` and `reason` distinguish missing/conflicting typed-view evidence from output or recording rejection. These are cumulative observations, not completed GPU-frame counts.

The `PFD scope` line records selected RT-exit scope flags (enabled=1, active pass=2, suspended pass=4, invalid recording=8, prior work=16), base/nonbase subresource counts and split barriers. It also reports completed calibration-clear recordings and the automatic/manual/calibration control masks.

The `PFD boundary copy` line counts attempts and completed copy recordings at target changes or command-list closure. `no_proof` and `reason` identify recordings without usable render-target transition evidence; their drawing fallback waits for native DIRECT command-list closure.

The `PFD guarded draw` line reports attempts, successful stamps and query/state refusals separately from texture copies. Its additional fields are:

| Field | Meaning |
| --- | --- |
| `recording_end` | Successful camera draws appended immediately before native DIRECT `Close` |
| `deferred` | Intermediate delivery opportunities held until closure because no safe copy was available |
| `close_forward_refused` | Drawing opportunities rejected because the captured `Close` forward was not a verified D3D12 runtime endpoint |

A drawing attempt requires a verified target, current image, complete state evidence and no open paired query or render pass. `Close` must forward to `D3D12Core.dll`, `d3d12.dll`, or the official `D3D12SDKLayers.dll` debug layer. The bridge binds its own camera pipeline and target without replaying application state afterward. A recording gets at most one closure attempt; only a successful native `Reset` permits another. Independently proved texture copies retain their own admission and resource-state restoration.

Camera composition can succeed while PFD writes are refused. When images exist but no PFD write has yet been recorded, the app reports that it is waiting for a verified write opportunity. These counters report recorded operations; they do not confirm that a frame was presented.

## Exposure

With valid ambient-light data, the exposure controller computes:

~~~text
darkness = clamp(log2(4000 / max(ambient, 1)) / log2(4000), 0, 1)
targetEV = clamp(manualEV + nightBoost * darkness, -16, 4)
~~~

Automatic adjustment moves toward the target at one EV per second. Stale lighting returns the target to manual EV. Ambient uses the SimConnect variable's numeric scale, not lux.

The shader applies exposure and tone mapping to `R11G11B10_FLOAT` camera inputs. Other supported formats use their sampled-colour path.

Source: [exposure controller](../src/graphics/display_exposure.hpp), [colour shader](../src/graphics/camera_compositor_d3d12.hpp).

## IPC and timing

The companion writes settings and a heartbeat; the bridge writes status. Both use a mutex-protected Windows memory mapping named for the MSFS process:

~~~text
Mutex:   Local\380TaxiCamera.Control.<MSFS_PID>
Mapping: Local\380TaxiCamera.Data.<MSFS_PID>
~~~

The header contains `magic`, `version`, `bytes`, `owner_pid` and `owner_heartbeat`. Magic is `0x54415849`, protocol version is `8` and size must equal `sizeof(Shared)`. The payload is the native C++ `Settings` and `Status` layout, so the EXE and DLL must be shipped as a compatible pair. Session-only TAXI requests carry a serial, selected sides and desired states. Status returns fresh button telemetry and request acknowledgement; these commands are never saved in aircraft calibration.

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
| Optional on-ground sampling / freshness | 250 ms / 500 ms |
| Background camera warmup | One attempt per session; up to 5000 ms |
| Ambient-light sampling / freshness | 500 ms / 1500 ms |
| Held TAXI intent after an invalid gap starts | Less than 2000 ms |
| PFD discovery | 1000 ms; three qualifying windows to confirm |
| PFD candidate rows in companion status | 1000 ms; profile changes clear the snapshot |
| Capture-recovery check | 250 ms |
| Diagnostic log snapshot | 5000 ms and each control/scene-stop transition |

Camera scheduling alternates activation pulses and closed intervals. These loop delays and the rate setting do not guarantee completed image FPS.

Capture recovery requires at least two seconds without composition progress, continuing source draws and a qualifying unknown-source-state condition. It permits three retries spaced by two seconds. Ten seconds of healthy capture progress can replenish the retry budget.

The above-60-knots cutoff remains inhibited while an OFF acknowledgement is pending. An accepted push event is not repeated while waiting for that acknowledgement.

Source: [IPC](../src/shared/protocol.hpp), [control loop](../src/bridge/bridge_main.cpp), [recovery policy](../src/camera/scene_recovery.hpp), [speed cutoff](../src/camera/taxi_speed_cutoff.hpp), [still-frame hold](../src/camera/still_frame_hold.hpp).

## Diagnostics

Native camera access uses dynamic instruction and object-identity discovery in the loaded executable. Moved code and data may be accepted when the complete reviewed contract still matches; executable hashes and storefront names are not compatibility gates. Changed instructions, layouts or ambiguous identities keep the camera disabled. A successful discovery check is separate from live rendering validation. See [Dynamic camera compatibility](dynamic-camera-compatibility.md) for the model and its limits.

The bridge writes a status snapshot to the companion and appends metadata to:

~~~text
%LOCALAPPDATA%\Taxi Cam\bridge.log
~~~

Control-transition logs include companion connectivity, cached-read contention count, requested scene state, TAXI validity/grace expiry, scene stop reason and recovery attempts. Output changes and readiness-wait episodes are also logged immediately when observed by the control loop. Camera entry IDs, per-view readiness, draw counts, unknown command lists and invalid recording counts distinguish a control disconnect, temporary pending view and GPU capture-state loss.
When an established camera entry is pending or its inspection changes during a read, the observer retains the pair and waits for fresh validation. It closes only independently validated ready views and makes no pose, resize or activation calls against unavailable views. A timeout does not authorize removing an unavailable camera. Completed images remain subject to resource-generation checks.

A primary-resolution or AA-mode change also retains the pair. Recovery requires the same owned IDs, both render gates observed closed across distinct manager updates, mode2 entries, and existing bitmap dimensions equal to the configured panes. Upscaling can leave different primary render and display sizes in the three camera-size pairs; recovery bounds all six values independently before restoring them to the original pane dimensions. Only the 24 camera-size bytes and projection are restored; no output allocator or entry deletion is called. A refused or partial restoration remains paused, with a restart message instead of repeated recreation. A local regression test covers mixed-size DLSS-to-TAA recovery; live simulator recovery remains unverified.

Initial camera setup also accepts independently bounded render, display and output sizes after its first closed-gate manager update. It still requests three identical size pairs matching the profile pane, rereads the full owned-view chain and checks every field around the exact 24-byte write. An initial resize failure preserves its original cause as `creation_failed`, uses guarded cleanup and does not automatically repeat a possibly partial allocation. It is not reported as retained resolution recovery.

OBS Game Capture's [D3D11On12 capture path](https://github.com/obsproject/obs-studio/blob/master/plugins/win-capture/graphics-hook/d3d12-capture.cpp) copies a wrapped backbuffer on an application queue. A null-buffer [SetPredication call](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication) disables conditional execution and does not invalidate capture evidence. A non-null predicate still invalidates injection for the entire recording, including after a later null call, until a successful native Reset. Hardware and WARP validation exercise the D3D11On12 copy sequence followed by fresh camera capture and both PFD pixel checks; this is separate from live OBS/MSFS validation.


Use the counters in pipeline order:

| Field | Stage represented |
| --- | --- |
| `graphics_ready` | Native graphics observation initialized |
| `scene_ready` | Both engine camera-ready flags are set |
| `captures` | Snapshot copy operations recorded |
| `composed` | Paired compositions submitted |
| `stamps` | Camera copies or draws recorded into PFD command lists |

Private-code mismatch diagnostics identify the failed discovery or validation stage. Relocated code is accepted only when the complete reviewed instruction and object-identity contract has one consistent result; partial or ambiguous matches never authorize native calls.

A counter measures work at its stage, not frames visibly presented. For example, increasing compositions with zero stamps points to display routing or PFD draw eligibility.

The frame-rate setting limits activation opportunities for each camera. Each opening is followed by a closed camera-manager interval. At low simulator update rates, opening and closing can therefore require work on every manager update even at the default 15 setting. The 5 and 10 settings can reduce that workload further, with less frequent camera updates. Settings do not guarantee completed image FPS.

With TAXI on at the default 15, a parked aircraft (fresh ground speed below 0.5 kt) holds both gates closed after the first successful pair. Status reports `Still-frame hold: gates closed; last captured pair retained.` Motion at or above 1.0 kt, a stale/invalid speed sample, TAXI OFF, or mount/recovery/warmup work resumes pulsing. This is intended to drop the two extra MSFS views while parked; it does not change the taxiing 15-fps budget and does not by itself prove a live FPS gain.

Compare render-thread, presented-frame and GPU timings with TAXI off/on at the same cockpit view when investigating stutter; loaded hooks retain essential tracking while OFF, so binary comparisons are also needed to measure changes in their standing overhead.

`probe_ms`, `query_ms` and `read_ms` describe the last serviced inspection callback, not every simulator frame. `inspections` counts serviced callbacks; its change over a log interval gives their frequency. Skipped callbacks leave the last timings visible. `clear_states` counts observed application graphics-state resets. These measurements exclude MSFS scene rendering and GPU time.

For an established pair, manager and pair inspection share one fresh read-only memory transaction. Field values and trace rereads are still checked. Private reads and explicitly enabled hot image reads validate typed allocation identity and requested-page permissions, with full-region fallback for unavailable page information or cache capacity. Image allocations must match the main module exactly. Every recorded proof is revalidated before publication or any native call. Default image readers, startup/code scanners and direct MBI queries retain full-region checks. Explicit image-page reads outside a transaction query fresh metadata each time. No cached metadata or field values carry across writes, engine calls or frames. Lifecycle changes invalidate the provisional result and require fresh inspection.

The log's `queries` and `query_ms` cover actual calls to the instrumented memory metadata APIs, including the fresh AA writable-span check. `allocation_queries`, `page_queries` and `region_queries` distinguish allocation information, page working-set information and legacy `VirtualQueryEx` calls; they sum to `queries`. A page query can validate a batch. Region queries include default image checks and private/image-page fallbacks. The exact AA flag reads also contribute to `read_ms`. These are last serviced callback metrics, not FPS or whole-session averages; they remain stale while inspection is idle. AA requires one private allocation and current `PAGE_READWRITE` protection for the complete 16-byte flag span; its writable check retains no cached proof across the operation.

| Symptom | Inspect |
| --- | --- |
| No bridge status | Companion connection, executable path/structure and bridge startup |
| Empty PFD texture list after starting Taxi Cam inside a loaded flight | The bridge may have missed existing texture/RTV creation. Reload the aircraft or flight with Taxi Cam running so the display resources are recreated; a panel redraw alone does not recover them. If the list stays empty after recreation, investigate display dimensions and formats. |
| Camera compatibility check failed | Reported instruction-discovery, function-boundary, object-identity or activation-data failure; include the launcher and bridge logs in the support report |
| Scenes not ready | Camera lifecycle and fresh aircraft/camera telemetry |
| Scenes ready, zero captures | Scene-to-texture match, source state and queue observation |
| Captures increase, no compositions | Both feeds, current scene identity and GPU completion |
| Compositions increase, no PFD draws | Assigned targets, active side, recording-end/query/state admission and `close_forward_refused` |
| PFD draws increase, wrong display | Target identification, left/right assignment and display layer |
| Camera inhibited below 60 knots | Pending TAXI OFF acknowledgement |

Status also contains the active side mask, target IDs, hook failures, applied exposure and GS. A status GS of −1 means unavailable. The candidate list contains up to 16 IDs, cumulative draw counts, dimensions, mip counts and formats for the selected profile.

A checked-in comparison of this tree against published 0.9.8 is in [version benchmark](version-benchmark.md). It uses these log fields and does not establish displayed FPS.

`probe_cpu_ms` and `probe_max_ms` record elapsed camera-observer callback time, not OS thread-CPU or GPU time. The ten IPC `stage_ms` values remain manager, pool, lifecycle, entries, view 1, view 2, handoff, pose, activation and publication. The separate log field `aa_ms` records recurring AA preparation, including its verification work; initial AA setup remains within lifecycle timing. It does not add an eleventh IPC stage or change the shared-memory layout. Memory-query/read timings overlap the stage that performs them, so these values must not be added as independent costs.

Two developer diagnostics can be enabled in the environment inherited by MSFS before the bridge starts. Both are disabled by default and leave the shared-memory layout unchanged:

- `TAXI_CAM_GRAPHICS_DIAGNOSTICS=1` enables cumulative lookup, cache-hit, registry-lookup and idle-bypass counters in `Graphics work` log records. The observation generation and invalidation count distinguish idle transitions from normal recording resets. These diagnostic counter increments are disabled during ordinary operation.
- `TAXI_CAM_GPU_TIMING=1` enables asynchronous timestamp spans on Taxi Cam's own command lists. `GPU timing` records contain cumulative `samples`, `rejected`, `total_ms` and `max_ms` for private queue-tail capture copies, composition, the output-buffer copy and requested patch preparation. Results are read only after the existing completion fence; diagnostics add no CPU wait for the GPU. Intervals between snapshots use differences in sample counts and totals. Zero samples mean no completed measurement, not zero cost.

These GPU spans exclude MSFS scene rendering, captures inserted into replayable application command lists and application PFD delivery. They are GPU elapsed intervals rather than presented-frame times. No timestamps are inserted into replayable application lists. Existing capture/composition counters still report work events, not displayed FPS. Keep timed and untimed runs separate when assessing instrumentation overhead.

For installation and local build commands, see the [README](../README.md). For automated validation and downloadable assets, see [Releases](releases.md).

### Aircraft identity and assignment diagnostics

Status includes `active_profile`, `detected_profile`, `identity_sample_ms`, `aircraft_type` and `aircraft_path`. The log records identity changes, including paths which do not match a supported adapter. A zero detected profile means unknown, ambiguous or stale identity, never an instruction to use the A380.

The PFD assignment lists refresh when candidate allocation IDs change. Opening a list freezes its contents until it closes, preserving selection while drawing continues. Entries show dimensions, mips and format; their allocation IDs are session-only. Automatic assignment stays automatic unless a texture is explicitly chosen.

After a TAXI telemetry gap exceeds the existing two-second intent grace, output is hidden and render gates are suspended. Existing owned views remain allocated through TAXI OFF, cutoff, service pause and companion disconnection. Aircraft/profile changes suspend and revalidate that pair, clear the old feed and body calibration, and require fresh captures after the new profile is applied. Native identity failures retain their separate guards; a profile change does not authorize camera removal or replacement. Fresh ON resumes that pair; this does not remove native memory/lifetime guards or establish the cause of a simulator crash.

A session change during initial loading can arrive while an empty camera reset is queued. The transition cancels that unmaterialized request only after excluding an in-flight controller operation and confirming no internal owned IDs or manager owner exist. A busy controller leaves the transition pending for the observer; existing or partial camera pairs retain the normal ownership checks. Allocation dimensions are published together with their owned IDs and manager lifetime, so newly created cameras wait for matching evidence before a transition can validate them. Waiting transitions report the live graphics connection and a specific transition message, with periodic log entries, rather than recycling an earlier camera-demand message.
