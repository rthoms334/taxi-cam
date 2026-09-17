# Native GPU validation

Run `build.ps1 -Validate` from the repository root with the pinned toolchain. It builds the companion and bridge, runs the native graphics checks on hardware and WARP, and runs the state, lifecycle, telemetry, startup and update tests. `-WarpOnly` explicitly skips hardware checks for hosted CI. Run `smoke-test.ps1` afterward to verify the exact executable and DLL covered by the build receipt.

`tests/graphics/graphics_validation.cpp` exercises the native bridge. `tests/graphics/compositor_main.cpp` independently checks the compositor across input formats and exposure settings. Shared GPU fixtures and pixel oracles live in `tests/support/`. The checks cover camera composition and lower-display preservation, native graphics state replay, pre-existing graphics objects, predicate guards and D3D11On12 coexistence. Readback is confined to validation. The optional D3D12 debug layer is used when available; a passing run without it does not establish debug-layer coverage.

The native validation run includes `tests/app/camera_hotkeys_test.cpp`: Ctrl + Shift + L / R / B defaults, saved combinations, editor reset and conflict handling, hidden-window dispatch, and synchronization with supported aircraft TAXI controls. Profile checks cover the iniBuilds A380's manual-only controls and separate saved calibration. These fixtures do not operate a live aircraft's buttons.

`tests/app/startup_state_test.cpp` checks first-launch Settings visibility, later tray starts, explicit manual opens, failed state writes and preview isolation using a private fixture directory. `tests/app/camera_status_test.cpp` verifies distinct status reporting for stopped cameras, pending recovery and successfully recovered cameras.

`tests/app/night_boost_migration_test.cpp` checks the one-time 8 EV preference migration in isolated profile files: all supported aircraft, preservation of calibration and unrelated preferences, legacy import, later user edits, migration revisions and failed-write retry. These CPU-only checks do not validate night rendering in MSFS.

`tests/camera/manager_inspection_test.cpp` checks every manager-chain read and reread, field changes and identity refusals. `tests/camera/scene_recovery_test.cpp` covers temporary pair and manager failures followed by guarded cleanup and bounded retry, startup calibration after an early inspection failure, and fatal identity refusal across later failures. These checks do not establish the cause of an individual simulator memory-read failure.

`tests/camera/local_memory_test.cpp` checks exact reads, access guards, bounded metadata caching and fresh endpoint validation. Large-allocation fixtures verify that overlapping suffix observations reuse a fresh endpoint query while retaining every original metadata comparison, independent queries for separate regions, rejection of conflicting observations and the original cache capacity. Query-count assertions are deterministic; diagnostic timings do not establish simulator FPS gains.

`tests/camera/private_page_memory_test.cpp` checks the production private-page mode: allocation identity, actual page protections, exact rereads and budgets, cross-page fields, cold-page fallback, mapped/COW/image refusal, changed or released memory, nested/thread-local scopes, and capacity fallback retaining earlier proofs. It explicitly distinguishes requested-page protection changes from unrelated unread-page changes. The original full-region and image-reader tests remain in the suite.

`tests/camera/image_page_memory_test.cpp` checks explicit image-page mode using isolated data pages in its own executable: exact main-module identity, bounds, COW image pages, guard/no-access/execute-only refusal, exact reads, both private/image cache-type rejection directions, endpoint changes, capacity fallback and nested/thread-local isolation. Default image readers and full-region scopes retain their original semantics. `tests/camera/writable_span_memory_test.cpp` checks the fresh AA flag-span validator, including cross-page and cross-allocation bounds, exact current `PAGE_READWRITE` protection, protection changes and all-or-nothing flag-read output. Both suites exercise unavailable-API and nonresident-page fallbacks through `TAXI_LOCAL_MEMORY_TESTING`; those fault controls are compiled out of production builds. The existing AA tests retain flag, override and closed-gate behavior checks.

The dynamic compatibility checks cover relocated instruction templates, cross-references, PE metadata and static RTTI identity with synthetic images. `tests/camera/camera_contract_test.cpp` exercises the complete generated model and rejection of incomplete or ambiguous evidence. These tests do not establish compatibility with an unobserved simulator build.

`tests/camera/retained_profile_test.cpp` covers empty startup cancellation, pending native operations and preservation of complete or partial owned pairs. `tests/camera/runtime_test.cpp` exercises the production request API on a wrong-host executable, including a queued empty stop followed by an aircraft-session transition. Mixed DLSS render/display sizes retain independent bounds in `tests/camera/view_resize_test.cpp`.

`tests/camera/render_schedule_test.cpp` covers 5–60 activation budgets, simulator cadence limits, uneven update intervals, live rate/feed changes and mandatory closing intervals. Settings, IPC, saved calibration and hidden UI checks include 5 and 10 while preserving the default of 15. Existing scene-demand tests retain bounded startup warmup and repeated OFF/ON ownership checks.

`tests/graphics/metadata_batch_bridge_test.cpp` exercises production lookup caching, idle forwarding, fresh PFD recording requirements, lifetime/address reuse and descriptor discovery. Its isolated CPU timing comparison does not establish simulator FPS. `tests/graphics/source_observation_test.cpp` checks combined draw tracking and idle capture suppression while retaining source-state evidence, recorded consumers and source leases. Frame-output and queue-tail tests cover optional GPU timestamps, completion-fence readback, unsubmitted discard and disabling diagnostics with a measurement pending.

The on-demand [performance sampler](../tools/performance/README.md) has separate isolated checks and does not become part of the installed runtime. Live simulator measurements remain a separate acceptance step.

Additional focused checks remain available:

- `tests/app/updater_test.ps1`: selection of current and legacy installer filenames, exact release URLs, version ordering, duplicate rejection and digest/checksum validation.
- `tests/graphics/scene_capture_build.ps1`: capture packet pixels, producer/consumer fences and storage reuse; see [capture contracts](../docs/scene-capture-validation.md).
- `tests/graphics/scene_capture_manager_test.ps1`: recording lifetime, submission receipts and capture publication.
- `tests/graphics/scene_frame_output_test.ps1`: stable output composition and synchronization.
- `tests/graphics/reshade_validation.ps1 -ReShadeDll <trusted-local-dll>`: actual proxy forwarding, native device identity and PFD pixel preservation in isolated hardware/WARP harnesses; see [ReShade compatibility](../docs/reshade-compatibility.md).
- `tests/hooks/render_boundary_test.ps1`: native render-boundary observations.
- `tests/installer/package_test.ps1`: exact five-file runtime inventory, validated binary hashes, and verbatim `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt` contents.
- `tests/installer/exe_xml_test.ps1`: strict launch parsing, opt-in repair of the recognized mislabeled launch header, exact-original backups, add-on/comment preservation, idempotence, concurrency/encryption refusal and physical-path visibility checks before XML writes.
- `tests/installer/install_test.ps1`: isolated installation, calibration preservation, Steam-header repair, simulator-specific XML inheritance, ambiguous-discovery fallback, required legal files and rollback of existing legal text on failure.
- `tests/installer/wizard_startup_selection_test.ps1`: compiled wizard argument fixtures for inherited versus explicit/edited XML choices, silent simulator overrides and the final installation guard. The recorder refuses before installation and leaves fixture XML files unchanged.
- `tests/installer/test-installer.ps1 -Installer <setup-path>`: checks the compiled installer and matching receipt through installation, update, rollback, uninstall and short-path fixtures, including automatic-startup retry on a later Setup attempt, safe manual fallback, concurrent edits, calibration preservation and legal notices. Successful installation scenarios use an isolated test identity and fixture directories; they do not verify MSFS auto-launch.
- `tests/diagnostics/test.ps1`: interface-inventory fixtures, real LLVM decoding and wrong-process refusal. See [diagnostic tools](../tools/diagnostics/README.md).
- `tests/diagnostics/benchmark_compare_test.ps1`: bridge-log parser fixtures and the 0.9.8 versus current-tree hot-path identity check. See [version benchmark](../docs/version-benchmark.md). Isolated Windows timings and live MSFS FPS are separate.

These checks do not establish live simulator compatibility, texture targeting, camera placement, lighting, motion or performance. Simulator observations must be recorded separately from local GPU and UI results.
