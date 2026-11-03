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

GlassOut works in a similar way: it paints marker colours from a helper in
every cockpit page and checks two pixels at each crop origin. We copy the
idea, not its code, and we keep out of its way.

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
so GlassOut's copy (package `zzz-glassout-hook`) keeps working unchanged.

### Why not own `BaseInstrument.js`? (re-weighed 2026-11-03)

Avoiding GlassOut's file was a hard rule at first. It is now a trade-off,
weighed on what is best for Taxi Cam first. Owning `BaseInstrument.js` still
loses on Taxi Cam's own terms:

- **What it would add.** Every gauge class extends `BaseInstrument`, so owning
  it would give per-gauge hooks: the gauge's lifecycle and update loop, its
  `instrumentIdentifier`, and `urlConfig` (`index`, `style`). The marks don't
  need hooks, only those facts. The gauges are DOM elements under
  `<vcockpit-panel>` with a `Guid` attribute, so the helper reads
  `element.instrumentIdentifier` and `element.urlConfig.index` from them at
  paint time. The prototype now does this. If a hook were ever needed, the
  helper could wrap `BaseInstrument.prototype` at runtime from our own file,
  since it loads after `BaseInstrument.js`. That works on whichever copy is
  loaded, stock or GlassOut's.
- **Stale-copy surface.** Stock `BaseInstrument.js` is 33,744 bytes of logic:
  screen state L:vars, flow events, the update loop. EFB and UI pages load it
  too (`fs-base-efb`, `fs-base-ui`, not only `asobo-vcockpits-core`). A stale
  copy after an update would break gauges in every aircraft and the EFB. A
  stale `VCockpit.html` is one declarative page, one line off stock, used by
  panels only.
- **Behaviour with GlassOut installed.** This decides it. GlassOut's
  `zzz-glassout-hook` overrides `BaseInstrument.js` and loads last. Its engine
  rewrites the hook on boot and warns about competing overrides. We would
  either lose the file to it, so Taxi Cam identification would silently stop
  for every GlassOut user (the users in issues 171/172), or win it, which
  breaks GlassOut. Editing GlassOut's files is ruled out. Neither outcome is
  acceptable.

With the `VCockpit.html` choice, GlassOut installed changes nothing for Taxi
Cam:

- Our page loads `BaseInstrument.js` by path and gets GlassOut's copy.
- Our helper reads gauge elements the same way on either copy.
- The two overrides are different files, so neither package needs the other.

What stays to check live (E7):
- whether GlassOut's competing-override warning also looks at
  `Pages/VCockpit/Core/VCockpit.html` (its docs don't say);
- whether its markers and ours ever overlap in a frame. Ours are centred in
  gauges and theirs are at the gauge origin.

If GlassOut ever overrides `VCockpit.html` too, it loads last and wins. Taxi
Cam then finds no marks, falls back, and says so. We don't fight for the file.

### Other load points

| Load point | Problem |
|---|---|
| `Instruments/Shared/BaseInstrument.js` | See above. Loses to GlassOut (or breaks it), and has the largest stale-copy surface. |
| `Core/VCockpit.js` | 23.5 KB of logic. A stale copy would break more, and Asobo is more likely to change it. |
| `/JS/common.js`, `simvar.js`, `avionics.js`, `GenericDataListener.js` | Every UI page loads them (toolbar, EFB, menus), so the helper would run everywhere and a stale copy would hurt everywhere. |
| A gauge added to each aircraft's `panel.cfg` | Edits third-party aircraft files, and aircraft updates overwrite it. |

**What could collide with it.** Any package that also overrides
`Pages/VCockpit/Core/VCockpit.html`; the last package loaded wins. On this
machine none of the ~25 Community packages does: I compared 8,325 add-on
`html_ui` files with 4,963 stock ones, and the only override of a stock file
is GlassOut's `BaseInstrument.js`. Candidates in future:

- a later GlassOut version;
- other panel streamers or "inject into every instrument" mods;
- an aircraft that ships its own `VCockpit.html` (none seen).

To detect a collision, the companion scans Community `layout.json` files for
the path and the bridge finds no marks. Then Taxi Cam falls back (below) and
says why in the bug report. It never fights for the file: we don't rename
the package to load later.

**Manifest.** The package declares the file in
`globally_overriden_base_sim_files`, the same way GlassOut's manifest does.
I found no public documentation of that field
(search on 2026-11-03), so whether MSFS needs it is experiment E0.

## What the page knows: the first open question

The page knows `sName`, which the stock debug button sets to `"VCockpit01"`:
the **section** name. It isn't known yet whether the page also gets
`texture=`. Section names aren't unique in MSFS 2024, because every
attachment has its own `panel.cfg`. For example, the Fenix
`Part_Interior_Panel_Refueling_Readouts/panel/panel.cfg` has its own
`[VCockpit17]` (`texture=Panel_Refueling`), which explains the table's two
`VCockpit17` panels.

What the result of E1 would mean:

- **The page gets `texture=`:** the mark carries it, and the panel-table
  decode can go.
- **Section only:** the mark carries `sName` plus a hash of `sConfigFile` (the
  attachment's `panel/` folder). The companion reads `texture=` for that
  section from the `panel.cfg` next to it. If that file isn't readable
  (encrypted packages), the panel table is used as a name lookup by section
  only, never by order.

In both cases order pairing is gone.

A second detail for E1 is gauge parameters. The A350 EFIS has the same URL on
both sides and differs only in the parameter after the rectangle
(`…wasm_gauge=EFIS,0,0,1644,1024,L` / `R`). The texture name separates those
two, but for gauges it needs to be known whether the page gets the parameter.
The prototype's panel block lists the panel data's keys and a gauge's keys,
and the gauge blocks carry every string field of the gauge.

## Mark format

The format is in `src/shared/panel_marks.hpp`; the JS mirror is
`src/panel_marks/taxi-cam-marks.js`.

**Cells.** A block is a grid of square cells, 16 wide, with a one-cell black
border. Each cell is one of the 8 colours whose channels are all 0 or 255, so
it carries 3 bits. 0 and 255 are fixed points of sRGB and of any other
transfer curve. Scaling blurs only cell edges. The decoder samples 9 points
around each cell's centre and needs 5 to agree. A channel counts as 1 at
≥ 170 and as 0 at ≤ 85.

**Rows.**
- Row 0 is a fixed finder of 16 colours (`7,0,7,0,4,2,1,6,3,5,0,7,4,0,1,7`).
  It gives the start and the horizontal pitch.
- Column 0 of every later row alternates black and white. That timing column
  gives the vertical pitch and the row count.
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
| panel name | u8 length + ASCII |
| gauge name | u8 length + ASCII |
| info | u8 length + ASCII |

- The gauge name is `wasm_gauge`, or the page's file name.
- The info text is the rest of the URL and the gauge's other string fields.
  It is cut short first when the block runs out of room.

**Size.** A 777 DU block is 8 rows. With 10 px cells on a 2048 panel it is
180×100 px; on a 300 px ISIS it is 72×40 px. The cell size is
`clamp(round(min(panel w, h) / 200), 4, 10)` page px.

**Scale.** The decoder measures the pitch. The texture rectangle is the page
rectangle × texture size / panel size, and the measured `pitch / cell`
cross-checks that scale.

## Placement and timing (coexisting with GlassOut)

- **Where.** Each gauge's block is centred in the gauge, and a gauge smaller
  than its block plus one cell all round gets none (the 777's 4×4 `Core`
  gauge). No block ever touches a gauge's top-left corner, where GlassOut
  reads its two verification pixels. One panel-level block goes bottom right,
  only where it overlaps no gauge block.
- **When.** Only on request. The bridge sets `L:TAXICAM_MARKS` to a new
  non-zero value through SimConnect, and every page polls it every 500 ms.
  - A change shows the blocks for 4 s, then they are removed.
  - The first read on a page only notes the value, so a request made before
    the page loaded is not for it.
  - `-1` turns the helper off for the page's life.
  - Without Taxi Cam nothing is ever painted, so GlassOut-only users see
    nothing.
- **Guarded.** Every step is in try/catch, and an exception removes what was
  drawn. The helper can't stop a page loading.
- **Visibility.** The blocks *are* visible in the cockpit for those 4 s, after
  each flight load and after a re-identify. They are small, saturated
  rectangles; say so in the docs and the changelog.

The prototype adds a load-time window for experiments (`LOAD_SHOW_MS`, set by
`make-package.ps1 -LoadShowMs`). Production uses only the request.

## Bridge detection path

1. **Candidates.** Every panel-class texture (multi-mip RGBA render target).
   Discovery already admits these, `panel_texture_desc` in phase 1, so this
   works for built-in and unknown aircraft alike.
2. **Request.** When the flight is loaded and panel textures exist, the bridge
   bumps `L:TAXICAM_MARKS`. Then, for up to 4 s, it snapshots each candidate
   through the existing machinery: a base-mip copy at a proven submission
   boundary, then mapping the readback. The full-size bytes go through
   `panel_marks::find_marks` before they are shrunk. The prototype already
   does this for every picture and logs `Display marks:` / `Display mark:`.
3. **Map.**
   - (panel name, gauge index) → (texture id, rect in texture px). Gauge name
     and info are stored for display.
   - A texture with no marks stays unidentified.
   - Two textures with the same panel name keep the newer one (GlassOut does
     the same). This is logged.
4. **Between requests,** `follow_copies` keeps a mapped texture across MSFS's
   one-to-one copies (FBW A380X, FSL), which is cheap.
5. **Request again** when:
   - a mapped texture dies with no copy to follow;
   - a routed name is missing;
   - the user opens Displays or setup;
   - Settings name a panel that the current map doesn't have.
6. **Cost.**
   - Offline, decoding a 2048² RGBA texture takes about 0.1 s on this machine
     (≈ 0.1–0.14 s for the whole tool run, including reading a 16 MB file).
   - The sweep runs on the bridge control thread, never on a hook path.
   - Snapshots are sequential, ≈ 0.4 s each by earlier measurements, so 17
     panels ≈ 7 s. That is longer than a 4 s window, so the sweep requests
     again per batch, or the window grows to cover the candidates. Tuned in
     E2–E6.
   - Live `read_us` is in the log line.

## What it replaces

| Today | With marks |
|---|---|
| `propose`, `resolve_names`, shift ≤ 8, `PanelOrder::section` | Deleted. The mark carries the name. |
| `resolve_by_shape`, `drop_copy_destinations`, `trim_trailing_ui` | Deleted. |
| `ProcessTables`, `aircraft_signature`, clean-load learning, layout anchors, catalog `panel_shapes` | Deleted. Nothing is learned or persisted about textures. |
| `RenderTargetSequence` creation-burst ring (pairing input) | Kept only if other diagnostics need it. |
| Panel table decode (`read_panels`, `panel_registry_contract`, `resolve_panel_table`) | Gone if E1 shows the page has `texture=`. Otherwise kept as a name lookup by section (or replaced by reading `panel.cfg` from `sConfigFile`'s folder). |
| Profile display rects (777 DUS sub-rects) and guided setup's default areas | Default to the gauge rectangle from the mark. |
| Guided setup "pick by picture" | Stays as the UI for choosing *which* display is left, right or lower. Pictures now carry trustworthy names, and picks are saved as panel + gauge names. |
| `follow_copies` | Kept for continuity between requests. |
| FBW A380X twin PFD swap | Fixed: each page names itself. |

**Fallback while marks are unavailable** (package missing, overridden, stale
or off): the current pairing stays for one or two releases. The bridge logs
`marks=unavailable reason=…` and the bug report carries it. Deleting the old
pairing is a separate PR, once E1–E7 pass.

## Install and update

- **Make the page from the user's own stock file, never ship one.** The
  companion does it at start (C++, not PowerShell: the installer AV history),
  and so does the installer.
  1. Find the stock file next to the running sim: `<exe dir>\Packages\
     asobo-vcockpits-core\html_ui\Pages\VCockpit\Core\VCockpit.html`. That is
     the same layout on Store; on Steam it is unverified.
  2. Require exactly one `VCockpit.js` script line and insert ours after it.
     Anything else means the page changed shape, so write nothing and report
     "marks unavailable: stock page changed".
  3. Write `Community\taxi-cam-panel-marks\` with `manifest.json`,
     `layout.json` (sizes, FILETIME dates), the page, the helper, and
     `taxi-cam-marks.stock.txt` (stock SHA-256 and path).
  4. Community is found through `UserCfg.opt` `InstalledPackagesPath`, the
     same roots `startup_xml.cpp` uses for `exe.xml`.
- **Keep it in step.**
  - At every companion start and when MSFS exits, compare the stock hash with
    the recorded one. If it differs, rebuild. A rebuild while MSFS runs takes
    effect at the next start.
  - **The stale window** is the first sim start after an update that changed
    `VCockpit.html`, if the companion starts after MSFS mounts packages
    (unknown, E8). In that window our copy lacks whatever line Asobo added.
  - How safe that is:
    - The copy differs from stock by one line.
    - The page is tiny and declarative.
    - The helper is optional.
    - The failure mode is the old page for one session, not a missing page.
    - If an update changes the page's shape, step 2 refuses, and the rebuild
      removes the override instead of keeping a stale one.
- **Remove.** The uninstaller and an Options switch delete the package, but
  only a folder holding our marker file.
- **Steam.** Same steps with the Steam `UserCfg.opt` and exe folder. This is
  unverified; the dev box is Store only.

## Risks and unknowns (each has an experiment)

| # | Question | Why it matters |
|---|---|---|
| E0 | Does MSFS load an override of `VCockpit.html` from a Community package, and does it need `globally_overriden_base_sim_files`? | Whole design |
| E1 | Which panel and gauge fields does the page get? Is `texture=` among them? | Whether the panel-table decode can go |
| E2–E6 | Do DOM blocks drawn over WASM / NanoVG gauges reach the panel texture, unscaled or at what scale, in all five aircraft? Do hidden or `quality=hidden` panels draw them? | Detection |
| E7 | GlassOut active: both tools still identify, GlassOut shows no `MISMATCH`, nothing new in its log | Coexistence |
| E8 | Does the companion run before MSFS mounts Community? | Stale-copy window |
| E9 | Do the 4 s blocks show in a way users mind (cockpit view at flight load)? | UX |

Also open: panels updating only when visible (blocks wait for the next draw),
and aircraft that put a full-page element above `document.body` children
(none seen).

## Done locally (2026-11-03)

- `tests/graphics/panel_marks_test.cpp`: 62 checks pass.
  - The JS and C++ encoders agree byte for byte (golden vector from node).
  - Round trips on a busy synthetic PFD at 1×.
  - The 777 `DUS` layout (4 DUs) shown in 2048, 1600 and 800 textures with
    bilinear filtering and 2 px corner markers at each gauge origin; every
    gauge rect is scaled right.
  - A350 EFIS in BGRA, and BGRA read as RGBA is refused.
  - The info text is capped.
  - A damaged cell fails the CRC.
  - No false blocks on a busy display.
- `tools/panel-marks/stub-page.js` + `decode_raw.cpp` run the real helper
  against a stub DOM with 777 DUS, A350 EFIS, A350 ISIS and FBW PFD panel
  data (`fixtures/`), rasterise its canvases at texture scales 0.5, 1 and
  1.25, and decode them. The fixtures use `VCockpit.js`'s field names. The
  A350 fixture's `sParameters` is a made-up name for the after-rectangle
  parameter, used to exercise the gauge info path.
  - Every gauge and panel block is found with the right names and rects.
  - The 777 `Core` gauge is skipped.
- `tools/panel-marks/make-package.ps1` built the package from the installed
  stock page into a scratch folder: one line added, manifest, layout, stock
  hash; `-Check` reports it current. It has not been written to the real
  Community folder.
- The bridge decodes marks in every ready display snapshot and logs them.
  The plain `build.ps1` is clean. `-Validate` and the smoke test have not been
  run, and the build is not installed.

## Experiment plan (fresh flight loads only; never reload or switch aircraft in a flight)

**Setup, once.** Get the owner's OK first, because it writes to the real
Community folder and needs an MSFS restart.

1. Close MSFS.
2. Install the prototype build (plain `build.ps1` + the usual install).
3. Run `tools/panel-marks/make-package.ps1 -LoadShowMs 120000`, which shows
   the blocks for 2 minutes after each panel loads.
4. Start MSFS.

**Per aircraft** (A350, 777-300ER, Fenix A320, FSL A321, FBW A380X): one fresh
flight load from the main menu.

1. Within 2 minutes of the cockpit appearing, open Taxi Cam → Displays (or
   Set up aircraft) so pictures cycle through every candidate.
2. Note the cockpit by eye: are blocks visible on each display?
3. After the flight, read `bridge.log`:
   - `Display mark: … kind=panel … info=K=…;I=…` answers E1, the page's
     fields. Is there a texture field?
   - `Display marks: id=… found=N read_us=…` per texture. Expect N ≥ 1 on
     every drawn display texture and 0 on non-panel textures. `read_us` gives
     the cost.
   - `texture_rect`, `pitch` and `cell` against the texture size give the
     scale, and the 777 DU rects against `panel.cfg`.
   - Compare with `Display identity (proposed)` and the snapshots: does every
     marked name sit on the texture the user would pick? Count disagreements;
     the A380X twin PFDs and the Fenix boot are the interesting ones.

**Pass:**
- every display Taxi Cam routes on these five aircraft is found by name in
  each load;
- no mark is decoded on the wrong texture;
- the decode cost is well under the snapshot cost.

**E7, GlassOut.** Repeat the A350 load with the GlassOut demo on and the
`efis-1` panel open. Look for GlassOut `MISMATCH` lines or a black panel
while blocks show.

**E8.** Log the companion start time against MSFS's package-mount marker in
its own logs, if there is one.

**Afterwards:** `make-package.ps1 -Remove`, or keep it for the next phase.

**Next phase, if E0–E2 pass.**
- The companion makes and checks the package.
- `L:TAXICAM_MARKS` request plus the bridge sweep.
- Routing from the mark map, behind the old pairing as fallback.
- Then a PR deleting the order-pairing code.
