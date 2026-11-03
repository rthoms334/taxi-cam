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

## No comments

Write no comments in project source, tests, scripts, shaders, build files, installer files or configuration. Code says what it does through its names and structure. A rule that matters (lock order, GPU synchronization, lifetime, memory bounds, compatibility) is enforced by a test. If code seems to need a comment, it is too complex: rename, extract or simplify it until the comment is unnecessary. Do not move comments into `docs/` instead.

- `tests/repository/comments_test.ps1` (run by `build.ps1 -Validate`) fails on any comment outside its exceptions. Comments that predate the rule are counted per file in `tests/repository/comment_baseline.json`. Counts there only go down: after removing comments, run `tests/repository/comments_test.ps1 -LowerBaseline`. Never add a file or raise a count.
- The rule does not cover: tool directives that are written as comments (`// clang-format off` and `on`, `NOLINT`, `#requires`, a first-line `#!`), with nothing after them; upstream licence and copyright notices on third-party code, which go in the check's third-party list; `.github/`, `REVIEW.md`, `licenses/`, `LICENSE` and `THIRD_PARTY_NOTICES.md`; Markdown documentation; and anything under `build/`.
- Comment-like text inside strings is data, not a comment, but comments inside embedded shader or assembly source count.

## Pull requests

The process is in `docs/pull-requests.md`; review severity rules are in `REVIEW.md`.

- Turn on auto-merge right after opening a pull request (`gh pr merge <number> --auto --merge`) unless it changes anything under `.github/`: such a pull request can fake the **Risk gate** check, so the owner merges it after checking the real gate run. Before pushing any commit that touches `.github/` to a pull request, including one you did not open, turn auto-merge off with `gh pr merge <number> --disable-auto`; turning it off after the push can lose the race to a faked check.
- Fill in the pull-request template. Put in-simulator observations under **Live result** and local results under **Tests**; write "Not run" rather than leaving a live result implied.
- Answer every AI review thread: fix the finding, or reply with the specific reason it is not a defect. Resolve a thread only after answering it. Pre-existing findings in the summary need no answer.
- Never approve or attempt to approve the `pr-approval`, `pr-live-check` or `release` environments, and never post approvals of any kind. Approvals are the owner's.
- Do not change `REVIEW.md`, `.github/review/`, the review workflows or the risk gate to make a pull request pass. Reviewer changes go in their own pull request.
