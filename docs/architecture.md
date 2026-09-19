# How the camera reaches the PFD

Taxi Cam creates two additional camera views inside MSFS and draws their images into the aircraft's Primary Flight Display (PFD) screen texture. MSFS supplies the scene rendering: aircraft geometry, airport surfaces and lighting. Taxi Cam controls where the cameras look and how their images are presented.

A texture is an image held in GPU memory. MSFS renders each camera into a texture, and the cockpit model displays its PFD using another texture. Taxi Cam connects them by combining the camera images and copying or drawing the result into the PFD texture.

The left and right PFDs share **one nose camera and one tail camera**. Each EFIS TAXI button or manual camera request controls whether its PFD receives the combined image.

## The complete path

~~~mermaid
flowchart TD
    Button["EFIS TAXI button state"] --> Control["Bridge selects the left or right PFD"]
    Manual["Keyboard shortcuts or manual previews"] --> Control
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

The installer can add an `exe.xml` entry that starts the companion in background mode when MSFS launches, or leave startup unchanged for manual launch.

The companion treats `--simulator` as a hint, not an exclusive lock. It accepts the running `FlightSimulator2024.exe` in the same Windows session and user when that image is the configured file, another discovered MSFS 2024 install (Steam `MSFS2024`/`Limitless` libraries, XboxGames, or the WindowsApps `Microsoft.Limitless_*` package), or a known 2024 layout. XboxGames and WindowsApps still match as one Store file when Windows reports the same identity. If both Steam and Store are running, the configured path wins when it is live; otherwise the lowest PID is chosen and `launcher.log` records the selection. With **Auto-connect** on (the default), it then loads the bridge using Windows `LoadLibraryW` in that process and calls the DLL's `TaxiCameraStart` export. Overview and the tray menu share one **Connect / Disconnect** control. Connect enables camera operation and resets the companion's attach attempt even if an older profile saved `enabled=0`. Each explicit Connect advances the session-only `profile_request` and clears old runtime routes and requests so the bridge repeats discovery and setup with the same aircraft and calibration preferences. Disconnect immediately publishes disabled settings and an expired owner heartbeat, clears temporary requests, and suppresses retries while retaining the Auto-connect preference. Reconnecting preserves the per-process guard against a second LoadLibrary after a remote load may have begun. The bridge starts its control worker after the loader has finished.

The bridge sets up observation of Direct3D calls and starts its SimConnect telemetry worker. When connected, with fresh data confirming a supported aircraft is on the ground at no more than 0.5 knots, it can prepare the camera pair before the first TAXI press. Warmup renders three combined nose/tail frames and waits for their GPU completion, then closes the render gates and keeps the pair ready. It writes neither PFD images nor TAXI-button state. Display discovery proceeds independently.

The bridge treats a new companion owner, a changed setup request, or a heartbeat reconnection as a new setup session. Disconnect closes output and parks retained native views through the existing lifecycle transaction. Connect revalidates those views, resets discovery routes and activity, and restarts capture, output preparation and warmup. Resource incarnations, creation-proven descriptors and outstanding GPU leases remain valid; recovered bindings are relearned during a fresh bounded association window. Existing descriptor aliases cannot close that window early. This preserves native lifetime and synchronization contracts while retrying setup.

Aircraft/profile changes and same-aircraft airport loads or teleports use a full flight-session reset. Public flow begin events immediately invalidate telemetry and block new camera creation, capture, calibration and PFD writes. An invalid WORLD camera sample also gates work before a later public event arrives. Intermediate SimStart notifications cannot clear a pending load. After flight end or return to the menu, FLT_LOADED completes only the file load; FLIGHT_START must also release the ended-flight latch. Completion requires fresh supported aircraft identity, body data and public WORLD camera telemetry; Connect in an already loaded flight can establish that readiness directly without requiring a historical start event.

The native observer closes and retires the exact old camera IDs using the existing manager, view and closed-gate checks. Loading does not authorize removal, and unknown native ownership stays retained until absence can be proved. A reset receipt requires both confirmed empty ownership and fresh public readiness. The control loop selects the telemetry adapter once, waits for its fresh samples, then resumes the matching GPU session and prepares a new pair. Same-profile setup clears samples and calibration while preserving the live flow subscription; stopped/loading sessions also keep listening for completion events. Native work requires the exact flight epoch admitted at startup, so later readiness from a different flight cannot approve it. Discovery routes, activity, recovered view associations, pose calibration and completed-frame state are cleared. Saved user calibration is unchanged. Old recordings and GPU leases keep their replay and fence obligations; their frames cannot publish into the new session, and a fresh native command-list Reset is required for new capture work. No GPU wait or thread suspension is added.

Background warmup gets one attempt per aircraft session. Its five-second rendering budget begins when both native views are ready; preparation and aircraft-loading waits have a separate two-minute overall ceiling. Invalid or stale readiness data and diagnostics immediately park background work. Fresh eligible data can resume that same attempt without recreating the pair or resetting either deadline. A failure or deadline ends the attempt. An explicit TAXI or scene-test request takes over immediately; it does not wait for background warmup. Only confirmed, assigned display textures receive the image. The explicit scene test prepares cameras without writing a PFD.

The first normal launch shows Settings even when MSFS starts the companion with `--background`. After Settings has been shown successfully, later background starts remain in the tray; explicit manual launches still show the window. First-use state belongs to the Windows user and survives updates. UI previews do not consume it. When MSFS closes, the companion stays in the tray so a later session can reconnect without depending on automatic startup.

Closing the settings window hides it. Exiting the companion clears camera delivery. The bridge and its installed hooks stay loaded until MSFS exits because recorded GPU commands may still refer to their resources.

Source: [launcher](../src/app/launcher.hpp), [connection recoverability](../src/app/connection_recoverability.hpp), [bridge startup and control loop](../src/bridge/bridge_main.cpp).

## 2. Read the TAXI buttons and select the displays

The companion can select the aircraft profile automatically. SimConnect supplies the aircraft type and loaded aircraft path; catalog rules match the variant and add-on identity. Two distinct matching samples trigger a switch. The bridge suspends the camera pair before changing subscriptions, geometry or display routing, and the companion loads that aircraft's saved settings. The pair and its output allocations remain attached to the same verified native manager. Unknown aircraft remain inactive.

Telemetry shutdown requests the worker to stop and polls for completion without waiting. If a SimConnect call is still running, its worker, event handles, cached state and selected profile remain owned until it exits. The bridge publishes a waiting status and continues servicing the connection; it neither starts a second worker nor commits the next profile while shutdown is pending. Closing camera render gates does not suspend simulator threads.

FBW A380 and A350 profiles supply one TAXI-state variable for each EFIS panel. The bridge reads these through SimConnect. An ON state requests delivery to that side's PFD; a fresh OFF state clears that request. The iniBuilds A380 has INOP TAXI buttons and uses manual camera requests from configurable keyboard shortcuts or the companion's previews. All requests retain the aircraft, session, service and speed guards.

The companion registers global left, right and both-display shortcuts, initially Ctrl + Shift + L / R / B. They preserve the TAXI-button follow setting and synchronize the selected cockpit buttons on FBW A380 and A350, including OFF requests. The iniBuilds A380 uses manual requests without guessed aircraft-variable writes. Pending aircraft commands require fresh acknowledgement; their state is session-only. See [Keyboard shortcuts](keyboard-shortcuts.md).

The bridge also needs to know which GPU texture represents each PFD. A cockpit screen is rendered into an off-screen texture before the cockpit model displays it. Many simulator textures have similar dimensions, so Taxi Cam observes their draw activity.

Connect remains available in a loaded flight. Native D3D12 observation can recover resources on barrier or copy use, descriptor creation or copies of known descriptors, and a unique render-target entry followed by a single bound view in the same recording. Reconnect resets activity and display routing and rearms the bounded discovery window while preserving native resource identities and GPU lifetime guards.

Live testing has confirmed A350 rendering and the revised A380 rendering path after attachment. Opaque views created before attachment can use the separately proved submission-copy path described below; a resource ID alone still does not authorize calibration or camera writes. Automatic selection remains unreliable on both A380 integrations when displays are booting and has selected other instruments. The bridge uses atomic native D3D12 vtable hooks without creating a D3D11On12 bootstrap device or intentionally suspending simulator threads. Native identity, recording, query, render-pass and GPU synchronization checks continue to govern all output. See [PR 42 validation](pr42-validation.md) for the tested binary and remaining limits.

For the FBW A380, detection selects candidates with:

- **768 × 1024** pixels;
- **five mip levels** — the texture's smaller-resolution copies;
- **RGBA8** format;
- a consistently dominant pair of draw rates across three one-second windows.

Initial assignment uses the profile's resource-ID ordering rule. This is a heuristic, so **PFD routing** provides identification, explicit assignment and swap controls. The material-name hints in the aircraft profile do not provide guaranteed GPU labels.

The A350 variants use the same activity ranking and side ordering on their 1644 × 1024 display candidates. The lower of the two leading draw rates must exceed third place by 25%, while FBW retains its 50% margin. A captured A350 cold-and-dark power-up exposed a third active display that repeatedly broke the old 50% test even though the two working PFDs stayed busiest. Both policies still require three consecutive stable windows, comparable rates for the pair, and fresh activity after new resources appear. Tied or insufficiently separated candidates remain unassigned.

When native draws cannot be observed, A350 ranks submitted render-target exits only within the complete three-surface, one-mip RGBA8 typeless EFIS group. Its five-mip UNORM auxiliary outputs can submit more exits than either EFIS and must not compete under this fallback. Missing, extra, changed or differently formatted EFIS members reset confirmation. The existing 25% lead, three stable windows and side ordering still apply. All supported textures remain in the manual list, and ordinary native-draw ranking is unchanged. The bridge cannot bypass this confirmation by immediately assigning the first two A350 textures when native draw evidence is absent.

The iniBuilds A380 detector checks exactly eight active 768 × 1024, one-mip RGBA8 typeless displays with stable membership across three one-second windows. The discovery loop can also adopt a complete idle group before those activity windows confirm. It selects the highest resource ID for the left PFD and third-highest for the right. This order matched earlier sessions but selected a different instrument in subsequent boot-time testing. Extra, missing or changed members invalidate the automatic selection. See [Aircraft profiles](aircraft-profiles.md#inibuilds-a380-configuration) for its validation limits and manual recovery.

Each resource gets an ID for its current lifetime. If one PFD is replaced, routing keeps the surviving side's identity. If both assigned textures disappear, manual reassignment can be required. Texture IDs are never saved between simulator sessions.

Source: [aircraft profile](../src/profiles/catalog.hpp), [PFD detector](../src/graphics/pfd_target_detector.hpp), [side routing](../src/graphics/taxi_button_routes.hpp).

## 3. Position and render the two cameras

The bridge calls internal MSFS camera functions to create two scene views. These functions are outside the public camera SDK. Before any private call, the bridge dynamically resolves 41 reviewed instruction templates and their code/data relationships within the loaded image. Exact instruction semantics and member offsets are preserved while decoded address operands may move. Constructor-derived class identities, typed vtable-slot relationships, function boundaries, relocation metadata, activation data and the manager update pointer must all agree. Ambiguous or changing evidence prevents native access. Simulator versions, timestamps, storefronts and executable hashes are not allowlists.

Camera operations run during the simulator's observed camera-manager update. The tray app submits requests; it does not manipulate camera objects from its UI thread. The bridge tracks the IDs of the views it creates so that it can update and remove its own pair. A transient inspection failure pauses new camera work. Removal requires a fresh, complete view inspection and a closed render gate observed across distinct manager updates. Pending or unreadable views retain their IDs; they cannot be erased or replaced until validation recovers. The engine handles deferred renderer release after an accepted removal. TAXI OFF, speed cutoff, service pause and companion disconnection close render gates and hide the PFD feed while retaining the pair. A subsequent ON reuses those same owned views. Aircraft/profile changes close and revalidate the retained pair before selecting the next adapter; they do not request removal or replacement. Losing GPU capture-state evidence reports a stalled feed; it does not authorize camera removal or recreation. Capture resumes only when ordered GPU observations establish a valid source state again.

For an established camera pair, one bounded read-only transaction inspects the manager identity, view pool, entry table and both views. Private object reads share allocation identity and metadata for the pages actually requested, never field contents. `QueryVirtualMemoryInformation` supplies allocation identity and bounds; `QueryWorkingSetEx` supplies current protection only for valid resident pages. Unavailable page information falls back to the original `VirtualQueryEx` region checks. Every exact field read and trace reread still runs. Allocation identities and every recorded page protection receive fresh endpoint checks before results are accepted. No cache survives a native engine call or a later frame. Changed ownership or intervening lifecycle work prevents reuse of the provisional pair inspection; the ordinary fresh inspection and refusal checks apply.

The page backend keeps at most 128 page observations and 64 allocation observations. New observations beyond those capacities use the existing 64-region fallback; all earlier proofs remain and must pass endpoint validation. Frequent manager, pose and AA image reads explicitly select page windows. Inside a page transaction, image proofs retain their type, exact main-module allocation, bounds and current requested-page protections alongside the private-object proofs. Outside a transaction, each explicit image-page query obtains fresh metadata without retaining a proof. Startup/code scanners, default image readers and direct MBI queries retain their full-region contract; an active full-region transaction also takes precedence over image-page mode. Cold or unavailable page information uses the original region checks. A page proof deliberately does not reject protection changes on unrelated unread pages elsewhere in the allocation. Exact reads and native lifecycle guards remain mandatory. See [runtime diagnostics](runtime-reference.md#diagnostics) for the query counters and timing limits.

Manager inspection distinguishes unreadable or changing observations from verified identity mismatches. Temporary inspection failures stop publication and queue guarded cleanup; no native call runs in the failed inspection pass. Recreation requires confirmed removal of both old IDs, a fresh pose and fresh manager validation, with at most three retries. An identity refusal remains fatal even if a later inspection is merely unavailable. The UI reports pending recovery or the stopped-camera error instead of continuing to promise camera images. Automated coverage verifies that a temporary pair-inspection failure followed by an unavailable manager remains recoverable; live simulator recovery remains unverified.

A scheduled closing pulse uses a separate inspection contract that validates ownership, the selected view and its render flags without reading camera or output objects. It can only close render gates; opening, pose changes, resizing and image publication still require the full inspection. The native close result and resulting flags are checked again before the closed state is accepted.

### Following the aircraft

Camera mounts use the active aircraft model's scene transform, read in the verified camera-update observer. The current simulator profile identifies the user/controller's generation-checked model node, requires its second node reference to agree, checks the attached model identity, and rereads every observed field before accepting a pose. Native matrix rows map to aircraft right, up and forward with the first row negated.

SimConnect still supplies aircraft identity, speed and a separate position/orientation plausibility check. Public camera data and the internal view establish the coordinate calibration used by that check. The asynchronous telemetry pose is never blended into the scene pose used for the mounts. A missing, changing or mismatched model pose closes the camera render gates while retaining the existing views.

Each camera has a mount expressed relative to the aircraft datum:

- **Right, up and forward** specify its position in metres.
- **Pitch and yaw** specify where it looks.
- **Lens** sets its field of view.

The scene transform is applied to each mount before its render gate opens. The nose and tail mounts remain independently adjustable. This follows the model by setting the owned cameras' world transforms; it does not change native scene parenting. The A350 views are stable with TAA. DLSS can produce slight back-and-forth aircraft movement in the lower view while taxiing, and a fixed-fin element can briefly enter the image. A380 behaviour and the DLSS mechanism remain unverified.

The integration clears only bit31 of each owned view's first flag word, matching the observed `ToggleVpEffectAA` command. It does this with the render gate closed at creation and checks it before each later activation, so a graphics-setting change cannot silently restore it. Both flag words and the global override values are checked around the operation; all other bits remain intact. A changed or globally forced setting leaves the pair closed. A successful write requires fresh owned-view identity and resource checks. The main view and simulator AA setting are unchanged. TAA is stable on the A350; lower-view movement with DLSS remains unresolved. No general performance comparison between TAA and DLSS has been established.

Before the AA operation, a fresh allocation/page check requires the entire 16-byte flag span to belong to one private allocation with current protection exactly `PAGE_READWRITE`. It checks both pages when the span crosses a boundary and falls back to the original committed-region walk when page information is unavailable. This writable proof is never cached across a write or native call. Exact flag reads and override rereads remain in place; their metadata and read costs are included in diagnostics, with AA preparation reported separately as `aa_ms`.

### Rendering at display size

| Camera | Render size | Destination |
| --- | --- | --- |
| Nose | A380: 736 x 251; A350: 774 x 251 | Upper pane inside the black border |
| Tail | A380: 736 x 496; A350: 774 x 496 | Lower camera pane inside the black border |

The aircraft profile selects these render sizes when a fresh pair is created. An aircraft change fully resets the session and retires the previous pair under the native ownership guards before creating the new profile's views. Ordinary TAXI OFF/ON retains their allocations. Both views remain much smaller than the main window.

Changing graphics settings can overwrite an established camera's size fields. The bridge closes both render gates and retains their entry IDs. When a fresh inspection proves both entries are mode2 and their existing output bitmaps still match the pair's original allocation sizes, it restores only the size fields and projection. It neither allocates replacement textures nor recreates the camera pair. Fresh captures are required before the PFD resumes. If the existing outputs or identities cannot be verified, the cameras stay closed and the app reports that MSFS must be restarted.

Recovery accepts independently bounded primary render and display-size pairs when the existing output proves the exact original pane size. A regression test uses a 1695×901 primary render size, 2542×1351 display-size pairs and unchanged 774×251 and 774×496 A350 camera bitmaps. It verifies restoration without allocation or changes outside the size fields. Live simulator recovery remains unverified.

The rate setting limits activation opportunities to **5–60 per camera per second** (minimum **5**), with a default of **10**. The first keep-install of this version writes `camera_rate=10` into existing `settings.ini` and known aircraft profile INIs and stamps `camera_rate_revision=2`; later upgrades keep a user-changed rate. Activations alternate between views, with a closed interval after each pulse. Actual image delivery also depends on simulator update cadence, GPU completion and the availability of both images. At lower simulator update rates, two settings can reach the same scheduling limit; 5 remains available as a lower budget while retaining mandatory closing intervals and avoiding catch-up bursts.

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
- nose reference markers and mirrored tail brackets: 14-by-14-pixel nose squares for both A380 and A350, with magenta as the default marking colour;
- an opaque ground-speed panel inset from the top-left camera edges, with internal padding and a width that fits the current value on both A380 and A350.

Ground speed is read from SimConnect and rendered by Taxi Cam. Its fractional part is discarded: 12.9 knots displays as 12. Unavailable data displays `--`. The original PFD's GS text is covered by this panel. All profiles start with the same green speed colour, RGB 22 / 109 / 19, while saved colour choices remain specific to each profile.

The guides use adjustable positions within each camera image. Changing the camera mount or field of view does not move them with the wheels; use the Reference guides page to realign them after changing the framing.

Exposure controls operate in the compositor. For the HDR `R11G11B10_FLOAT` camera format, the shader applies the selected exposure, tone mapping and colour encoding. Automatic exposure adjusts the requested EV from ambient-light data, with a gradual transition. It can brighten captured content but cannot supply lighting that MSFS omitted from the scene.

Private PFD patches are created only when a validated texture-copy opportunity requests an exact pixel format, size and content rectangle. The first request reserves bounded metadata without GPU work; a later composition renders that patch, including its border, image, GS and guides, into a stable GPU buffer. It becomes available only after submission. Without an admitted copy request, composition creates no typed patches and the final command-list drawing path uses the shared image directly.

Every requested patch is refreshed on each later composition, including after profile changes. Published buffers retain their addresses because MSFS can replay previously recorded copies. Matching requests reuse a slot; unsupported formats or geometry do not reserve one.

Source: [compositor](../src/graphics/camera_compositor_d3d12.hpp), [output buffer](../src/graphics/scene_frame_output.hpp), [exposure](../src/graphics/display_exposure.hpp).

Reference-guide positions and colour are saved separately for each aircraft profile. The **Reference guides** settings page provides X/Y controls for the nose markers and the tail brackets’ upper, outside-corner and inner endpoints, plus a **Marking colour** picker independent of ground-speed colour. Coordinates are percentages of the relevant camera pane: X starts at its left edge and Y at its top. The configured left marker is mirrored to the right. **Apply live** previews position edits on subsequent composed camera frames without changing the camera mounts or recreating views; choosing a colour applies it live. **Save changes** persists them. **Reset guides** restores the selected profile’s shipped marker coordinates and colour.

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

A bounded thread-local weak cache also reuses known command-list metadata across callbacks. Each hit verifies object identity, liveness and recording generation; misses use the registry lock. Weak references do not prolong the lifetime of the tracking record or its graphics resources. Source-draw completion uses one manager operation to validate the targets and record their effects instead of staging and looking them up again.

The native adapter continues tracking command-list lifetimes, target bindings and render boundaries. Hooks forward the exact captured original for their table. A native `ClearState` discards pending overlay work and clears tracked bindings. It does not reset command-list lifetime or revive a recording that was unsafe for injection.

### Keeping readers and writers in order

MSFS can record a GPU command list once and execute it again later. Taxi Cam therefore keeps the output buffer's address stable, allowing those commands to read updated image contents.

A shared per-device fence timeline orders output writes and PFD reads, including work submitted on different queues. The output cannot be overwritten while an earlier tracked PFD read still needs it. Resources remain alive while recorded commands can reference them.

Late Connect can discover display textures without recovering their existing RTV descriptors. A separate submission path then uses fully observed native recordings: it inserts a prepared copy before a list whose first display transition is an explicit render-target exit preceding work that could access its base mip in render-target state, or after an exit list containing only barriers. Timing queries and buffer operations do not access that texture; shader, copy, depth and resolve operations require incompatible states and cannot have accessed the base mip without an earlier transition. Draws, render-target clears, mesh dispatch, render passes and conservative atomic-copy handling still block a new prefix. Every GPU operation continues to exclude a barrier-only tail. The prefix copy restores render-target state before the original list, so cockpit consumers later in that same list see the image. A later writable transition of that display, or a draw that can promote COMMON back to render-target, revokes the prefix: stamping before TAA/DLSS or a second instrument pass lets the native PFD flash through the overlay. That revoked exit still counts as submitted-display activity so autodetection does not lose the completion. Every original recording must pass the lifecycle and render-pass checks; aliases, split barriers, unknown work and suspended/resuming passes still prevent insertion. Up to two owned lists are added to the same native Execute call without changing original order. Packet allocation and retirement stay on the service thread, and the existing GPU timeline retains the copy resources. Valid lower-mip transitions on FBW displays do not change base-mip proof.

Command-list discovery runs before submission serialization so it cannot acquire the bridge registry while holding the submission lock. Fully observed recordings with no camera packets, PFD reads or camera-source state changes bypass that lock, and the queue wrapper releases its submit lock before forwarding those unrelated `ExecuteCommandLists` calls. Contended helpers report their recording effects without waiting for the manager mutex. Only fully observed, unrelated recordings with no source effects or owned GPU work bypass busy manager/submission locks. Source-affecting and unknown recordings, along with recorded GPU readers and writers, retain serialization and resource leases on the shared fence timeline. This is not a general guarantee against blocking in native APIs or other injectors.

Private composition attempts submission admission without waiting. If an application batch is still using the ordering transaction, the never-submitted composition recording is discarded and its input leases remain pending for a later service iteration. Queue contention alone does not fail the output device or block the bridge's discovery/control loop.

Turning off one TAXI side stops recording further camera copies or draws to that PFD. Normal aircraft drawing restores its display. The other side can continue using the same camera pair. When neither side, the scene test nor the bounded startup warmup requires a view, the bridge closes their render gates and retains the camera pair for the next activation. Once the healthy pair is fully idle, it skips periodic private-memory inspection. Resuming or handling pending camera work requires fresh validation before any native camera call.

After warmup, with no scene or calibration demand, graphics callbacks bypass PFD-only state tracking and new capture/PFD work. An observation generation invalidates incomplete PFD recording proofs; reactivation needs an actual successful native Reset before those recordings can supply a new PFD draw. Camera source-state changes and safety invalidations remain observed, so a texture that stays in render-target state can resume without requiring a new transition that MSFS might never emit. Resource/descriptor lifetimes, target bindings and display draw activity also remain observed for discovery, and previously recorded GPU work retains its submission and retirement guards. OFF therefore removes optional per-command work without claiming that a loaded bridge has zero overhead. Startup warmup is unchanged and deliberately counts as active demand.

Companion candidate rows refresh once per second. Their draw-count sorting happens after releasing the registry lock; automatic target detection retains its separate complete-inventory and lifetime checks. Candidate rows are informational, and assignments still validate current resource identity.

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
| Aircraft/profile changes or same-aircraft airport loads | Fully reset the flight session, safely retire old camera IDs, rescan displays and create a fresh pair after load completion and fresh telemetry |
| Companion settings mutex is briefly busy | Retain the last validated settings within the existing heartbeat deadline |
| Companion exits or its heartbeat expires | Suppress delivery and close render gates while retaining the pair |
| Changed private code/layout or invalid GPU state | Refuse the affected operation and report the failed check |

Recovery is conditional. It does not infer a valid camera image from a non-null pointer, and a ready flag does not mean a frame has reached the PFD. The [diagnostic reference](runtime-reference.md#diagnostics) explains how to distinguish each stage.

## Aircraft-specific parts

The companion selects **FlyByWire A380X**, **iniBuilds A350-900 / ULR**, **iniBuilds A350-1000** or **iniBuilds A380**. Each profile supplies aircraft identity, control strategy, camera mounts and dimensions, texture constraints, detection policy, display rectangles, composition marks, exposure and speed cutoff. Switching profiles fully resets the flight session and retires the previous cameras under native ownership guards. The new aircraft must provide fresh identity, body and WORLD camera data before a new pair is created; the PFD waits for new captures. GPU fixtures do not establish live aircraft behaviour.

Windows startup, settings transport, private camera integration and GPU capture are shared components. Adding an aircraft requires its control, display and geometry integration; changing two variable names is not sufficient. See [Aircraft integration](aircraft-profiles.md).

## Runtime limits

- The integration has reviewed instruction and member-layout expectations. Relocated functions and image data are resolved dynamically when their complete relationships still match. Changed instructions or layouts need review; successful discovery does not guarantee future compatibility or replace the ownership, lifetime and execution-context checks.
- PFD detection uses an activity heuristic and can need manual assignment.
- Camera motion uses fresh aircraft scene transforms at scheduled manager updates. Public telemetry remains a separate freshness and plausibility guard; increasing the rate limit does not bypass either requirement.
- Scene content and lighting depend on what MSFS renders for these views.
- Build receipts report automated checks. Hardware rendering and live MSFS behaviour are separate validation scopes.

The 768 x 763 GPU buffer is a common working canvas; final placement comes from each profile. A350 presentation covers the inner PFD area of each combined EFIS texture, preserving the central grey separator, its edge padding and the neighbouring ND.

Exact settings, dimensions, timeouts and IPC fields are in the [runtime reference](runtime-reference.md). Build and publication behaviour is in [Releases](releases.md).
