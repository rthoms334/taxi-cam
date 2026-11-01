# Taxi Cam development

This is an independent Windows native project. Keep project source, builds and documentation in this repository; do not edit or depend on an aircraft source checkout.

- Keep external aircraft Lvar and material identifiers exact where required for compatibility. They are not project branding.
- Do not add inherited organization copyright or SPDX headers to original project files. Preserve upstream notices on third-party code and dependencies.
- Build with the pinned toolchain in `dependencies.json` through `build.ps1`; use the root `.clang-format` for C++.
- Run the smallest relevant checks first. Native delivery changes require `build.ps1 -Validate` and `smoke-test.ps1` for the exact executable and DLL before installation.
- Build products, downloaded dependencies, process captures, logs and historical binary archives belong in ignored `build/` directories. Never rewrite historical release receipts to describe a different source tree or binary.
- Separate local test results from in-simulator observations. Keep unresolved rendering, targeting, lighting and motion issues explicit.
- Preserve native identity, memory bounds, lifecycle and GPU synchronization guards. Graphics resources and public SimConnect telemetry have different ownership and update contracts.
- The installed DLL is locked while MSFS runs. Do not stop the simulator or overwrite user calibration without authorization; build and validate first.
- The default delivery is the native Windows tray companion and in-process graphics bridge.  Local GPU and UI validation does not establish live simulator behaviour.
- Work that needs Windows-native validation (`build.ps1`, `build.ps1 -Validate`, `smoke-test.ps1`, GPU or UI checks, in-simulator observation, reproducing bridge crashes or install/DLL problems) cannot run in the Linux cloud sandbox. Schedule it on a local Windows machine. Cloud or scheduled routines should limit themselves to triage, source review and reporting, and must say which findings still need local Windows confirmation.
