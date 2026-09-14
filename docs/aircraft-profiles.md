# Aircraft integration

**Auto aircraft** on Overview selects a supported profile from public SimConnect metadata. The matcher requires the variant's `ATC TYPE` and an add-on path component in the `AircraftLoaded` response. Two distinct samples must agree before switching. A missing response does not select a default aircraft. Choosing a profile manually turns automatic selection off.

Each profile owns its camera calibration, display colour and exposure settings. Before switching, the companion saves edits to the departing profile and loads the arriving profile's own file. Invalid unfinished input delays a switch rather than discarding edits.

## Profiles

| Profile | Settings key | Display texture | Nose / tail render sizes |
| --- | --- | --- | --- |
| FlyByWire A380X | `fbw-a380x` | 768 x 1024, RGBA8, five mips | 768 x 255 / 768 x 504 |
| iniBuilds A350-900 / ULR | `ini-a350-900` | 1644 x 1024 EFIS surface | 806 x 255 / 806 x 504 |
| iniBuilds A350-1000 | `ini-a350-1000` | 1644 x 1024 EFIS surface | 806 x 255 / 806 x 504 |

A350 package identifiers and geometry were inspected in iniBuilds version 1.2.6. The A350 adapters are undergoing live simulator validation; a passing GPU fixture does not establish aircraft framing or automatic target ordering.

## Controls

| Aircraft | Left / right state | Automatic OFF |
| --- | --- | --- |
| A380 | `L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON`, `L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON` | Corresponding `A32NX.FCU_EFIS_L_TAXI_PUSH` / `R_TAXI_PUSH` event |
| A350 | `L:INI_TAXI_LEFT`, `L:INI_TAXI_RIGHT` | Write zero to the selected latch through public SimConnect |

The installed iniBuilds behavior XML uses each TAXI latch for its button state and lamp. Its input-event setter toggles that latch. An idempotent zero write makes cutoff independent of toggle timing. The other side is not written. Both adapters wait for a fresh OFF acknowledgement. The catalog supplies the speed limit, currently 60 knots for all profiles.

## Display placement

Each side has an explicit destination rectangle. The A380 covers rows 0 through 762 across the PFD. The A350 captain uses columns 0 through 805 of the combined EFIS surface; the first officer uses columns 838 through 1643. Both camera regions are 806 pixels wide, leaving a 32-pixel central gap for the grey separator and its edge padding. The adjacent navigation display and rows 763 through 1023 are preserved. The gap is based on the installed divider artwork; exact cockpit alignment remains subject to live verification. The display arrangement follows the [Airbus ETACS diagram, section 4-1-0](https://www.aircraft.airbus.com/sites/g/files/jlcbta126/files/2024-06/AC_A350_0524.pdf).

The profile defines accepted texture dimensions, mip policy and formats. Resource IDs identify an allocation lifetime, not an aircraft material. The detector ranks activity across three one-second windows and requires a clear pair above other candidates. The side-order rule is profile data; it must be verified in the simulator. `$EFIS_LEFT` / `$EFIS_RIGHT` and A380 material hints are reference labels, not proof of GPU identity. **PFD routing** supports explicit assignment and correction when the heuristic is ambiguous.

## Shared rendering contract

The renderer captures two independently sized scene textures. It composes them into one bounded **768 x 763 working image**, then maps that image into the profile's destination rectangle. This stable GPU buffer is shared infrastructure, not a request to render a full-size simulator view. The A350's 806-pixel sources preserve the destination aspect ratio through composition and presentation.

Profiles supply the pane division, visible separator, reference dot/bracket coordinates and colour. A380 uses magenta marks; A350 uses amber marks from the Airbus diagram. On both A380 and A350, the ground-speed panel is inset from the camera edges, with internal padding and a width that fits the current value. The same layout applies to both PFDs. Panel layout belongs to the aircraft profile. The ground-speed text has its own saved RGB colour, editable on **Display**; changing it does not change exposure or the reference marks. Marks are visual references; adjusting mounts or field of view does not calibrate metric clearance.

## Geometry and settings

Each camera mount supplies right/up/forward metres, pitch/yaw degrees and lens radians. A350-900 and -1000 have separate mount presets and settings files. The presets start from the exterior model's camera-mesh locations, with a forward adjustment for the belly camera. The -900/ULR and longer -1000 keep independent nose and tail mounts; matching the -900 does not validate -1000 framing. **Camera views** adjusts each profile separately. The Airbus planning diagram establishes display placement and guide colour for both variants, but does not establish identical perspective or field of view. Final framing requires a cockpit ETACS reference for the specific variant; passenger entertainment camera images can have a different crop.

Changing profiles stops output and retires owned views through the engine update callback before changing the control subscription or render dimensions. The bridge clears texture routes, button intent, pose calibration and capture history. A new profile cannot inherit another aircraft's saved mounts or stale ON state. The application stores the selected profile separately from each profile's INI file.

## Adding an aircraft

Define an `AircraftProfile` in [the catalog](../profiles/catalog.hpp): accepted aircraft types and add-on path markers, control strategy and variables, texture constraints, side-order rule, destination rectangles, camera dimensions, mounts, composition and speed limit. Rendering, GPU synchronization, exposure and native camera ownership consume these values without aircraft-name branches.

Add profile/settings tests and a GPU fixture that checks both camera regions and every preserved display region. Then verify actual cockpit buttons, texture identity, framing, cutoff and aircraft reload. An aircraft with different control semantics needs a control adapter; an aircraft without two camera views needs a different composition contract.
