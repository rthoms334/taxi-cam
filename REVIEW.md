# Review rules

These rules set what a pull-request review reports and how severe each finding is. The automated reviewer ([AI review](.github/workflows/pr-review.yml)) reads this file from `main`, never from the pull request under review, so a change to it takes effect only after it is merged. A change to this file is first replayed against past incidents by the [reviewer evals](.github/workflows/review-evals.yml). Human reviewers use the same rules. The full pull-request process is in [docs/pull-requests.md](docs/pull-requests.md).

## Severity

### Important: answer before merge

A defect that this change introduces, or makes reachable, and that would ship. Every Important finding needs an answer: fix it, or reply with the reason it is not a defect and resolve the conversation.

- **Simulator stability.** The bridge DLL runs inside `FlightSimulator2024.exe`, so a crash, hang or stall there takes the simulator down for every user. Examples: a simulator thread that waits on a bridge lock without `BoundedLock` and a budget ([render-thread freeze guards](docs/render-thread-freeze-guards.md)); a GPU resource reused or released before the fence that covers it completes; a queue `Wait` without its `Signal`; hook removal or unload without quiescence ([hooks](docs/hooks.md)); an exception or unchecked failure crossing a hook boundary.
- **Simulator memory and identity.** A read or write of simulator memory that skips the existing access guards, bounds, rereads or identity proofs; a fixed build address, offset or package version used instead of a resolved contract ([dynamic camera compatibility](docs/dynamic-camera-compatibility.md)); a refusal path weakened so the bridge guesses instead of refusing.
- **Lifecycle.** State not reset on a flight, aircraft, session or device change; a stale generation, epoch or identity accepted; a camera pair, view or GPU lease leaked, released twice or used after retirement ([camera ownership](docs/camera-ownership.md)); a race between a bridge worker and a simulator thread with a concrete interleaving.
- **The user's system and data.** Saved settings or calibration overwritten without the user's choice; Setup, uninstall or `--setup` commands that lose settings, break rollback or edit `exe.xml` unsafely; the updater trusting a download it has not verified ([releases](docs/releases.md)).
- **Release integrity.** A release version that is not strictly higher than the last one, or that disagrees with `changelog.json`; a historical receipt rewritten; a pinned dependency or action changed without its hash or version; a pull-request workflow gaining write access or the ability to publish; a workflow job or commit status reusing the name of a required check (**PR test build**, **AI review**, **Risk gate**) outside the workflow that owns it, or a change that lets the review or risk gate pass without doing its work.
- **Claims without evidence.** Code comments, docs, the changelog or the pull-request description stating simulator behaviour (rendering, targeting, lighting, motion, performance or compatibility) that only local, WARP or fixture tests support. Local results and in-simulator observations stay separate ([AGENTS.md](AGENTS.md)).
- **Tests.** Changed behaviour with no test where the area already has a fixture ([tests](tests/README.md)), or a test weakened, skipped or removed to make a change pass.
- **Logic errors** with a concrete failure: wrong condition, off-by-one, unit or sign error, overflow, unhandled failure, copy-paste mistake.

### Nit: worth fixing, never blocking on its own

Report at most five. Small clarity problems, misleading names or comments, avoidable duplication, and documentation errors without user impact. A nit still gets an answer (a fix, or a one-line reason) before its conversation is resolved.

### Pre-existing: report, don't block

An Important-grade defect in code this change touches or depends on that the change did not introduce. It is listed in the review summary and does not need an answer on this pull request.

## Do not report

- Formatting, include order or naming taste. The root `.clang-format` governs C++ formatting.
- Missing comments or documentation for self-evident code.
- A concern without a concrete failure path traced through the code ("this could race" without the interleaving).
- Anything the pinned compiler, its warnings or the existing validation already enforces.
- Tuning values (budgets, thresholds, timings, rates) unless the change contradicts the documented value or its reason.
- Requests to restructure, generalize or add abstractions.
- Third-party notices and licence texts, and the absence of copyright or SPDX headers on original files (the project adds none).
- Aircraft Lvar and material identifiers that look unusual: they must stay exact for compatibility.

## Facts reviewers get wrong

- `taxi-cam.exe` runs outside the simulator. `taxi-camera-bridge.dll` runs inside it: code in `src/bridge`, `src/graphics`, `src/hooks`, `src/camera` and `src/shared` runs on simulator threads unless shown otherwise.
- Simulator threads never block on a bridge lock. They take `BoundedLock` with a budget and skip their work for the frame when it expires. Bridge-owned threads may block.
- Refusing is the safe outcome. When evidence is incomplete the bridge refuses (no injection, no write) rather than guessing. A change that turns a refusal into a best guess is a defect even if it makes more aircraft work.
- `changelog.json`'s first entry is the release version. Installed companions fetch `changelog.json` and `docs/whats-new/` from `main`, so merging changes there reaches users immediately.
- WARP and local GPU validation do not establish live simulator behaviour.
- Build products, downloaded dependencies, captures and logs belong in the ignored `build/` directory and are never committed.

## Where to look

| Change in | Read first |
| --- | --- |
| `src/graphics/`, `src/hooks/`, `src/bridge/` | [Render-thread freeze guards](docs/render-thread-freeze-guards.md), [hooks](docs/hooks.md), [capture manager](docs/scene-capture-manager.md), [capture validation](docs/scene-capture-validation.md) |
| `src/camera/` | [Camera ownership](docs/camera-ownership.md), [camera entry contract](docs/camera-entry-contract.md), [dynamic camera compatibility](docs/dynamic-camera-compatibility.md) |
| `src/setup/`, `installer/`, `src/app/updater.*` | [Releases](docs/releases.md), especially Saved settings |
| `tests/` | [Tests](tests/README.md) |
| Anything else | [Architecture](docs/architecture.md), [repository structure](docs/repository-structure.md) |
