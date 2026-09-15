# How the camera reaches the PFD

Taxi Cam creates two additional camera views inside MSFS and draws their images into the aircraft's Primary Flight Display (PFD) screen texture. MSFS supplies the scene rendering: aircraft geometry, airport surfaces and lighting. Taxi Cam controls where the cameras look and how their images are presented.

A texture is an image held in GPU memory. MSFS renders each camera into a texture, and the cockpit model displays its PFD using another texture. Taxi Cam connects them by combining the camera images and copying or drawing the result into the PFD texture.

The left and right PFDs share **one nose camera and one tail camera**. Each EFIS TAXI button controls whether its own PFD receives the combined image.

## The complete path

~~~mermaid
flowchart TD
    Button["EFIS TAXI button state"] --> Control["Bridge selects the left or right PFD"]
    Control --> Cameras["MSFS renders nose and tail views"]
    Cameras --> Capture["Bridge captures the GPU images"]
    Capture --> Combine["GPU combines views, guides and ground speed"]
    Combine --> Display["Bridge updates the enabled PFD texture"]
~~~

There are three interfaces in this path:

| Interface | What Taxi Cam uses it for |
| --- | --- |
| **SimConnect** | Read aircraft data and TAXI state; send TAXI-button events |
| **Internal MSFS camera functions** | Create the extra views and set their position, direction, field of view and render size |
| **Direct3D 12** | Find camera and display textures, copy images, combine them and copy the result into the PFD |

SimConnect carries data and control events. The camera images come from MSFS's renderer and travel through Direct3D 12.

## What runs where

**The Windows application — `taxi-cam.exe`** runs outside MSFS. It provides the tray icon, settings window and saved aircraft settings. Its connection worker finds the simulator, loads the bridge and exchanges settings and status with it.

**The bridge — `taxi-camera-bridge.dll`** runs inside `FlightSimulator2024.exe`. It needs this position because both the internal camera objects and the simulator's Direct3D resources belong to that process. It manages the camera views, reads SimConnect data and records the GPU work that places images on the PFD.

**The GPU** renders the two MSFS scenes and carries out the bridge's copies and drawing commands. Camera pixels stay in GPU resources. The tray app receives counters and status, not image frames.

The EXE and DLL communicate through a small Windows shared-memory block protected by a mutex. It carries settings, a heartbeat and status. [IPC fields and timing](runtime-reference.md#ipc-and-timing) are documented separately.

## 1. Start and connect to MSFS

The installer adds an `exe.xml` entry that starts the companion in background mode when MSFS launches.

The companion checks the simulator's executable path, Windows user/session and AMD64 executable structure. Different paths are accepted only when Windows identifies them as the same file, allowing the Xbox installation path and its WindowsApps alias to match. It then loads the bridge using Windows `LoadLibraryW` in the simulator process and calls the DLL's `TaxiCameraStart` export. The bridge starts its control worker after the loader has finished.

The bridge sets up observation of Direct3D calls and starts its SimConnect telemetry worker. With the service enabled and fresh data confirming a supported aircraft is on the ground at no more than 0.5 knots, it can prepare the camera pair before the first TAXI press. Warmup renders until the first combined nose/tail frame is available, then closes the render gates and keeps the pair ready. It writes neither PFD images nor TAXI-button state. Display discovery proceeds independently.

Background warmup gets one attempt per aircraft session and a five-second budget. Invalid or stale readiness data, diagnostics, a failure or the budget ending parks the attempt without an automatic retry. An explicit TAXI or scene-test request takes over immediately; it does not wait for background warmup. Only confirmed, assigned display textures receive the image. The explicit scene test prepares cameras without writing a PFD.

Closing the settings window hides it. Exiting the companion clears camera delivery. The bridge and its installed hooks stay loaded until MSFS exits because recorded GPU commands may still refer to their resources.

Source: [launcher](../src/app/launcher.hpp), [bridge startup and control loop](../src/bridge/bridge_main.cpp).

## 2. Read the TAXI buttons and select the displays

The companion can select the aircraft profile automatically. SimConnect supplies the aircraft type and loaded aircraft path; catalog rules match the variant and add-on identity. Two distinct matching samples trigger a switch. The bridge suspends the camera pair before changing subscriptions, geometry or display routing, and the companion loads that aircraft's saved settings. The pair and its output allocations remain attached to the same verified native manager. Unknown aircraft remain inactive.

Each aircraft profile supplies one TAXI-state variable for each EFIS panel. The bridge reads these through SimConnect. An ON state requests delivery to that side's PFD; a fresh OFF state clears that request.

The bridge also needs to know which GPU texture represents each PFD. A cockpit screen is rendered into an off-screen texture before the cockpit model displays it. Many simulator textures have similar dimensions, so Taxi Cam observes their draw activity.

For the A380, detection selects candidates with:

- **768 × 1024** pixels;
- **five mip levels** — the texture's smaller-resolution copies;
- **RGBA8** format;
- a consistently dominant pair of draw rates across three one-second windows.

Initial assignment uses the profile's resource-ID ordering rule. This is a heuristic, so **PFD routing** provides identification, explicit assignment and swap controls. The material-name hints in the aircraft profile do not provide guaranteed GPU labels.

Each resource gets an ID for its current lifetime. If one PFD is replaced, routing keeps the surviving side's identity. If both assigned textures disappear, manual reassignment can be required. Texture IDs are never saved between simulator sessions.

Source: [aircraft profile](../src/profiles/catalog.hpp), [PFD detector](../src/graphics/pfd_target_detector.hpp), [side routing](../src/graphics/taxi_button_routes.hpp).

## 3. Position and render the two cameras

The bridge calls internal MSFS camera functions to create two scene views. These functions are outside the public camera SDK. Before any private call, the bridge verifies the loaded image structure, 29 required code fingerprints, activation data and manager update pointer. It uses the observed image size and section bounds. Simulator version, timestamp and section count are not allowlists.

Camera operations run during the simulator's observed camera-manager update. The tray app submits requests; it does not manipulate camera objects from its UI thread. The bridge tracks the IDs of the views it creates so that it can update and remove its own pair. A transient inspection failure pauses new camera work. Removal requires a fresh, complete view inspection and a closed render gate observed across distinct manager updates. Pending or unreadable views retain their IDs; they cannot be erased or replaced until validation recovers. The engine handles deferred renderer release after an accepted removal. TAXI OFF, speed cutoff, service pause and companion disconnection close render gates and hide the PFD feed while retaining the pair. A subsequent ON reuses those same owned views. Aircraft/profile changes close and revalidate the retained pair before selecting the next adapter; they do not request removal or replacement. Losing GPU capture-state evidence reports a stalled feed; it does not authorize camera removal or recreation. Capture resumes only when ordered GPU observations establish a valid source state again.

For an established camera pair, one bounded read-only transaction inspects the manager identity, view pool, entry table and both views. These adjacent checks share memory-region metadata, never field contents. Metadata queries begin at the containing 64 KiB window when that region covers the requested field; protection or allocation splits fall back to the exact field address. Every field read and trace reread still runs, and each queried region passes a fresh endpoint check before the transaction ends. No cache survives a native engine call or a later frame. Changed ownership or intervening lifecycle work prevents reuse of the provisional pair inspection; the ordinary fresh inspection and refusal checks apply.

A scheduled closing pulse uses a separate inspection contract that validates ownership, the selected view and its render flags without reading camera or output objects. It can only close render gates; opening, pose changes, resizing and image publication still require the full inspection. The native close result and resulting flags are checked again before the closed state is accepted.

### Following the aircraft

SimConnect supplies latitude, longitude, altitude, pitch, bank and true heading. Taxi Cam converts these into an aircraft position and orientation in the scene's world coordinate system. Public camera data and the internal view establish the coordinate calibration.

Each camera has a mount expressed relative to the aircraft datum:

- **Right, up and forward** specify its position in metres.
- **Pitch and yaw** specify where it looks.
- **Lens** sets its field of view.

The aircraft transform is applied to each mount on camera updates. A camera therefore moves and turns with the aircraft while retaining its configured viewpoint. The nose and tail mounts are independently adjustable.

### Rendering at display size

| Camera | Render size | Destination |
| --- | --- | --- |
| Nose | A380: 736 x 251; A350: 774 x 251 | Upper pane inside the black border |
| Tail | A380: 736 x 496; A350: 774 x 496 | Lower camera pane inside the black border |

The initial aircraft selects these render sizes. A pair retains its allocation sizes when the aircraft changes: for example, a pair created for the A380 keeps its 736-pixel width when used by the A350. The compositor scales it into the selected aircraft's display rectangle. This avoids replacing native camera output allocations during a flight change. Both views remain much smaller than the main window.

Changing graphics settings can overwrite an established camera's size fields. The bridge closes both render gates and retains their entry IDs. When a fresh inspection proves both entries are mode2 and their existing output bitmaps still match the pair's original allocation sizes, it restores only the size fields and projection. It neither allocates replacement textures nor recreates the camera pair. Fresh captures are required before the PFD resumes. If the existing outputs or identities cannot be verified, the cameras stay closed and the app reports that MSFS must be restarted.

The rate setting limits activation opportunities to **15–60 per camera per second**. Activations alternate between views, with a closed interval after each pulse. Actual image delivery also depends on simulator update cadence, GPU completion and the availability of both images.

Source: [camera integration](../src/camera/probe.cpp), [pose conversion](../src/camera/body_pose_math.hpp), [mounts](../src/camera/aircraft_mounts.hpp), [schedule](../src/camera/render_schedule.hpp).

## 4. Capture rendered images from the GPU

The engine's camera objects identify their output resources. The bridge matches these outputs against textures seen in actual Direct3D calls. A match includes the resource's lifetime and the current camera pair, so a reused memory address cannot accidentally identify an old frame as current.

The bridge observes rendering, texture copies, resource-state changes and queue submissions. A **resource state** describes how the GPU is currently allowed to use a texture, such as rendering into it or copying from it.

Descriptor-copy observation tracks only render-target and depth-stencil heaps. Other heap types pass straight through without device-identity queries. The registry retains its device interface: the identical pointer proves that identity directly, while alternate interfaces still require matching canonical `IUnknown` identities.

A capture is recorded when one of these paths provides enough information:

| Capture path | Where the copy is inserted |
| --- | --- |
| Whole-image copy | After a compatible copy made by MSFS |
| Render-target transition | At a known transition away from rendering into the camera texture |
| End of queue submission | After submitted draws when tracking proves the texture remains a valid render target |

Legacy barrier metadata is tracked in full for batches of up to 1,048,576 entries. This linear scan does not issue GPU work or dereference resource pointers. Larger or malformed batches invalidate source-state evidence. General camera-capture insertion is limited to 256 entries. Larger complete batches can deliver at most one PFD copy opportunity per selected display, using a linear scan of only those two target identities. Earlier transitions, aliases and uncertain batch metadata prevent insertion.

The copy goes into a texture owned by Taxi Cam. This gives the compositor an image whose lifetime it controls while MSFS continues rendering into its own resources.

A **GPU fence** marks completion of submitted work. Taxi Cam waits for the relevant fence and recording-lifetime conditions before using a captured image. If the resource identity or state is unknown, that capture is refused.

Source: [scene/resource matching](../src/graphics/scene_handoff.hpp), [capture manager](../src/graphics/scene_capture_manager.hpp), [resource-state tracking](../src/graphics/scene_source_state.hpp).

## 5. Combine the views and display information

Source dimensions come from the aircraft profile selected when the pair is created and remain fixed for that pair. The compositor takes a completed nose image and a completed tail image from the current camera pair. It draws a **768 × 763** output containing:

- nose view above and tail view below;
- a 12-pixel black horizontal divider in the composed image;
- nose reference dots and mirrored tail brackets: magenta for the A380, amber for the A350;
- an opaque ground-speed panel inset from the top-left camera edges, with internal padding and a width that fits the current value on both A380 and A350.

Ground speed is read from SimConnect and rendered by Taxi Cam. It is rounded to whole knots; unavailable data displays `--`. The original PFD's GS text is covered by this panel.

The guides use adjustable positions within each camera image. Changing the camera mount or field of view does not move them with the wheels; use the Reference guides page to realign them after changing the framing.

Exposure controls operate in the compositor. For the HDR `R11G11B10_FLOAT` camera format, the shader applies the selected exposure, tone mapping and colour encoding. Automatic exposure adjusts the requested EV from ambient-light data, with a gradual transition. It can brighten captured content but cannot supply lighting that MSFS omitted from the scene.

Private PFD patches are created only when a validated texture-copy opportunity requests an exact pixel format, size and content rectangle. The first request reserves bounded metadata without GPU work; a later composition renders that patch, including its border, image, GS and guides, into a stable GPU buffer. It becomes available only after submission. Without an admitted copy request, composition creates no typed patches and the final command-list drawing path uses the shared image directly.

Every requested patch is refreshed on each later composition, including after profile changes. Published buffers retain their addresses because MSFS can replay previously recorded copies. Matching requests reuse a slot; unsupported formats or geometry do not reserve one.

Source: [compositor](../src/graphics/camera_compositor_d3d12.hpp), [output buffer](../src/graphics/scene_frame_output.hpp), [exposure](../src/graphics/display_exposure.hpp).

Reference-guide positions are saved separately for each aircraft profile. The **Reference guides** settings page provides X/Y controls for the nose dots and the tail brackets’ upper, outside-corner and inner endpoints. Coordinates are percentages of the relevant camera pane: X starts at its left edge and Y at its top. The configured left marker is mirrored to the right. **Apply live** previews edits on subsequent composed camera frames without changing the camera mounts or recreating views; **Save changes** persists them. **Reset guide positions** restores only the selected profile’s shipped marker coordinates.

## 6. Deliver the result to the PFD

The bridge tracks drawing into each selected PFD texture and supports two delivery paths:

- **Texture copy:** copy the prepared patch into the exact profile rectangle, restoring the target's original resource state afterward. The bridge prefers this path at a target change or command-list closure when an explicit render-target transition and a later completed native draw in that recording prove the applicable barrier model. It also supports verified transitions out of render-target state. Copies leave graphics bindings and drawing-query results untouched. Missing or invalidated state evidence prevents this path; a target change or closure alone is insufficient.
- **Draw at command-list closure:** when a copy cannot be proved safe, retain the verified PFD target until MSFS closes that DIRECT command list. At render-target changes and query endings, the bridge records pending display work without inserting a camera draw. Immediately before forwarding native `Close`, it binds the PFD and records the camera draw as final work in that recording. It leaves its own graphics and render-target bindings in place; no application root arguments or other drawing state are replayed afterward. An observed sample pattern is reset to the default required by the single-sample camera pipeline.

The final draw requires a current target identity, valid typed view and dimensions, no intervening target transition, complete tracked graphics state, and a recording outside render passes and all paired GPU queries. It is admitted only when the captured `Close` implementation belongs to `D3D12Core.dll`, `d3d12.dll`, or the official `D3D12SDKLayers.dll` debug layer. An unverified forward blocks this drawing path; independently proved texture copies remain available. Each recording gets one closure attempt, even if native `Close` fails. A successful native `Reset` is required before new drawing can be appended.

Query tracking starts from an observed recording creation or successful native Reset. Unknown, mismatched or overflowing query scopes block camera drawing until a successful Reset. Timestamp queries do not open a paired scope. This prevents camera drawing from adding samples to the simulator's visibility and pipeline-statistics queries.

Both paths exclude the navigation area, central gutter and lower trim display. Calibration uses bounded render-target clears at a verified PFD draw boundary; it does not depend on camera capture or shader drawing.

| PFD region | Content |
| --- | --- |
| Upper 763 rows | Black border around the combined camera image, divider, guides and GS |
| Lower 261 rows | Aircraft's existing trim display |

This is why the camera appears on the cockpit's physical screen: the cockpit model samples the texture that Taxi Cam has just updated. The camera is part of the image rendered on the aircraft display.

Barrier metadata uses one command-list lookup per native batch. A thread-local scope retains the tracking record during the batch; each item still checks its identity and recording generation. Nested batches have separate scopes. A changed or retired recording, a mismatched identity or exhausted scope capacity uses the ordinary lookup path. The scope does not hold the registry lock while processing callbacks.

The native adapter continues tracking command-list lifetimes, target bindings and render boundaries. Hooks forward the exact captured original for their table. A native `ClearState` discards pending overlay work and clears tracked bindings. It does not reset command-list lifetime or revive a recording that was unsafe for injection.

### Keeping readers and writers in order

MSFS can record a GPU command list once and execute it again later. Taxi Cam therefore keeps the output buffer's address stable, allowing those commands to read updated image contents.

A shared per-device fence timeline orders output writes and PFD reads, including work submitted on different queues. The output cannot be overwritten while an earlier tracked PFD read still needs it. Resources remain alive while recorded commands can reference them.

Command-list discovery runs before submission serialization so it cannot acquire the bridge registry while holding the submission lock. Fully observed recordings with no camera packets, PFD reads or camera-source state changes bypass that lock. Unknown and participating recordings still revalidate their metadata under both locks, retain their resource leases and use the shared fence timeline.

Turning off one TAXI side stops recording further camera copies or draws to that PFD. Normal aircraft drawing restores its display. The other side can continue using the same camera pair. When neither side, the scene test nor the bounded startup warmup requires a view, the bridge closes their render gates and retains the camera pair for the next activation. Once the healthy pair is fully idle, it skips periodic private-memory inspection. Resuming or handling pending camera work requires fresh validation before any native camera call.

The retained pair is shared by both displays; toggling TAXI does not allocate another pair. Owned capture storage has a maximum of 16 snapshot packets and a 256 MiB aggregate budget. Up to eight private PFD patch slots reuse matching allocations. Output buffers, pipelines and resources that recorded GPU work may still reference remain allocated until safe release or simulator exit. These bounds cover Taxi Cam storage, not all memory allocated internally by the simulator or driver.

Source: [native graphics adapter](../src/bridge/d3d12_bridge.cpp), [private PFD patch rendering](../src/graphics/pfd_stamp_d3d12.hpp), [runtime coordination](../src/graphics/scene_runtime.cpp).

## Behaviour during interruptions

The control loop checks companion heartbeat, aircraft telemetry, display identity and capture progress throughout operation.

| Condition | Response |
| --- | --- |
| Ground speed exceeds 60 knots | Inhibit cameras and send TAXI push events to switch active buttons off; wait for OFF acknowledgement |
| TAXI telemetry briefly disappears | Hold the last accepted button state for a bounded interval; continue checking camera-pose freshness separately |
| Camera output changes identity | Discard the old image pairing and wait for current captures |
| Capture stalls while source draws continue | Retain the camera pair and wait for fresh, ordered GPU-state evidence |
| Aircraft/profile changes | Hide the feed, close and revalidate the same camera pair, reset routing and pose calibration, then require fresh captures |
| Companion settings mutex is briefly busy | Retain the last validated settings within the existing heartbeat deadline |
| Companion exits or its heartbeat expires | Suppress delivery and close render gates while retaining the pair |
| Changed private code/layout or invalid GPU state | Refuse the affected operation and report the failed check |

Recovery is conditional. It does not infer a valid camera image from a non-null pointer, and a ready flag does not mean a frame has reached the PFD. The [diagnostic reference](runtime-reference.md#diagnostics) explains how to distinguish each stage.

## Aircraft-specific parts

The companion selects **FlyByWire A380X**, **iniBuilds A350-900 / ULR** or **iniBuilds A350-1000**. Each profile supplies aircraft identity, TAXI controls, camera mounts and dimensions, texture constraints, side ordering, display rectangles, composition marks and speed cutoff. Switching profiles closes and revalidates the retained cameras, then resets telemetry, routing and pose calibration. The new aircraft must provide fresh identity and body-pose data before the pair resumes; the PFD waits for new captures. The A350 adapters require live simulator validation in addition to their GPU fixtures.

Windows startup, settings transport, private camera integration and GPU capture are shared components. Adding an aircraft requires its control, display and geometry integration; changing two variable names is not sufficient. See [Aircraft integration](aircraft-profiles.md).

## Runtime limits

- The integration has fixed private-function addresses and layout expectations. Compatible simulator updates are allowed; changed or relocated functions require an updated integration. Fingerprint matches do not guarantee future compatibility or prove every ABI assumption.
- PFD detection uses an activity heuristic and can need manual assignment.
- Camera motion follows received aircraft telemetry; increasing the rate limit does not remove telemetry timing differences.
- Scene content and lighting depend on what MSFS renders for these views.
- Build receipts report automated checks. Hardware rendering and live MSFS behaviour are separate validation scopes.

The 768 x 763 GPU buffer is a common working canvas; final placement comes from each profile. A350 presentation covers the inner PFD area of each combined EFIS texture, preserving the central grey separator, its edge padding and the neighbouring ND.

Exact settings, dimensions, timeouts and IPC fields are in the [runtime reference](runtime-reference.md). Build and publication behaviour is in [Releases](releases.md).
