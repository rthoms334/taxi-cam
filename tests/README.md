# Native GPU validation

Run `build.ps1 -Validate` from the repository root with the pinned toolchain. It builds the companion and bridge, runs the native graphics checks on hardware and WARP, and runs the state, lifecycle, telemetry, startup and update tests. `-WarpOnly` explicitly skips hardware checks for hosted CI. Run `smoke-test.ps1` afterward to verify the exact executable and DLL covered by the build receipt.

`tests/graphics/graphics_validation.cpp` exercises the native bridge. `tests/graphics/compositor_main.cpp` independently checks the compositor across input formats and exposure settings. Shared GPU fixtures and pixel oracles live in `tests/support/`. The checks cover camera composition and lower-display preservation, native graphics state replay, pre-existing graphics objects, predicate guards and D3D11On12 coexistence. Readback is confined to validation. The optional D3D12 debug layer is used when available; a passing run without it does not establish debug-layer coverage.

The native validation run includes `tests/app/camera_hotkeys_test.cpp`: Ctrl + Shift + L / R / B defaults, saved combinations, editor reset and conflict handling, hidden-window dispatch, and synchronization with supported aircraft TAXI controls. Profile checks cover the iniBuilds A380's manual-only controls and separate saved calibration. These fixtures do not operate a live aircraft's buttons.

`tests/app/startup_state_test.cpp` checks first-launch Settings visibility, later tray starts, explicit manual opens, failed state writes and preview isolation using a private fixture directory. `tests/app/camera_status_test.cpp` verifies that stopped cameras and pending recovery do not report a generic frame wait, while recovered cameras stop showing historical temporary failures.

`tests/camera/manager_inspection_test.cpp` checks every manager-chain read and reread, field changes and identity refusals. `tests/camera/scene_recovery_test.cpp` covers temporary pair and manager failures followed by guarded cleanup and bounded retry, plus fatal identity refusal across later failures. These checks do not establish the cause of an individual simulator memory-read failure.

Additional focused checks remain available:

- `tests/app/updater_test.ps1`: selection of current and legacy installer filenames, exact release URLs, version ordering, duplicate rejection and digest/checksum validation.
- `tests/graphics/scene_capture_build.ps1`: capture packet pixels, producer/consumer fences and storage reuse; see [capture contracts](../docs/scene-capture-validation.md).
- `tests/graphics/scene_capture_manager_test.ps1`: recording lifetime, submission receipts and capture publication.
- `tests/graphics/scene_frame_output_test.ps1`: stable output composition and synchronization.
- `tests/hooks/render_boundary_test.ps1`: native render-boundary observations.
- `tests/installer/package_test.ps1`: exact five-file runtime inventory, validated binary hashes, and verbatim `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt` contents.
- `tests/installer/install_test.ps1`: isolated installation, calibration preservation, startup registration, required legal files and rollback of existing legal text on failure.
- `tests/installer/test-installer.ps1 -Installer <setup-path>`: checks the compiled installer and matching receipt through installation, update, rollback, uninstall and short-path fixtures, including licence and third-party notice handling.
- `tests/diagnostics/test.ps1`: interface-inventory fixtures, real LLVM decoding and wrong-process refusal. See [diagnostic tools](../tools/diagnostics/README.md).

These checks do not establish live simulator compatibility, texture targeting, camera placement, lighting, motion or performance. Simulator observations must be recorded separately from local GPU and UI results.
