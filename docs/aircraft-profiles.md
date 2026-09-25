# Aircraft integration

**Auto aircraft** on Overview selects a supported profile from public SimConnect metadata. The matcher requires an add-on path component in the `AircraftLoaded` response. The FBW A380's own aircraft path identifies that integration; its `ATC TYPE` can be the Airbus brand string `ATCCOM.ATC_NAME AIRBUS.0.text`, not the ICAO code `A388`. The A350 profiles additionally require the variant's type. Type and path must come from the same metadata poll, and two distinct samples must agree before switching. A missing response does not select a default aircraft. Manual selection uses the same aircraft identity checks and turns automatic selection off.

Each profile owns its camera calibration, display colour and exposure settings. Before switching, the companion saves edits to the departing profile and loads the arriving profile's own file. Invalid unfinished input delays a switch rather than discarding edits.

## Profiles

| Profile | Settings key | Display texture | Camera render sizes |
| --- | --- | --- | --- |
| FlyByWire A380X | `fbw-a380x` | 768 x 1024, RGBA8, five mips | Nose 736 x 251 / tail 736 x 496 |
| iniBuilds A350-900 / ULR | `ini-a350-900` | 1644 x 1024 EFIS surface | Nose 774 x 251 / tail 774 x 496 |
| iniBuilds A350-1000 | `ini-a350-1000` | 1644 x 1024 EFIS surface | Nose 774 x 251 / tail 774 x 496 |
| iniBuilds A380 | `ini-a380` | 768 x 1024, one mip; supported RGBA/BGRA views | Nose 736 x 251 / tail 736 x 496 |
| PMDG 777-200ER | `pmdg-777` | Shared `DUS`, 2048 x 2048; gauges `DU_LeftInboard` and `DU_RightInboard`. Lower DU: `DU_Lower` on `EICASCDU`, 2048 x 2048 | Nose 736 x 268 / left and right wing 360 x 360 |
| PMDG 777-300ER | `pmdg-777-300er` | Same `DUS` and `EICASCDU` layout as 200ER | Same pane sizes; nose forward **22** m |
| PMDG 777F | `pmdg-777f` | Same `DUS` and `EICASCDU` layout as 200ER | Same pane sizes; nose mount copied from 200ER |
| Aerosoft A340-600 | `aerosoft-a346` | Shared `$GAUGES_UNIFIED`, 4096 x 4096; gauges `CaptPFD`, `CoPFD` and `ECAM_LOWER` (750 x 750) | Nose 736 x 251 / tail 736 x 496 |
| iniBuilds A340-300 | `ini-a340-300` | One 1560 x 2340 one-mip texture, a 2 x 3 grid of 780 x 780 display cells (name unknown) | Nose 736 x 251 / tail 736 x 496 |

A350 package identifiers and geometry were inspected in iniBuilds version 1.2.6. The user has verified A350 rendering in the simulator. This does not establish every aircraft variant, framing, graphics mode or automatic target ordering; see [PR 42 validation](pr42-validation.md).

## Controls

| Aircraft | Left / right state | Automatic OFF |
| --- | --- | --- |
| FBW A380 | `L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON`, `L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON` | Corresponding `A32NX.FCU_EFIS_L_TAXI_PUSH` / `R_TAXI_PUSH` event |
| A350 | `L:INI_TAXI_LEFT`, `L:INI_TAXI_RIGHT` | Write zero to the selected latch through public SimConnect |
| iniBuilds A380 | Manual previews or configurable camera hotkeys; cockpit buttons marked INOP | Suppress camera output above the speed limit; no aircraft-variable writes |
| PMDG 777 | Display Select Panel: L INBD / R INBD / LWR CTR selector lamps and the CAM button (read only). Manual previews and Ctrl + Shift + L / R / B / D add displays | Suppress camera output above the speed limit; no aircraft-variable writes |
| Aerosoft A340-600 | `L:AB_VC_CAM_CAPT_SEL`, `L:AB_VC_CAM_FO_SEL`, plus `L:AB_VC_CAM_SD_SEL` for the lower ECAM (0 OFF, 1 TAXI) | Write zero to the selected latch through public SimConnect |
| iniBuilds A340-300 | Manual previews or camera shortcuts (Left, Right, Both, SD); no cockpit camera control | Suppress camera output above the speed limit; no aircraft-variable writes |

The installed iniBuilds A350 behavior XML uses each TAXI latch for its button state and lamp. Its input-event setter toggles that latch. An idempotent zero write makes cutoff independent of toggle timing. The other side is not written. The FBW A380 and A350 adapters wait for a fresh OFF acknowledgement. The manual-only iniBuilds A380 suppresses output without waiting for a cockpit-button acknowledgement. The catalog supplies the speed limit, currently 60 knots for all profiles.

## Display placement

Each side has an explicit destination rectangle. The A380 covers rows 0 through 762 across the PFD. The A350 captain uses columns 0 through 805 of the combined EFIS surface; the first officer uses columns 838 through 1643. Both outer camera regions are 806 pixels wide, leaving a 32-pixel central gap for the grey separator and its edge padding. The adjacent navigation display and rows 763 through 1023 are preserved. The gap is based on the installed divider artwork; exact cockpit alignment remains subject to live verification. The display arrangement follows the [Airbus ETACS diagram, section 4-1-0](https://www.aircraft.airbus.com/sites/g/files/jlcbta126/files/2024-06/AC_A350_0524.pdf).

Inside each outer region, both aircraft draw a black border 16 target pixels wide on the left and right and 12 pixels high at the top. The complete camera image fits within that border, including its GS panel and reference marks. Content is 736 x 751 pixels on A380 and 774 x 751 on A350. This is distinct from the preserved central grey separator.

The profile defines accepted texture dimensions, mip policy and formats. Resource IDs identify an allocation lifetime, not an aircraft material. FBW A380 and A350 detection ranks activity across three one-second windows and requires a clear pair above other candidates. The iniBuilds A380 rule instead checks the complete display group described below. The side-order rule is profile data; it must be verified in the simulator. `$EFIS_LEFT` / `$EFIS_RIGHT` and A380 material hints are reference labels, not proof of GPU identity. **PFD routing** supports explicit assignment and correction when the heuristic is ambiguous.

## Shared rendering contract

The renderer captures independently sized scene textures (two feeds for most profiles; three when `split_bottom` is set). It composes them into one bounded **768 x 763 working image**, then maps that image into the profile's inner content rectangle. This stable GPU buffer is shared infrastructure, not a request to render a full-size simulator view. Native sources match the pane sizes: 736 pixels wide on A380 and 774 on A350, with nose and tail heights of 251 and 496 pixels. The PMDG 777 uses 736 x 268 for the nose feed and 360 x 360 for each wing feed. The visible divider on non-split profiles covers working rows 251 through 262, a 12-pixel band; reducing this band leaves the source dimensions and pane positions unchanged. The border is drawn by the existing PFD shader, without an additional GPU pass.

Profiles supply the pane division, visible separator, reference dot/bracket coordinates and default colour. Both A380 and A350 use 14-by-14-pixel nose squares and default to magenta markings. Both retain their existing tail brackets. **Marking colour** on **Reference guides** changes the nose squares and tail brackets together, with a separate saved RGB colour for each aircraft profile. On A380 and A350, the ground-speed panel is inset from the camera edges, with internal padding and a width that fits the current value. The PMDG 777 keeps ground speed and reference guides off. The same composed page applies to both sides of a profile. Panel layout belongs to the aircraft profile. The ground-speed text has its own saved RGB colour, editable on **Display** when the profile draws GS; changing it does not change exposure or the reference marks. Marks are visual references; adjusting mounts or field of view does not calibrate metric clearance.

## Geometry and settings

Each camera mount supplies right/up/forward metres, pitch/yaw degrees and lens radians. A350-900 and -1000 have separate mount presets and settings files. The presets start from the exterior model's camera-mesh locations, with a forward adjustment for the belly camera. **Camera views** adjusts each profile separately.

The A350-900 defaults are nose **right/up/forward 0 / -2 / 16 m, pitch/yaw -15 / 0 degrees, lens 0.55 rad**; tail **0 / 10 / -33 m, -15 / 0 degrees, 0.62 rad**. Its tail guide points are `(0.29, 0.76)`, `(0.26, 0.87)` and `(0.31, 0.87)` for the left upper point, lower corner and inner endpoint, mirrored on the right. All aircraft share the default GS colour RGB **22 / 109 / 19** (`#166D13`) and marking colour RGB **255 / 0 / 255** (`#FF00FF`).

The A350-1000 retains the same height, pitch, yaw and lens for each camera, with nose forward position **19.81 m** and tail **-36.17 m** for its longer fuselage. Its tail guide points are `(0.31, 0.69)`, `(0.29, 0.85)` and `(0.37, 0.855)`. The FBW A380 uses tail guide points `(0.33, 0.64)`, `(0.305, 0.75)` and `(0.365, 0.758)`.

All profiles use a daytime exposure default of **-8 EV**, with automatic night adjustment enabled. Existing saved settings take precedence until a keep-install forces `exposure=-8` once (see runtime reference). FBW A380 and A350 profiles start with TAXI-button control enabled; iniBuilds A380 starts in manual control with both displays off. Guide points are fixed image references; changing camera settings can move the gear relative to them. The geometry check projects the installed iniBuilds A350 1.2.6 `flight_model.cfg` gear contact points; it does not replace a cockpit comparison or establish metric clearance.

Changing profiles hides output and closes the owned views through the engine update callback. After validating the same manager, entry IDs and output resources, it retains that pair while changing the control subscription and camera mounts. Render dimensions stay fixed at the pair's original allocation sizes; the compositor scales into the selected profile's PFD rectangle. The bridge clears texture routes, button intent, pose calibration and capture history. A new profile cannot inherit another aircraft's saved mounts or stale ON state. The application stores the selected profile separately from each profile's INI file.

Flight/aircraft load notifications and changes in simulation running state also trigger this suspended transition, including reloads of the same aircraft. Confirming the current profile again in the dropdown explicitly retries its connection and display discovery. Manual texture IDs and preview/calibration requests belong to one flight and are cleared at the transition; saved mounts, guides and exposure remain intact. Native identity and GPU lifetime checks still apply. If the retained manager, IDs or output allocations cannot be verified, the cameras remain unavailable; a flight-load notification does not authorize abandoning or replacing those objects.

## Adding an aircraft

### iniBuilds A380 configuration

The iniBuilds A380 reports `ATC TYPE=Airbus` and an `AircraftLoaded` path under `SimObjects/Airplanes/inibuilds-a380/presets/inibuilds/a380-800_rr_basic/config/aircraft.CFG`. Matching requires its exact product path component; a generic Airbus type or iniBuilds vendor folder alone cannot select this profile. The FBW A380 keeps its separate identity and settings.

The cockpit marks both TAXI buttons INOP, and no supported public control contract is available for them. The profile therefore neither reads nor writes guessed TAXI variables. Use the companion's left/right preview controls or camera hotkeys. Manual control still obeys aircraft identity, session, service, GPU and speed guards.

The public input inventory contains 1,000 descriptors. Subscriptions expose the captain's PFD brightness adjustment (`AIRLINER_MIP_SIDE_PFD_LEFT`) but no input-value notification for either TAXI button. The clickable cursor therefore does not provide a supported public camera toggle. This does not rule out a private aircraft interaction.

The iniBuilds A380 defaults are: nose **right/up/forward 0 / 2.2 / 16 m, pitch/yaw -17.5 / 0 degrees, lens 1 rad**; tail **0 / 18 / -34 m, -32 / 0 degrees, 1 rad**. The left tail guide points are `(0.34, 0.52)`, `(0.305, 0.65)` and `(0.355, 0.65)`, mirrored on the right. Nose markers are `(0.14, 0.48)`. Daytime exposure defaults to **-8 EV**, with automatic night adjustment enabled. Settings remain independent in `ini-a380.ini`, and saved calibration takes precedence over defaults. These are visual alignments, not a metric clearance calibration.

Texture admission requires 768 x 1024, exactly one mip and supported RGBA/BGRA views. The ini A380 detector uses eight RGBA8 typeless resources (format 27), assigning the highest resource ID left and third-highest right. It checks unchanged membership and activity on every member across three one-second windows; discovery also has a complete-idle-group fallback. IDs may have gaps and are not fixed constants. Missing, extra, changed or incompletely tracked resources prevent the complete-group assignment. Structural changes withdraw automatically assigned sides while preserving explicit selections; loss of both identities can require reselecting the aircraft profile or assigning the PFDs manually. Pauses alone do not remove existing identities. FBW A380 and A350 use activity ranking instead.

The user confirmed camera rendering with the revised bridge, then reported that both A380 integrations selected another instrument when the camera was requested during display boot. Automatic display identity is therefore a known unresolved issue. PFD routing permits manual correction. The dropdown sorts by draw count, so its visual position is not resource-ID order. No texture-content classifier or thumbnail export is implemented. Own-device GPU fixtures check display regions and preserved lower rows; they do not establish cockpit identity, hotkeys, AA, turns, cutoff or reload behavior. See [PR 42 validation](pr42-validation.md) for the exact tested build and scope.

### PMDG 777 configuration

Three profiles match the 777-200ER, 777-300ER and 777F from the airplane folder in the `AircraftLoaded` path (`pmdg-777`, `pmdg-777-300er`, `pmdg-777f`). The open 777-200ER RR reported `SimObjects\Airplanes\PMDG 777-200ER\presets\pmdg\PMDG 777-200ER RR\config\aircraft.CFG` and `ATC TYPE` `ATCCOM.ATC_NAME BOEING.0.text`. That path has no `pmdg-aircraft-77er` component, so a package-folder match does not select it. The matching component is `PMDG 777-200ER`. The 300ER and 777F use the same kind of component, `PMDG 777-300ER` and `PMDG 777F`; those two paths were not in this log. ATC TYPE is the Boeing brand string and is not required. A `-copy` folder does not match.

The picture is the navigation display, not a flight PFD. A live scan of the open 777-200ER RR found no separate taxi-camera texture. Both inboard gauges, `DU_LeftInboard` and `DU_RightInboard`, draw one shared 2048 x 2048 texture named `DUS`. The navigation display is the whole inboard gauge. `panel.cfg` gives the same `htmlgauge` x, y, width, height on the 777-200ER, 777-300ER, and 777F: `DU_LeftInboard` is 1058, 33, 958, 971 and `DU_RightInboard` is 30, 1058, 958, 971. Taxi Cam stamps the composed page only inside those two rectangles. The rest of `DUS`, including the outboard PFDs, stays clear.

Taxi Cam does not copy a camera page the simulator already drew. It keeps **three** viewpoints — nose looking forward over the nose gear, and separate left- and right-wing mounts looking aft at each wing — and composes them into the shared 768 x 763 working image (`split_bottom`), then stamps that image into each inboard gauge rectangle on `DUS`. Layout in that working image:

- nose picture **280** px tall from y = 0 (full width), with a **10** px black frame on the left, right and bottom edges before the T;
- T divider from y **280** to **318** (**38** px, ~4/5 of the **48** px gap) in colour **`#1C1B22`**, including the vertical gap between the bottom panes. The T is written like PMDG's own UI colours, so an sRGB view stores it as about `#5D5C66`, as it stores those colours (inferred);
- equal **360 x 360** square bottom panes from y **318** to **678** (camera feeds 360 x 360) with **10** px black borders on the top and sides only (no bottom border), and a single **24** px round at the T junction (left pane top-right, right pane top-left); other corners stay square. Rows below the squares stay black.
- Horizontal T bar keeps **`#1C1B22`** with **10** px black only at its left and right ends (not along the whole bar).

An **85** px top inset (`camera_padding` top; bottom inset 0; L/R 0) moves the stamped block down on the 958 x 971 ND so the black band above the nose picture is larger without shortening the nose picture. Other profiles leave `split_bottom` off, so their full-width tail is unchanged.

Ground speed is **off** for these profiles only: the live path skips the GS overlay entirely (no readout and no black GS box). The GS font fixture still enables the panel on every catalog profile, including 777, and keeps the default panel origin so padding checks stay strict. Reference guides stay off; the companion disables that settings page for 777. Daytime exposure stays the shared default of **-8 EV**. Settings are stored per type in `pmdg-777.ini`, `pmdg-777-300er.ini` and `pmdg-777f.ini`. PFD refresh is not measured (`pfd_refresh_hz` 0).

Published mount defaults: 777-200ER and 777F nose **0 / -2 / 16 m, pitch/yaw -18 / 0 degrees, lens 1 rad**; 777-300ER nose **0 / -2 / 22 m, pitch/yaw -18 / 0 degrees, lens 1 rad** (longer fuselage; forward only differs from the 200). Left wing **-6 / 1.5 / -28 m, -5 / -12 degrees, 0.6 rad**; right wing **6 / 1.5 / -28 m, -5 / 12 degrees, 0.6 rad**. Wing mounts stay these published values until those types are calibrated separately. They are not a metric clearance calibration. Live framing and lighting still need an in-simulator check.

The display list only offers GPU resources that match the selected profile's size. Until this profile is selected, the bridge keeps the previous profile's filter. The default is the FBW A380 at 768 x 1024, five mips, format 28. A 2048 x 2048 `DUS` texture is not a candidate for that filter, so the list stays empty and detection reports `no_candidates`. The log does not print gauge names. After this profile is selected, a 2048 x 2048 resource with any non-zero format and 1–12 mips is a candidate. Mip count and DXGI format were not in the scan. The texture list and the display-routing dropdown use resource-id order, not draw count. When more than one candidate matches, automatic selection takes the last entry, which is the highest resource id, and assigns that one texture to both inboard rectangles. It does not take the first entry, and it does not use a texture name. Display routing can still assign a texture by hand. GPU targeting cannot read gauge or material names; `DU_LeftInboard` and `DU_RightInboard` are the rectangles above on the shared `DUS` texture.

#### CAM button and display selection

The glareshield Display Select Panel (DSP) chooses the display. Select **L INBD**, **R INBD** or **LWR CTR**, then press **CAM**. That display shows the camera page until **CAM** is pressed again while the same display is selected. Displays are independent, so any combination of the two navigation displays and the lower DU can show the camera. The CAM button carries an INOP label in the cockpit, but PMDG still draws its own placeholder camera page when it is pressed; Taxi Cam stamps the live camera over that page.

Taxi Cam reads public L:vars only and writes nothing to the aircraft. PMDG's SDK data broadcast is not needed. `L:GMC_L_INB`, `L:GMC_R_INB`, `L:GMC_LWR` and `L:GMC_DU` exist in the cockpit behaviour but stayed 0 while the CAM page was shown, and no model node uses them. A read-only capture on the 777-300ER on 2026-09-23 established the inputs:

| L:var | Meaning | Observed values |
| --- | --- | --- |
| `switch_2311_a`, `switch_2321_a`, `switch_2331_a` | L INBD, R INBD and LWR CTR selector lamps | About 1.003 lit, 0 unlit; exactly one lit |
| `switch_243_a` | CAM | 100 while pushed, for 0.1–0.25 s |
| `switch_234_a`–`switch_242_a`, `switch_244_a`–`switch_246_a` | ENG, STAT, ELEC, HYD, FUEL, AIR, DOOR, GEAR, FCTL, CHKL, COMM, NAV | Momentary, same as CAM |
| `switch_315_a`, `switch_290_a` | Left and right INBD DSPL knobs | Position |

The same switch numbers are in the 777-200ER and 777F cockpit behaviour files. After CAM was removed from R INBD, the selector lamp returned to LWR CTR by itself.

The telemetry worker polls these values with the other TAXI data and also streams changes every simulator frame, so a short CAM push is not missed. It follows the button presses rather than reading PMDG's page state:

- CAM with exactly one lamp lit toggles that display.
- Another synoptic button with a lamp lit replaces that display's CAM page.
- Moving a side's INBD DSPL knob replaces that inboard display's CAM page.
- All lamps dark (panel unpowered) clears every CAM page. More than one lamp lit (lamp test) ignores CAM.
- The first sample after connection only records switch positions. A CAM page left on from before Taxi Cam connected needs CAM to be pressed again.

Pages changed in other ways can leave Taxi Cam out of step with the cockpit until CAM is pressed again. The rules have been checked against the recorded sequence and unit fixtures only. Live stamping on the lower DU has not been observed yet.

**Overview → CAM button** turns this cockpit control on or off. The first load of an existing PMDG 777 profile after this change turns it on once (`cam_button_revision=1`), because these profiles were previously saved as manual-only; later choices are kept. Manual previews and the Left, Right, Both and SD shortcuts add displays on top of the CAM selection and do not turn cockpit control off. A display turned on by CAM is turned off with CAM. Calibration turns cockpit control off, as on other aircraft, and turning the last calibration control off restores it.

#### Lower DU

`panel.cfg` `[VCockpit02]` draws `DU_Lower` on a separate 2048 x 2048 texture, `EICASCDU`, at 1058, 21, 958, 971 on all three variants. The upper EICAS and the three CDU screens share that texture. The lower DU uses the same composed page and the same 85 px top inset as the navigation displays.

`EICASCDU` has the same size as `DUS`, the tablet textures and the upper-EICAS copy, so the size filter cannot separate them. Automatic selection takes the next-highest resource id after `DUS`. One earlier 777 session log listed five 2048 x 2048 textures (268–272): `DUS` was 272, and 271 had the highest draw count of the rest, consistent with `EICASCDU`. This guess is unverified. **PFD routing** has a **LOWER DU TEXTURE** list, **Lower preview** and **Calibrate lower** to identify and correct it. On single-display profiles (the PMDG 777 and both A340s) the calibration bars fill exactly the selected gauge rectangle. Earlier builds painted a full-height column from the top of the texture, so on the 777 **Calibrate right** also lit the left ND, and the queued private-patch path rejected the 777 gauge size. An explicit choice is kept until that texture is destroyed; a lost automatic choice is guessed again.

### Aerosoft A340-600 configuration

Inspected in the installed Aerosoft A346 Pro 1.0.1 package (`aerosoft-aircraft-a346-pro`), a ToLiss systems port. `aircraft.cfg` gives `atc_type` as the Airbus brand string and `atc_model` `A346`. The profile matches the SimObject folder component `SimObjects/Airplanes/airbus-a346-pro` (or the package folder) and does not require ATC TYPE. A `-copy` folder, the simulator's `_CVT_` conversion folder and the iniBuilds A340 do not match. The live `AircraftLoaded` path has not been logged yet.

`panel.cfg` draws every display into one 4096 x 4096 `$GAUGES_UNIFIED` texture through `MSFS_ToLiss_Plugin.wasm`. The TACS selectors on the main panel (**CAM CAPT**, **CAM SD**, **CAM F/O**) control the captain's, first officer's and lower ECAM camera displays. Taxi Cam drives all three as display sides 0, 1 and 2: `CaptPFD` at 9, 470, 750, 750, `CoPFD` at 9, 1230, 750, 750 and `ECAM_LOWER` at 1529, 1230, 750, 750. Live testing on 2026-09-23 showed the `CaptND` and `CoND` regions one display out on each pilot side; the captain's PFD placement and the lower ECAM were confirmed in the simulator, and the F/O PFD follows the same layout but is not yet re-checked. It is the only profile with a third side; every mask, command and copy path is bounded by the profile's side count, so two-display aircraft are unchanged. Detection uses the single-texture rule: the last size-matched texture-list entry is assigned to every side, and everything else on `$GAUGES_UNIFIED` stays clear. On **PFD routing**, the first list chooses that texture for every side; the second list and **Swap** are disabled. The routing page adds **SD preview** and **Calibrate SD**. Calibration clears a full-height column at a side's x position; both PFDs share x = 9, so calibrating either pilot side covers both PFDs. Mip count and DXGI format have not been scanned. Until they are, any 4096 x 4096 render target with a non-zero format and 1–12 mips is a candidate, so automatic selection must be checked against the texture list.

Each ND keeps the Airbus layout: nose above the 12-pixel divider, tail below, the GS panel and the reference guides. The border is the usual 16 pixels at the sides and 12 at the top, which gives 718 x 738 content pixels. The A380 source panes are scaled into that slightly smaller rectangle. Guide defaults come from the user's live calibration on 2026-09-23: tail upper `(0.28, 0.72)`, corner `(0.24, 0.86)` and inner `(0.31, 0.86)`, with the shared nose markers.

Each selector is a two-state XML switch whose setter writes 1 (TAXI) or 0 (OFF) to its latch, `L:AB_VC_CAM_CAPT_SEL`, `L:AB_VC_CAM_FO_SEL` or `L:AB_VC_CAM_SD_SEL`. The three latches arrive in one 64-byte SimConnect packet. The profile reads those latches and uses the A350's idempotent zero write for automatic cutoff. The ToLiss module also reads the latches: when one is set, it draws its own synthetic taxi-camera page on that ND from the sprite sheet `data/display data/TaxiCam.png`. No package setting disabling that page was found. Taxi Cam covers the whole ND rectangle from the moment a side is admitted: first with its PLEASE WAIT page, then with the cameras, and with the page again if the camera image goes missing or stale. The built-in page can still appear before the display texture is routed and above the speed cutoff, where the latch is written OFF. Stamp ordering against the module's redraws (visible flicker) is unverified.

`flight_model.cfg` contact points (feet) put the nose gear at forward/up 103.51 / -21.4 and the wing gear at right/up/forward ±17.5 / -22.4 / -4.46. The mounts started from the A350-900's gear-relative offsets. The defaults are the user's saved live calibration from 2026-09-23: nose **0 / -2.5 / 23.33 m, -15 / 0 degrees, 0.55 rad**; tail **0 / 8.18 / -28.99 m, -15 / 0 degrees, 0.62 rad**. These are visual alignments, not a metric clearance calibration. PFD refresh is not measured (`pfd_refresh_hz` 0). Settings are stored in `aerosoft-a346.ini`.

### iniBuilds A340-300 configuration

The iniBuilds A340-300 is a streamed MSFS 2024 package (`fs24-inibuilds-aircraft-a340`). Its archives are encrypted, so `panel.cfg`, `flight_model.cfg` and the texture names cannot be read. On 2026-09-23 the loaded `a340-300_eis2` preset reported `ATC TYPE` `Airbus` and the `AircraftLoaded` path `SimObjects\Airplanes\inibuilds-a340\presets\inibuilds\a340-300_eis2\config\aircraft.CFG`. The profile matches the SimObject folder component `SimObjects/Airplanes/inibuilds-a340` (or the package folder) and does not require ATC TYPE. Other presets in that folder, such as a different EIS, use the same profile but have not been seen.

The same session's `Render-target shapes` log showed, when the cockpit loaded, a group of one-mip `R8G8B8A8_TYPELESS` textures like the other iniBuilds displays: 1560 x 2340, 1024 x 1024, 1024 x 735, 300 x 300 and two each of 450 x 400 and 450 x 340. The 1560 x 2340 texture, created once, is exactly a 2 x 3 grid of 780 x 780 cells, one per display unit. Taxi Cam treats it as a single display texture, as on the A340-600: the last size-matched texture-list entry is assigned to every side. A five-mip 2340 x 2340 texture from the same load is not a candidate.

Live testing on 2026-09-23 placed the cells: the top-left cell is the captain's ND, the middle-right cell the first officer's PFD and the bottom-right cell the lower ECAM. The rows are ND / PFD pairs: CAPT ND and CAPT PFD, F/O ND and F/O PFD, then E/WD and SD. The camera displays are therefore the right column, each 780 x 780: the captain's PFD at 780, 0 (from that pattern; not yet re-checked), the first officer's PFD at 780, 780 and the lower ECAM at 780, 1560. **Calibrate left**, **Calibrate right** and **Calibrate SD** on **PFD routing** light the selected cell. Inside each cell the usual 16 / 12-pixel border leaves 748 x 768 content pixels, and guide defaults are the A340-600's.

No TACS selector or other camera control was found, so the profile is manual-only like the iniBuilds A380: Ctrl + Shift + L / R / B / D and the previews turn the three displays on and off, nothing is written to the aircraft, and output stops above the speed limit. Mounts start from the A350-900 defaults. PFD refresh is not measured (`pfd_refresh_hz` 0). Settings are stored in `ini-a340-300.ini`. Automatic detection and the F/O PFD and SD placement were seen in the simulator; framing and cutoff still need a live check.

### Integration requirements

Define an `AircraftProfile` in [the catalog](../src/profiles/catalog.hpp): accepted aircraft types and add-on path markers, control strategy and variables, texture constraints, side-order rule, destination rectangles and border insets, camera dimensions, mounts, composition, speed limit and the measured PFD refresh (`pfd_refresh_hz`: bridge `stamps`/s ÷ 2; 0 until measured; iniBuilds A380 16 from 0.9.11, A350 80 from the 0.9.35 sweep under frame generation, FBW A380 not yet measured). Rendering, GPU synchronization, exposure and native camera ownership consume these values without aircraft-name branches.

When the package cannot be read (streamed iniBuilds aircraft are encrypted), load the aircraft with the bridge attached and read the `Render-target shapes` and `Aircraft identity` records in `bridge.log` (see [runtime reference](runtime-reference.md)). Shapes first created at the aircraft load are the display-texture candidates.

Add profile/settings tests and a GPU fixture that checks both camera regions and every preserved display region. Then verify actual cockpit buttons, texture identity, framing, cutoff and aircraft reload. An aircraft with different control semantics needs a control adapter; an aircraft without two camera views needs a different composition contract.
