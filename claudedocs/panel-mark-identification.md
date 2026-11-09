# Display identification by panel marks

Status: design plus offline prototype (2026-11-03). Nothing here has run in
the simulator yet. The prototype bridge only logs what it finds; routing is
unchanged.

## Problem

Taxi Cam has to know which D3D12 texture is which cockpit display, for
example `$EFIS_LEFT` on the A350 or `DUS`/`DU_LeftInboard` on the 777. The
current approach:

1. It decodes MSFS's panel table from memory (`display_panels.cpp`,
   `panel_registry_contract.hpp`), which gives section and `texture=` names.
2. It pairs those names with textures by creation order (`propose`,
   `resolve_names`), with shift, section and shape fallbacks
   (`resolve_by_shape`, `PanelOrder::section`), copy trimming
   (`drop_copy_destinations`, `trim_trailing_ui`) and anchors learned from
   clean loads (`ProcessTables`, `aircraft_signature`, the layout's anchors).
3. When that fails, the user picks displays by picture.

Every rule exists because some aircraft broke the one before:

- the Fenix display boot (shift −1);
- FSL recreating 9 textures;
- the FBW A380X creating its twin PFDs in either order;
- issue 172, where `paired=0 shift=-1`.

Creation order isn't a property of the display, so pairing by it will keep
breaking.

## Idea

Each cockpit panel is drawn by its own Coherent page, `VCockpit.html`, and
that page knows its panel's names and every gauge's rectangle. When Taxi Cam
asks, a small helper on that page paints a short block of pixels into each
gauge saying "panel X, gauge N, rect R". The bridge reads each candidate
texture once and decodes the blocks, which gives name → texture plus gauge
rectangle directly. No order and no history are involved. Settings keep
names only, and every load identifies again.

## Where the helper loads

**Choice: a Taxi Cam copy of `html_ui/Pages/VCockpit/Core/VCockpit.html`**,
with one added line after `VCockpit.js`:

```html
<script type="text/javascript" src="/Pages/VCockpit/Core/taxi-cam-marks.js"></script>
```

The copy comes from the stock file in
`asobo-vcockpits-core` (1515 bytes on Store 1.9.21, 30 lines). These are
facts read from the installed files on 2026-11-03:

- `VCockpit.html` is the page for every `[VCockpitNN]` panel. It loads
  `BaseInstrument.js`, `XMLLogic.js` and `VCockpit.js`. `VCockpit.js` gets the
  panel data in `Coherent.on("ShowVCockpitPanel")` and imports each gauge into
  `<vcockpit-panel>`.
- WASM gauges are `WasmInstrument/WasmInstrument.html`, a
  `<wasm-sim-canvas>` inside that same page. The PMDG 777 DUs and the
  iniBuilds A350 EFIS are WASM gauges (their `panel.cfg`), so a page-level
  helper reaches them too.
- The page keeps the panel data in the global `globalPanelData`:
  - `sName`, `sConfigFile`, `vLogicalSize`, `vDisplaySize`, `daAttributes`;
  - `daInstruments[]`, each with `iGUId`, `sUrl` and `vPosAndSize`.
  - `setupInstrument` places each gauge at `vPosAndSize × vDisplaySize /
    vLogicalSize`, rounded.

We don't override `BaseInstrument.js` (trade-off below). Our page still loads it by the same path,
so any other package's copy of it keeps working unchanged.

### Why not own `BaseInstrument.js`? (re-weighed 2026-11-03)

Weighed on what is best for Taxi Cam, owning `BaseInstrument.js` loses on
Taxi Cam's own terms:

- **What it would add.** Every gauge class extends `BaseInstrument`, so owning
  it would give per-gauge hooks: the gauge's lifecycle and update loop, its
  `instrumentIdentifier`, and `urlConfig` (`index`, `style`). The marks don't
  need hooks, only those facts. The gauges are DOM elements under
  `<vcockpit-panel>` with a `Guid` attribute, so the helper reads
  `element.instrumentIdentifier` and `element.urlConfig.index` from them at
  paint time. The prototype now does this. If a hook were ever needed, the
  helper could wrap `BaseInstrument.prototype` at runtime from our own file,
  since it loads after `BaseInstrument.js`. That works on whichever copy is
  loaded.
- **Stale-copy surface.** Stock `BaseInstrument.js` is 33,744 bytes of logic:
  screen state L:vars, flow events, the update loop. EFB and UI pages load it
  too (`fs-base-efb`, `fs-base-ui`, not only `asobo-vcockpits-core`). A stale
  copy after an update would break gauges in every aircraft and the EFB. A
  stale `VCockpit.html` is one declarative page, one line off stock, used by
  panels only.
- **Other packages that override it.** This decides it. `BaseInstrument.js`
  is a shared file other Community packages may override too, and the last
  one loaded wins. We would either lose the file, so identification would
  silently stop for those users, or win it and break the other package. We
  never edit another package's files. Neither outcome is acceptable.

With the `VCockpit.html` choice, another package's `BaseInstrument.js`
changes nothing for Taxi Cam:

- Our page loads `BaseInstrument.js` by path and gets whichever copy is
  loaded.
- Our helper reads gauge elements the same way on any copy.
- The two overrides are different files, so neither package needs the other.

If another package ever overrides `VCockpit.html` too and loads later, it
wins. Taxi Cam then finds no marks. We don't fight for the file.

### Other load points

| Load point | Problem |
|---|---|
| `Instruments/Shared/BaseInstrument.js` | See above. Contested by other packages' overrides, and has the largest stale-copy surface. |
| `Core/VCockpit.js` | 23.5 KB of logic. A stale copy would break more, and Asobo is more likely to change it. |
| `/JS/common.js`, `simvar.js`, `avionics.js`, `GenericDataListener.js` | Every UI page loads them (toolbar, EFB, menus), so the helper would run everywhere and a stale copy would hurt everywhere. |
| A gauge added to each aircraft's `panel.cfg` | Edits third-party aircraft files, and aircraft updates overwrite it. |

**What could collide with it.** Any package that also overrides
`Pages/VCockpit/Core/VCockpit.html`; the last package loaded wins. On this
machine none of the ~25 Community packages does: I compared 8,325 add-on
`html_ui` files with 4,963 stock ones, and the only override of a stock file
is of a different file (`BaseInstrument.js`). Candidates in future:

- panel streamers or "inject into every instrument" mods;
- an aircraft that ships its own `VCockpit.html` (none seen).

Not built yet: the companion could scan Community `layout.json` files for
the path and say why no marks are found. It never fights for the file: we
don't rename the package to load later.

### When another package takes the page (issue 184, 2026-11-05)

The collision above happened: vRAAS (LAND3 Simulations, payware) ships
`panel-raas`, its own copy of `VCockpit.html` (an older stock page plus
`/RAAS/signalr-3.1.31.js` and a deferred `/RAAS/RaasMain.js`), listed in
`globally_overriden_base_sim_files` with `package_order_hint` `PANEL_PATCH`.
1.3.3 test builds put our line into a copy of that page, took its hint and
relied on our folder name sorting later. On the reporter's PC (Store
1.9.21–1.9.24) vRAAS's page still loaded and no marks appeared; with
`panel-raas` moved out, ours loaded and the displays were found. The SDK
(Virtual File System Information) says packages of one hint mount in
alphabetical order, the later one winning, but not which name is sorted
(folder, title, package name) or whether case counts; our title "Taxi Cam
panel marks" sorts before "panel-raas" if it is the title, case-sensitively.
Which page loads therefore can't be predicted.

**Choice: while another package takes the page, ours replaces
`Core/VCockpit.js` instead**, the stock script with the helper appended.
Every copy of the page loads `VCockpit.js` (vRAAS's does, by the stock
line), so our helper runs whichever page loads. The trade-off weighed in
"Other load points" stands, so the script is used only in this case: a
simulator update to `VCockpit.js` runs our older copy for one session until
the next check rewrites it. The package is left out (`conflict`) when the
other page doesn't load `VCockpit.js` by the stock line, or another package
replaces `VCockpit.js` too.

Live (dev box, Store, A350, 2026-11-05) with a stand-in `panel-raas` made
of the reporter's vRAAS `manifest.json` and `VCockpit.html` (vRAAS's own
`/RAAS/` scripts absent): the earlier merged page read 0 of 24 textures and
reported the package inactive; script mode read the marks and routed both
PFDs and the door display (`ids=271,272,255`). Not run with vRAAS itself,
so its PFD/ND text beside our helper is unverified. While the simulator
runs, a mode switch keeps the old mode's files (they are mounted); they go
at the first check of a later session.

**Manifest.** The package declares the file in
`globally_overriden_base_sim_files`, as Community packages that override
base files do.
I found no public documentation of that field
(search on 2026-11-03), so whether MSFS needs it is experiment E0.

## What the page knows (E1, answered on the A350 2026-11-04)

The panel data has exactly `__Type, sName, vLogicalSize, vDisplaySize,
daAttributes, daInstruments, sConfigFile`, and each gauge has `__Type, iGUId,
sContainer, sUrl, …`. There is **no `texture=`**: the page knows its
**section** name (`VCockpit02`) and its `panel.cfg` path.

Section names aren't unique in MSFS 2024, because every attachment has its
own `panel.cfg`. For example, the Fenix
`Part_Interior_Panel_Refueling_Readouts/panel/panel.cfg` has its own
`[VCockpit17]` (`texture=Panel_Refueling`). So each mark carries `sName`,
the gauge name and `c=` + a 16-bit hash of `sConfigFile` in its info text.
The stable name is (section, config hash, gauge index). Where the texture
name is wanted for display, the companion can read `texture=` for that
section from the `panel.cfg` next to it.

On the A350 every panel's page size equalled its texture size, so the page
draws at one texture pixel per page pixel.

## Mark format

The format is in `src/shared/panel_marks.hpp`; the JS mirror is
`src/panel_marks/taxi-cam-marks.js`.

**Cells.** A block is a grid of square cells, 16 wide, with a one-cell
border of 0. Each cell is one of 8 colours whose channels are each 0 or the
helper's level, so it carries 3 bits. The helper paints `kLevel` = 56 (dark
grey; 16 was read as about 1 on the A350's WASM displays, 2026-11-04) in 2 px cells (`kCell`). A transfer curve keeps 0 at 0 and only raises
the level (an sRGB texture encoding it again gives about 70), so a channel
counts as 1 at ≥ 8 and as 0 at ≤ 2. The decoder samples 9 points around
each cell's centre and needs 5 to agree. Bright (255) blocks from the first
prototype still read at their own size; scaled 2 px blocks would not, which
is why the page must draw at texture size.

**Rows.**
- Row 0 is a fixed finder of 16 colours (`7,0,7,0,4,2,1,6,3,5,0,7,4,0,1,7`).
  It gives the start and the horizontal pitch.
- Column 0 of every later row alternates 0 and 7. That timing column gives
  the vertical pitch and the row count.
- The other 15 cells of each row carry payload bits, most significant bit
  first, then a CRC-16/CCITT-FALSE.

**Payload, version 1** (2–160 bytes):

| Field | Type |
|---|---|
| version | u8 |
| length | u8 |
| kind (gauge or panel) | u8 |
| cell size | u8 |
| gauge index | u8 |
| gauge x, y, w, h (page px) | u16 each |
| panel width, height (page px) | u16 each |
| panel name (`sName`) | u8 length + ASCII |
| gauge name | u8 length + ASCII |
| info (`c=` + config hash) | u8 length + ASCII |

The gauge name is `wasm_gauge`, or the page's file name. Kind `panel` was the
prototype's probe block and is no longer drawn.

**Size.** With 2 px cells a block is 36 px wide and 22–26 px high (9–11 rows
including the border): about 2% of an A350 EFIS texture's width, against
90×55 px for the first prototype's 5 px bright cells.

**Scale.** The texture rectangle is the page rectangle × texture size /
panel size.

## Placement and timing

- **Where.** Each gauge's block sits in the gauge's top-right corner, one
  cell in from the edges; a gauge smaller than its block plus one cell all
  round gets none. No block touches a gauge's top-left corner, which other
  tools sample. On most displays that corner is black, so the near-black
  block barely shows.
- **When.** From panel load (+0.5 s) until Taxi Cam has read it, at most
  `MAX_SHOW_MS` (45 s of running simulation). The page reads
  `L:TAXICAM_MARKS` when it loads; the bridge's SimConnect worker sets it to a
  new value once the bridge has read the panels, and every page polls it every
  250 ms and takes its blocks down when it changes. An L:var outlives the
  flight, so only a change after the page loaded counts. Without Taxi Cam the
  cap removes them after 45 s. The bridge also asks the pages to show their
  marks (a new negative value) as soon as an aircraft session has an unread
  panel texture: on the A350 (2026-11-04) the page's own window ran out during
  the flight load, and nothing was read until the first re-show ~45 s later.
- **Take-down policy (bridge).** The bridge signals "read" as soon as every
  display the profile names is found, without waiting for the cockpit's other
  panel textures. Otherwise they stay up at most 20 s (`MarkShowMs`). A read
  texture being destroyed brings them back on any aircraft, found or not, and
  holds them up 5 s past the last loss (`ReplacementWatch`): on 2026-11-08 the
  Fenix A320 recreated 59 display textures in 86 minutes of flight, and the
  list fell to none identified with 20 still present. Opening the companion's
  aircraft setup asks for them each time (the setup lists only read textures),
  and so does a camera asked for on a side with no texture. Other re-shows are
  for a texture created since the last show, while a display is still to find
  or the setup is open. Before
  this (FBW A380X, 2026-11-04) the scan waited on every texture: about 13 of
  22 are never drawn, entries churned, the scan never finished, so the marks
  stayed until the page cap and came back twice after Ready.
- **Guarded.** Every step is in try/catch, and an exception removes what was
  drawn. The helper can't stop a page loading.
- **Visibility.** A small dark rectangle in a gauge's top-right corner for
  about the time the bridge needs to read the panels after the cockpit
  appears; on a light display (EFB) it shows as a dark square for that time.

First live run (A350, 2026-11-04, prototype 1): bright 5–10 px blocks,
centred, shown 2 min after load. The user found them far too visible and too
long; this design is the answer.

## Bridge detection path

1. **Candidates.** Every panel-class texture (multi-mip RGBA render target).
   Discovery already admits these, `panel_texture_desc` in phase 1, so this
   works for built-in and unknown aircraft alike.
2. **Scan.** The bridge reads every panel texture of the flight once by
   itself (`MarkScan` in `bridge_main.cpp`), not only when the Displays page
   asks for a picture: one snapshot at a time through the existing
   machinery (a base-mip copy at a proven submission boundary, then mapping
   the readback), only while the companion's own picture is not pending. The
   full-size bytes go through `panel_marks::find_marks` before they are
   shrunk. A texture with no marks yet is tried again after 1 s, at most 5
   times. When every texture is read or given up on and none has appeared
   for 3 s, it calls `signal_panel_marks_read()` and the pages take their
   marks down. A later read signals again. Logged as `Display mark:` and
   `Display marks: N of M panel textures read`.
3. **Map.**
   - (panel name, gauge index) → (texture id, rect in texture px). Gauge name
     and info are stored for display.
   - A texture with no marks stays unidentified.
   - Two textures with the same panel name keep the newer one. This is
     logged.
4. **Between requests,** `follow_copies` keeps a mapped texture across MSFS's
   one-to-one copies (FBW A380X, FSL), which is cheap.
5. **Request again** when:
   - a mapped texture dies with no copy to follow;
   - a routed name is missing;
   - the user opens Displays or setup;
   - Settings name a panel that the current map doesn't have.
6. **Cost.** Live on the A350, decoding took 0.1–6 ms per texture (300² to
   2355×1467) on the bridge control thread, never on a hook path. Snapshots
   came about 0.4 s apart when the Displays page asked for them; 17 panels
   took about 17 s that way, which the scan replaces.

## What it replaced (done, 2026-11-04)

The owner chose marks as the only identity path, with no fallback, and the
activity detector removed too.

| Removed | Now |
|---|---|
| Panel table read (`read_panels`, `panel_registry_contract`, `resolve_panel_table`, `display_panels.*`) and the display-identity research scanner | Deleted. Nothing is read from simulator memory to name displays. |
| Order pairing (`propose`, `resolve_names`, shift, `resolve_by_shape`, `drop_copy_destinations`, `trim_trailing_ui`, creation bursts, `ProcessTables`, clean-load learning) | Deleted. `MarkIdentityState` / `service_mark_identity` in `bridge_main.cpp` with `src/shared/mark_identity.hpp`. |
| Layout anchors, catalog `panel_shapes`, `panel_clean`, `panel_shift`, `PanelMapState` unavailable/unresolved/incomplete | Deleted (protocol 29: `waiting`, `marked`). |
| Activity / allocation-group detector (`pfd_target_detector.hpp`, `dominant_activity_pair`, `allocation_group_pair`) | Deleted. `discover_pfds` adopts named sides only; explicit choices still win. `PfdDetectionPolicy` remains as a layout flag (single display, set-up). |
| `follow_copies` / `follow_recreated` | Copies inherit the source's marks (`Store::inherit`); the end of a copy chain wins. A copy whose source is gone stands in for it (the PMDG 777 replaces its lower display with a copy), `inherit` running before `retain`. Recreated textures are read again (marks re-shown). |
| Keys | Catalog and saved choices name displays by key: `@gauge`, or `@gauge#rank/count` where several panels carry the gauge (ranked by panel number, resolved only when exactly that many do). The page's `sName` is MSFS's running panel number over all attachments in load order, NOT the panel.cfg section: on 2026-11-04 the Fenix's FAP EFB loaded first and every cockpit panel moved up by one, so section-based keys failed. Built-in keys: A350 `@EFIS#0/2`, `@EFIS#1/2`, door `@CTR`; FBW A380X `@pfd#0/2`, `@pfd#1/2`, door `@sdv2`; 777 `@DU_LeftInboard`, `@DU_RightInboard`, `@DU_Lower`; Aerosoft A346 `@NanoVGInstruments`; iniBuilds A340-300 `@DisplayManager`; iniBuilds A380 `@ini-crash-report#0/2`, `@ini-crash-report#1/2`, door `@DU#3/6` (from the owner's choice in Displays, 2026-11-04: its package is encrypted); Fenix door template `@ECAM_Lower`. |
| Old saved `texture=` names | `legacy_display_names.hpp`: the companion converts them once from the aircraft's panel.cfg files (Community, then Official) and saves; unreadable files ask the user to choose again. |

## Install and update (done, 2026-11-04)

`src/setup/marks_package.hpp`, the helper embedded in `taxi-cam.exe` (app.rc
RCDATA 201):
- Setup's `configure` writes the package (best effort, outcome in
  `setup-diagnostics.log` as "Panel marks package: …"); `uninstall` removes it
  (only a folder with our marker).
- The companion checks it at every start on a worker thread: stock page or
  helper changed (SHA-256 in `taxi-cam-marks.stock.txt`) rewrites it; a stock
  page without exactly one `VCockpit.js` line removes the package. A write
  while MSFS runs takes effect at the next start (notice shown).
- Community from the `InstalledPackagesPath` of the simulator in use: Steam's
  `UserCfg.opt` (`%APPDATA%`) for an install under `steamapps`, the Store's
  (`%LOCALAPPDATA%\Packages\Microsoft.Limitless_…`) otherwise, falling back to
  Steam's only when the Store has none. The simulator folder is the
  installer's chosen folder (Setup) or the simulator the companion attaches
  to (no process scan of its own). The uninstaller removes the package from
  every Community folder it finds. The owner's PC has both builds; the first
  Steam start (2026-11-04) showed the old Store-then-Steam order checking the
  Store's package while Steam ran without one.
- `tools/panel-marks/make-package.ps1` stays as a developer tool; the
  companion rewrites its package on the next start (different marker text).

## Risks and unknowns (each has an experiment)

| # | Question | Why it matters |
|---|---|---|
| E0 | Does MSFS load an override of `VCockpit.html` from a Community package, and does it need `globally_overriden_base_sim_files`? | Whole design |
| E1 | Which panel and gauge fields does the page get? Is `texture=` among them? **Answered (A350): no `texture=`; section + `sConfigFile`.** | Whether the panel-table decode can go |
| E2–E6 | Do DOM blocks drawn over WASM / NanoVG gauges reach the panel texture, unscaled or at what scale, in all five aircraft? Do hidden or `quality=hidden` panels draw them? | Detection |
| E7 | With other panel tools installed, displays are still identified and those tools keep working | Coexistence |
| E8 | Does the companion run before MSFS mounts Community? | Stale-copy window |
| E9 | Do the dark top-right blocks show in a way users mind, and how long are they up after the cockpit appears? | UX |
| E10 | Is every page drawn at its texture size on the other four aircraft (2 px cells don't survive scaling)? | Detection |

Also open: panels updating only when visible (blocks wait for the next draw),
and aircraft that put a full-page element above `document.body` children
(none seen).

## Live result 1 (A350, 2026-11-04, first prototype)

- E0: the Community override of `VCockpit.html` loads.
- E1: answered above (no `texture=`).
- E2: every one of the 17 A350 panels was found by its marks, with the right
  section and gauge names: both EFIS (PFD/ND), CTR, MFD, both HUDs, BAT,
  three RMPs, FCU, both EFBs, both ISIS and the FAP. Page size = texture
  size everywhere; decoding 0.1–6 ms per texture.
- E9: the bright, centred blocks were far too visible, and the 2-minute
  load window kept them up much longer than needed. This led to the current
  design (dark 2 px blocks top-right, read by the bridge's own scan, taken
  down on its signal, then a 20 s cap, now 45 s).
- A350, 2026-11-04 (level 56): all 17 panels read again, and named routing
  resolved `@EFIS#0/2,@EFIS#1/2,@CTR` to 261, 260 and 259. But nothing was
  read until the bridge's first re-show, so it took about 70 s from the start
  of the scan. Hence the show request at session start (not yet run live).

## Done locally

- `tests/graphics/panel_marks_test.cpp`: the JS and C++ encoders agree byte
  for byte (golden vector); dark 2 px blocks top-right decode on 0, 4, 60 and
  235 backgrounds, BGRA with a raised level (70), the 777 `DUS` layout with
  corner markers at gauge origins; bright 1× blocks still decode; damaged
  blocks fail the CRC; no false blocks on a busy display.
- `tools/panel-marks/stub-page.js` + `decode_raw.cpp` run the real helper
  against a stub DOM with the 777 DUS, A350 EFIS, ISIS and FBW PFD fixtures
  on dark, grey and light backgrounds: every gauge block is found with the
  right names and rects.
- `tests/camera/body_pose_provider_test.cpp --self-test` passes with the
  `L:TAXICAM_MARKS` definition added.
- `-Validate` and the smoke test have not been run.

## Experiment plan (fresh flight loads only; never reload or switch aircraft in a flight)

**Setup, once.** Get the owner's OK first, because it writes to the real
Community folder and needs an MSFS restart.

1. Close MSFS.
2. Install the prototype build (plain `build.ps1` + the usual install).
3. Run `tools/panel-marks/make-package.ps1`.
4. Start MSFS.

**Per aircraft** (A350, 777-300ER, Fenix A320, FSL A321, FBW A380X): one fresh
flight load from the main menu.

1. Keep Taxi Cam's companion running (it connects the bridge); don't open
   Displays and leave the cameras off for the first minute.
2. Note the cockpit by eye: are dark blocks visible in the displays'
   top-right corners, and how long after the cockpit appears do they go?
3. After the flight, read `bridge.log`:
   - `Display mark: … name=… gauge_name=… info=c=…` per gauge found, and
     `Display marks: N of M panel textures read; cockpit pages told to take
     their marks down` once (its time against the cockpit appearing).
   - `pitch` 2.00 on every mark answers E10; a panel with no mark is either
     scaled or not drawn.
   - `texture_rect` against the texture size, and the 777 DU rects against
     `panel.cfg`.
   - Compare with `Display identity (proposed)` and the snapshots: does every
     marked name sit on the texture the user would pick? Count disagreements;
     the A380X twin PFDs and the Fenix boot are the interesting ones.

**Pass:**
- every display Taxi Cam routes on these five aircraft is found by name in
  each load;
- no mark is decoded on the wrong texture;
- the decode cost is well under the snapshot cost.

**E7, other panel tools.** Repeat the A350 load with another panel tool
installed and in use. Check its panels keep working while blocks show.

**E8.** Log the companion start time against MSFS's package-mount marker in
its own logs, if there is one.

**Afterwards:** `make-package.ps1 -Remove`, or keep it for the next phase.

**Next phase, if E0–E2 pass.**
- The companion makes and checks the package.
- `L:TAXICAM_MARKS` request plus the bridge sweep.
- Routing from the mark map, behind the old pairing as fallback.
- Then a PR deleting the order-pairing code.
