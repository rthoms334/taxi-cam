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

## Pull requests

The process is in `docs/pull-requests.md`; review severity rules are in `REVIEW.md`.

- Fill in the pull-request template. Put in-simulator observations under **Live result** and local results under **Tests**; write "Not run" rather than leaving a live result implied.
- Answer every AI review thread: fix the finding, or reply with the specific reason it is not a defect. Resolve a thread only after answering it. Pre-existing findings in the summary need no answer.
- Never approve or attempt to approve the `pr-approval`, `pr-live-check` or `release` environments, and never post approvals of any kind. Approvals are the owner's.
- Do not change `REVIEW.md`, `.github/review/`, the review workflows or the risk gate to make a pull request pass. Reviewer changes go in their own pull request.
