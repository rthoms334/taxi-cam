# Aircraft integration

**Auto aircraft** on Overview selects a supported profile from public SimConnect metadata. The matcher requires an add-on path component in the `AircraftLoaded` response. The FBW A380's own aircraft path identifies that integration; its `ATC TYPE` can be the Airbus brand string `ATCCOM.ATC_NAME AIRBUS.0.text`, not the ICAO code `A388`. The A350 profiles additionally require the variant's type. Type and path must come from the same metadata poll, and two distinct samples must agree before switching. A missing response does not select a default aircraft. Manual selection uses the same aircraft identity checks and turns automatic selection off.

Each profile owns its camera calibration, display colour and exposure settings. Before switching, the companion saves edits to the departing profile and loads the arriving profile's own file. Invalid unfinished input delays a switch rather than discarding edits.

## Profiles

| Profile | Settings key | Display texture | Nose / tail render sizes |
| --- | --- | --- | --- |
| FlyByWire A380X | `fbw-a380x` | 768 x 1024, RGBA8, five mips | 736 x 251 / 736 x 496 |
| iniBuilds A350-900 / ULR | `ini-a350-900` | 1644 x 1024 EFIS surface | 774 x 251 / 774 x 496 |
| iniBuilds A350-1000 | `ini-a350-1000` | 1644 x 1024 EFIS surface | 774 x 251 / 774 x 496 |
| iniBuilds A380 (experimental) | `ini-a380` | 768 x 1024, one mip; supported RGBA/BGRA views | 736 x 251 / 736 x 496 |

A350 package identifiers and geometry were inspected in iniBuilds version 1.2.6. The A350 adapters are undergoing live simulator validation; a passing GPU fixture does not establish aircraft framing or automatic target ordering.

## Controls

| Aircraft | Left / right state | Automatic OFF |
| --- | --- | --- |
| FBW A380 | `L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON`, `L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON` | Corresponding `A32NX.FCU_EFIS_L_TAXI_PUSH` / `R_TAXI_PUSH` event |
| A350 | `L:INI_TAXI_LEFT`, `L:INI_TAXI_RIGHT` | Write zero to the selected latch through public SimConnect |
| iniBuilds A380 | Manual previews or configurable camera hotkeys; cockpit buttons marked INOP | Suppress camera output above the speed limit; no aircraft-variable writes |

The installed iniBuilds A350 behavior XML uses each TAXI latch for its button state and lamp. Its input-event setter toggles that latch. An idempotent zero write makes cutoff independent of toggle timing. The other side is not written. The FBW A380 and A350 adapters wait for a fresh OFF acknowledgement. The manual-only iniBuilds A380 suppresses output without waiting for a cockpit-button acknowledgement. The catalog supplies the speed limit, currently 60 knots for all profiles.

## Display placement

Each side has an explicit destination rectangle. The A380 covers rows 0 through 762 across the PFD. The A350 captain uses columns 0 through 805 of the combined EFIS surface; the first officer uses columns 838 through 1643. Both outer camera regions are 806 pixels wide, leaving a 32-pixel central gap for the grey separator and its edge padding. The adjacent navigation display and rows 763 through 1023 are preserved. The gap is based on the installed divider artwork; exact cockpit alignment remains subject to live verification. The display arrangement follows the [Airbus ETACS diagram, section 4-1-0](https://www.aircraft.airbus.com/sites/g/files/jlcbta126/files/2024-06/AC_A350_0524.pdf).

Inside each outer region, both aircraft draw a black border 16 target pixels wide on the left and right and 12 pixels high at the top. The complete camera image fits within that border, including its GS panel and reference marks. Content is 736 x 751 pixels on A380 and 774 x 751 on A350. This is distinct from the preserved central grey separator.

The profile defines accepted texture dimensions, mip policy and formats. Resource IDs identify an allocation lifetime, not an aircraft material. FBW A380 and A350 detection ranks activity across three one-second windows and requires a clear pair above other candidates. The experimental iniBuilds A380 rule instead checks the complete display group described below. The side-order rule is profile data; it must be verified in the simulator. `$EFIS_LEFT` / `$EFIS_RIGHT` and A380 material hints are reference labels, not proof of GPU identity. **PFD routing** supports explicit assignment and correction when the heuristic is ambiguous.

## Shared rendering contract

The renderer captures two independently sized scene textures. It composes them into one bounded **768 x 763 working image**, then maps that image into the profile's inner content rectangle. This stable GPU buffer is shared infrastructure, not a request to render a full-size simulator view. Native sources match the inner pane sizes: 736 pixels wide on A380 and 774 on A350, with nose and tail heights of 251 and 496 pixels. The visible divider covers working rows 251 through 262, a 12-pixel band; reducing this band leaves the source dimensions and pane positions unchanged. The border is drawn by the existing PFD shader, without an additional GPU pass.

Profiles supply the pane division, visible separator, reference dot/bracket coordinates and colour. A380 uses 14-by-14-pixel magenta nose squares; A350 uses 12-pixel-diameter amber nose circles. Both retain their existing tail brackets. On both A380 and A350, the ground-speed panel is inset from the camera edges, with internal padding and a width that fits the current value. The same layout applies to both PFDs. Panel layout belongs to the aircraft profile. The ground-speed text has its own saved RGB colour, editable on **Display**; changing it does not change exposure or the reference marks. Marks are visual references; adjusting mounts or field of view does not calibrate metric clearance.

## Geometry and settings

Each camera mount supplies right/up/forward metres, pitch/yaw degrees and lens radians. A350-900 and -1000 have separate mount presets and settings files. The presets start from the exterior model's camera-mesh locations, with a forward adjustment for the belly camera. **Camera views** adjusts each profile separately.

The A350-900 defaults are nose **right/up/forward 0 / -2 / 16 m, pitch/yaw -15 / 0 degrees, lens 0.55 rad**; tail **0 / 10 / -33 m, -15 / 0 degrees, 0.62 rad**. Its amber tail guide points are `(0.29, 0.76)`, `(0.26, 0.87)` and `(0.31, 0.87)` for the left upper point, lower corner and inner endpoint, mirrored on the right. All aircraft share the default GS colour RGB **22 / 109 / 19** (`#166D13`).

The A350-1000 retains the same height, pitch, yaw and lens for each camera, with nose forward position **19.81 m** and tail **-36.17 m** for its longer fuselage. Its amber tail guide points are `(0.31, 0.69)`, `(0.29, 0.85)` and `(0.37, 0.855)`. The FBW A380 uses magenta tail guide points `(0.33, 0.64)`, `(0.305, 0.75)` and `(0.365, 0.758)`.

These per-aircraft defaults were promoted from the user's saved local configurations on 2026-09-15. Camera mounts, nose markers and exposure settings already matched those configurations. Existing saved settings still take precedence. FBW A380 and A350 profiles start with TAXI-button control enabled; iniBuilds A380 starts in manual control with both displays off. Guide points are fixed image references; changing camera settings can move the gear relative to them. The geometry check projects the installed iniBuilds A350 1.2.6 `flight_model.cfg` gear contact points; it does not replace a cockpit comparison or establish metric clearance.

Changing profiles hides output and closes the owned views through the engine update callback. After validating the same manager, entry IDs and output resources, it retains that pair while changing the control subscription and camera mounts. Render dimensions stay fixed at the pair's original allocation sizes; the compositor scales into the selected profile's PFD rectangle. The bridge clears texture routes, button intent, pose calibration and capture history. A new profile cannot inherit another aircraft's saved mounts or stale ON state. The application stores the selected profile separately from each profile's INI file.

Flight/aircraft load notifications and changes in simulation running state also trigger this suspended transition, including reloads of the same aircraft. Confirming the current profile again in the dropdown explicitly retries its connection and display discovery. Manual texture IDs and preview/calibration requests belong to one flight and are cleared at the transition; saved mounts, guides and exposure remain intact. Native identity and GPU lifetime checks still apply. If the retained manager, IDs or output allocations cannot be verified, the cameras remain unavailable; a flight-load notification does not authorize abandoning or replacing those objects.

## Adding an aircraft

### iniBuilds A380 configuration

The streamed aircraft was observed on 2026-09-15 reporting `ATC TYPE=Airbus` and an `AircraftLoaded` path under `SimObjects/Airplanes/inibuilds-a380/presets/inibuilds/a380-800_rr_basic/config/aircraft.CFG`. Matching requires its exact product path component; a generic Airbus type or iniBuilds vendor folder alone cannot select this profile. The FBW A380 keeps its separate identity and settings.

Both INOP TAXI buttons showed no visible response when clicked. The A350-name candidate Lvars stayed at zero during the two-minute read-only capture; this does not establish that those variables exist on the A380. The profile therefore neither reads nor writes guessed TAXI variables. Use the companion's left/right preview controls or camera hotkeys. Manual control still obeys aircraft identity, session, service, GPU and speed guards.

A corrected public input inventory returned 1,000 descriptors. A two-minute subscription test detected the captain's PFD brightness adjustment (`AIRLINER_MIP_SIDE_PFD_LEFT`, 100 down to 65 and back), but no input-value notification correlated with the user's clicks on either PFD. The clickable cursor has therefore not established a usable public camera toggle. The earlier diagnostic had ignored inventory response ID 34 and rejected the client's unused trailing descriptor slot; those parser issues are corrected. This result does not rule out a private aircraft interaction.

The display rectangle and initial guide layout started from FBW A380. A parked live capture showed the copied nose mount below ground, producing a large black area inside the raw nose image. Raising that mount revealed the ground; the user then corrected both views and reference markers. The compositor's pane placement and divider were correct in the captured images.

The accepted iniBuilds A380 calibration was promoted to defaults on 2026-09-15: nose **right/up/forward 0 / 2.2 / 16 m, pitch/yaw -17.5 / 0 degrees, lens 1 rad**; tail **0 / 18 / -34 m, -32 / 0 degrees, 1 rad**. The left tail guide points are `(0.34, 0.52)`, `(0.305, 0.65)` and `(0.355, 0.65)`, mirrored on the right. Nose markers remain `(0.14, 0.48)`. Daytime exposure defaults to **-11.5 EV**, with automatic night adjustment enabled. Settings remain independent in `ini-a380.ini`, and saved user calibration takes precedence over defaults. These are accepted visual alignments, not a metric clearance calibration.

Texture admission requires 768 x 1024, exactly one mip and supported RGBA/BGRA views. Two live sessions exposed eight active RGBA8 typeless resources (format 27); the user identified the last allocation as left and the third-last as right in both sessions. The experimental automatic rule requires that complete eight-resource group, unchanged membership and activity on every member across three one-second windows. It permits gaps in resource IDs and does not use fixed IDs. Missing, extra, changed or incompletely tracked resources prevent automatic selection. Structural changes withdraw automatically assigned sides while preserving explicit selections; loss of both identities requires reselecting the aircraft profile or assigning the PFDs manually. Pauses alone do not remove existing identities. The original activity-ranking rule remains in use for FBW A380 and A350. Local detector tests cover both observed inventories, but the new automatic rule still requires a live simulator check. Own-device GPU fixtures check display regions and preserved lower rows; hotkeys, AA, turns, cutoff and reload also need live verification.

### Integration requirements

Define an `AircraftProfile` in [the catalog](../src/profiles/catalog.hpp): accepted aircraft types and add-on path markers, control strategy and variables, texture constraints, side-order rule, destination rectangles and border insets, camera dimensions, mounts, composition and speed limit. Rendering, GPU synchronization, exposure and native camera ownership consume these values without aircraft-name branches.

Add profile/settings tests and a GPU fixture that checks both camera regions and every preserved display region. Then verify actual cockpit buttons, texture identity, framing, cutoff and aircraft reload. An aircraft with different control semantics needs a control adapter; an aircraft without two camera views needs a different composition contract.
