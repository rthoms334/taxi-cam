# Repository structure

All application code lives under `src/`. The executable and DLL are built from these modules:

| Directory | Responsibility |
| --- | --- |
| `src/app/` | Windows tray app, settings UI, launcher, updater and application resources |
| `src/bridge/` | DLL startup, companion control loop and native Direct3D integration |
| `src/camera/` | Camera ownership, simulator memory and code checks, view lifecycle, mounts and SimConnect telemetry |
| `src/graphics/` | Capture, composition, display routing, exposure and GPU synchronization |
| `src/hooks/` | Camera-update and Direct3D observers, including the Win64 assembly thunk |
| `src/shared/` | App/bridge protocol, control policy and generated-version interface |
| `src/profiles/` | Aircraft integration metadata |

Supporting files are kept separate:

| Directory or file | Responsibility |
| --- | --- |
| `tests/` | App, camera, graphics, hook, installer and diagnostic tests; shared fixtures are in `tests/support/` |
| `tools/diagnostics/` | Optional interface-inventory, pose, material and telemetry tools for development |
| `installer/` | Inno Setup definition, package/build/bootstrap scripts and embedded install/uninstall helpers |
| `ci/` | Semantic version calculation, pinned toolchain helpers and release publication |
| `.github/workflows/` | GitHub Actions build and release workflow |
| `docs/` | Architecture, integration contracts and development documentation |
| `licenses/` | Required third-party source/runtime notices |
| `LICENSE` | Project copyright notice and GNU GPLv3-only terms |
| `build/` | Ignored binaries, toolchains, packages, validation results and historical captures |
| `version.json` | Version baseline; kept at the repository root so its Git history remains the version source |
| `dependencies.json` | Compiler and installer dependency pins |
| `taxi-camera-mounts.cfg` | Default mount configuration included in the runtime package |

The root `bootstrap.ps1`, `build.ps1` and `smoke-test.ps1` are the local build entry points. Package and installer commands are documented in [Releases](releases.md); focused checks are in [Tests](../tests/README.md).

Production code does not include files from `tests/` or `tools/`. Diagnostics reuse the camera contracts in `src/camera/`, while their inspection executables remain outside the shipping build. The installer contains the five required runtime files, including `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt`; development tools, tests, documentation folders and loose PowerShell scripts are not installed.

Obsolete probes bound to archived 0.7.x binaries and one-off debugger fixtures have been removed. Their source remains available in Git history. Existing local captures and build receipts from the former module directories are preserved under `build/archives/source-layout/` without changing their contents.
