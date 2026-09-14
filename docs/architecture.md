# How the camera reaches the PFD

Taxi Cam creates two additional camera views inside MSFS and draws their images into the aircraft's Primary Flight Display (PFD) screen texture. MSFS supplies the scene rendering: aircraft geometry, airport surfaces and lighting. Taxi Cam controls where the cameras look and how their images are presented.

A texture is an image held in GPU memory. MSFS renders each camera into a texture, and the cockpit model displays its PFD using another texture. Taxi Cam connects them by combining the camera images and drawing the result into the PFD texture.

The left and right PFDs share **one nose camera and one tail camera**. Each EFIS TAXI button controls whether its own PFD receives the combined image.

## The complete path

~~~mermaid
flowchart TD
    Button["EFIS TAXI button state"] --> Control["Bridge selects the left or right PFD"]
    Control --> Cameras["MSFS renders nose and tail views"]
    Cameras --> Capture["Bridge captures the GPU images"]
    Capture --> Combine["GPU combines views, guides and ground speed"]
    Combine --> Display["Bridge draws into the enabled PFD texture"]
~~~

There are three interfaces in this path:

| Interface | What Taxi Cam uses it for |
| --- | --- |
| **SimConnect** | Read aircraft data and TAXI state; send TAXI-button events |
| **Internal MSFS camera functions** | Create the extra views and set their position, direction, field of view and render size |
| **Direct3D 12** | Find camera and display textures, copy images, combine them and draw into the PFD |

SimConnect carries data and control events. The camera images come from MSFS's renderer and travel through Direct3D 12.

## What runs where

**The Windows application — `taxi-cam.exe`** runs outside MSFS. It provides the tray icon, settings window and saved aircraft settings. Its connection worker finds the simulator, loads the bridge and exchanges settings and status with it.

**The bridge — `taxi-camera-bridge.dll`** runs inside `FlightSimulator2024.exe`. It needs this position because both the internal camera objects and the simulator's Direct3D resources belong to that process. It manages the camera views, reads SimConnect data and records the GPU work that places images on the PFD.

**The GPU** renders the two MSFS scenes and carries out the bridge's copies and drawing commands. Camera pixels stay in GPU resources. The tray app receives counters and status, not image frames.

The EXE and DLL communicate through a small Windows shared-memory block protected by a mutex. It carries settings, a heartbeat and status. [IPC fields and timing](runtime-reference.md#ipc-and-timing) are documented separately.

## 1. Start and connect to MSFS

The installer adds an `exe.xml` entry that starts the companion in background mode when MSFS launches.

The companion checks the simulator's executable path, Windows user/session and AMD64 executable structure. Different paths are accepted only when Windows identifies them as the same file, allowing the Xbox installation path and its WindowsApps alias to match. It then loads the bridge using Windows `LoadLibraryW` in the simulator process and calls the DLL's `TaxiCameraStart` export. The bridge starts its control worker after the loader has finished.

The bridge sets up observation of Direct3D calls and starts its SimConnect telemetry worker. Camera creation is requested when at least one enabled TAXI side has an assigned PFD, or when the explicit scene test is active.

Closing the settings window hides it. Exiting the companion clears camera delivery. The bridge and its installed hooks stay loaded until MSFS exits because recorded GPU commands may still refer to their resources.

Source: [launcher](../standalone/launcher.hpp), [bridge startup and control loop](../standalone/bridge_main.cpp).

## 2. Read the TAXI buttons and select the displays

The companion can select the aircraft profile automatically. SimConnect supplies the aircraft type and loaded aircraft path; catalog rules match the variant and add-on identity. Two distinct matching samples trigger a switch. The bridge retires the old camera pair before changing subscriptions, geometry or display routing, and the companion loads that aircraft's saved settings. Unknown aircraft remain inactive.

Each aircraft profile supplies one TAXI-state variable for each EFIS panel. The bridge reads these through SimConnect. An ON state requests delivery to that side's PFD; a fresh OFF state clears that request.

The bridge also needs to know which GPU texture represents each PFD. A cockpit screen is rendered into an off-screen texture before the cockpit model displays it. Many simulator textures have similar dimensions, so Taxi Cam observes their draw activity.

For the A380, detection selects candidates with:

- **768 × 1024** pixels;
- **five mip levels** — the texture's smaller-resolution copies;
- **RGBA8** format;
- a consistently dominant pair of draw rates across three one-second windows.

Initial assignment uses the profile's resource-ID ordering rule. This is a heuristic, so **PFD routing** provides identification, explicit assignment and swap controls. The material-name hints in the aircraft profile do not provide guaranteed GPU labels.

Each resource gets an ID for its current lifetime. If one PFD is replaced, routing keeps the surviving side's identity. If both assigned textures disappear, manual reassignment can be required. Texture IDs are never saved between simulator sessions.

Source: [aircraft profile](../profiles/catalog.hpp), [PFD detector](../src/pfd_target_detector.hpp), [side routing](../src/taxi_button_routes.hpp).

## 3. Position and render the two cameras

The bridge calls internal MSFS camera functions to create two scene views. These functions are outside the public camera SDK. Before any private call, the bridge verifies the loaded image structure, 29 required code fingerprints, activation data and manager update pointer. It uses the observed image size and section bounds. Simulator version, timestamp and section count are not allowlists.

Camera operations run during the simulator's observed camera-manager update. The tray app submits requests; it does not manipulate camera objects from its UI thread. The bridge tracks the IDs of the views it creates so that it can update and remove its own pair. A transient inspection failure pauses new camera work. Removal requires a fresh, complete view inspection and a closed render gate observed across distinct manager updates. Pending or unreadable views retain their IDs; they cannot be erased or replaced until validation recovers. The engine handles deferred renderer release after an accepted removal. TAXI OFF, speed cutoff, service pause and companion disconnection close render gates and hide the PFD feed while retaining the pair. A subsequent ON reuses those same owned views. Aircraft/profile changes still require guarded cleanup before selecting the next adapter. Losing GPU capture-state evidence reports a stalled feed; it does not authorize camera removal or recreation. Capture resumes only when ordered GPU observations establish a valid source state again.

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
| Nose | 768 × 255 | Upper pane |
| Tail | 768 × 504 | Lower camera pane |

MSFS renders each view at its pane size. The image does not need to be rendered at the main window's resolution and reduced afterward.

The rate setting limits activation opportunities to **15–60 per camera per second**. Activations alternate between views, with a closed interval after each pulse. Actual image delivery also depends on simulator update cadence, GPU completion and the availability of both images.

Source: [camera integration](../native-camera/probe.cpp), [pose conversion](../native-camera/body_pose_math.hpp), [mounts](../native-camera/aircraft_mounts.hpp), [schedule](../native-camera/render_schedule.hpp).

## 4. Capture rendered images from the GPU

The engine's camera objects identify their output resources. The bridge matches these outputs against textures seen in actual Direct3D calls. A match includes the resource's lifetime and the current camera pair, so a reused memory address cannot accidentally identify an old frame as current.

The bridge observes rendering, texture copies, resource-state changes and queue submissions. A **resource state** describes how the GPU is currently allowed to use a texture, such as rendering into it or copying from it.

A capture is recorded when one of these paths provides enough information:

| Capture path | Where the copy is inserted |
| --- | --- |
| Whole-image copy | After a compatible copy made by MSFS |
| Render-target transition | At a known transition away from rendering into the camera texture |
| End of queue submission | After submitted draws when tracking proves the texture remains a valid render target |

The copy goes into a texture owned by Taxi Cam. This gives the compositor an image whose lifetime it controls while MSFS continues rendering into its own resources.

A **GPU fence** marks completion of submitted work. Taxi Cam waits for the relevant fence and recording-lifetime conditions before using a captured image. If the resource identity or state is unknown, that capture is refused.

Source: [scene/resource matching](../src/scene_handoff.hpp), [capture manager](../src/scene_capture_manager.hpp), [resource-state tracking](../src/scene_source_state.hpp).

## 5. Combine the views and display information

Source dimensions come from the aircraft profile. The compositor takes a completed nose image and a completed tail image from the current camera pair. It draws a **768 × 763** output containing:

- nose view above and tail view below;
- a black horizontal divider;
- two magenta nose reference dots and mirrored tail brackets;
- an opaque ground-speed panel at the top left.

Ground speed is read from SimConnect and rendered by Taxi Cam. It is rounded to whole knots; unavailable data displays `--`. The original PFD's GS text is covered by this panel.

The magenta guides are fixed positions in the image. Changing the camera mount or field of view does not reproject them onto the ground.

Exposure controls operate in the compositor. For the HDR `R11G11B10_FLOAT` camera format, the shader applies the selected exposure, tone mapping and colour encoding. Automatic exposure adjusts the requested EV from ambient-light data, with a gradual transition. It can brighten captured content but cannot supply lighting that MSFS omitted from the scene.

The result is copied into a GPU buffer with a stable address. This buffer is the image source used by PFD drawing commands.

Source: [compositor](../src/camera_compositor_d3d12.hpp), [output buffer](../src/scene_frame_output.hpp), [exposure](../src/display_exposure.hpp).

## 6. Draw the result into the PFD

The bridge observes drawing into the selected PFD texture. After eligible aircraft display draws, it records another draw that places the combined camera image over the upper region.

| PFD region | Content |
| --- | --- |
| Upper 763 rows | Combined camera image, divider, guides and GS |
| Lower 261 rows | Aircraft's existing trim display |

This is why the camera appears on the cockpit's physical screen: the cockpit model samples the texture that Taxi Cam has just updated. The camera is part of the image rendered on the aircraft display.

The bridge saves and restores the graphics state it changes, including the pipeline, shader inputs, viewport and clipping rectangle. It only inserts the draw when enough application state is known to restore it.

### Keeping readers and writers in order

MSFS can record a GPU command list once and execute it again later. Taxi Cam therefore keeps the output buffer's address stable, allowing those commands to read updated image contents.

A shared per-device fence timeline orders output writes and PFD reads, including work submitted on different queues. The output cannot be overwritten while an earlier tracked PFD read still needs it. Resources remain alive while recorded commands can reference them.

Turning off one TAXI side stops further camera draws to that PFD. Normal aircraft drawing restores its display. The other side can continue using the same camera pair. When neither side nor the scene test requires a view, the bridge requests removal of its cameras.

Source: [native graphics adapter](../standalone/d3d12_bridge.cpp), [PFD draw](../src/pfd_stamp_d3d12.hpp), [graphics-state restoration](../src/pfd_stamp_state.hpp), [runtime coordination](../src/scene_runtime.cpp).

## Behaviour during interruptions

The control loop checks companion heartbeat, aircraft telemetry, display identity and capture progress throughout operation.

| Condition | Response |
| --- | --- |
| Ground speed exceeds 60 knots | Inhibit cameras and send TAXI push events to switch active buttons off; wait for OFF acknowledgement |
| TAXI telemetry briefly disappears | Hold the last accepted button state for a bounded interval; continue checking camera-pose freshness separately |
| Camera output changes identity | Discard the old image pairing and wait for current captures |
| Capture stalls while source draws continue in a qualifying state | Request camera recreation, with a bounded retry budget |
| Companion settings mutex is briefly busy | Retain the last validated settings within the existing heartbeat deadline |
| Companion exits or its heartbeat expires | Suppress delivery and request camera stop |
| Changed private code/layout or invalid GPU state | Refuse the affected operation and report the failed check |

Recovery is conditional. It does not infer a valid camera image from a non-null pointer, and a ready flag does not mean a frame has reached the PFD. The [diagnostic reference](runtime-reference.md#diagnostics) explains how to distinguish each stage.

## Aircraft-specific parts

The companion selects **FlyByWire A380X**, **iniBuilds A350-900 / ULR** or **iniBuilds A350-1000**. Each profile supplies aircraft identity, TAXI controls, camera mounts and dimensions, texture constraints, side ordering, display rectangles, composition marks and speed cutoff. Switching profiles retires owned cameras before resetting telemetry, routing and pose calibration. The A350 adapters require live simulator validation in addition to their GPU fixtures.

Windows startup, settings transport, private camera integration and GPU capture are shared components. Adding an aircraft requires its control, display and geometry integration; changing two variable names is not sufficient. See [Aircraft integration](aircraft-profiles.md).

## Runtime limits

- The integration has fixed private-function addresses and layout expectations. Compatible simulator updates are allowed; changed or relocated functions require an updated integration. Fingerprint matches do not guarantee future compatibility or prove every ABI assumption.
- PFD detection uses an activity heuristic and can need manual assignment.
- Camera motion follows received aircraft telemetry; increasing the rate limit does not remove telemetry timing differences.
- Scene content and lighting depend on what MSFS renders for these views.
- Build receipts report automated checks. Hardware rendering and live MSFS behaviour are separate validation scopes.

The 768 x 763 GPU buffer is a common working canvas; final placement comes from each profile. A350 presentation covers the outer PFD half of each combined EFIS texture and preserves the neighbouring ND.

Exact settings, dimensions, timeouts and IPC fields are in the [runtime reference](runtime-reference.md). Build and publication behaviour is in [Releases](releases.md).
