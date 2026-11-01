# Windows builds and approved releases

The [Windows build and release workflow](../.github/workflows/release.yml) runs on pushes to `main` that change application source, tests, diagnostic tools, installer or CI code, the root build scripts, dependency configuration, `changelog.json` (the release version), camera defaults, the project licence, bundled runtime notices, or the release workflow itself. A successful run retains a downloadable Windows x64 release candidate for checking. Publication waits for your approval in the same workflow run.

README, documentation, issue-template and other repository-only changes do not trigger an automatic build. Markdown files and formatting/ignore files are excluded even inside the code directories. A push containing both documentation and a qualifying code change still runs. Keep the workflow's `paths` list current when adding a new build input outside the listed directories.

The build workflow can also be started manually from GitHub Actions. Only successful builds from `main` can be published.

## Pull-request test builds

Every pull request to `main` runs the [Windows PR test build](../.github/workflows/pr-test-build.yml) workflow. It bootstraps the compiler pinned in `dependencies.json`, runs `build.ps1 -Validate -WarpOnly` and `smoke-test.ps1` against the exact `taxi-cam.exe` and `taxi-camera-bridge.dll` from that run, and uploads those binaries with `validation.json` and `SHA256SUMS.txt` as `windows-pr-test-build-<pr>-run-<number>-attempt-<attempt>`. That artifact is a test/PR build only: the job does not package an installer, does not run `ci/publish-release.ps1`, and cannot publish a GitHub release. PR binaries keep build number zero, the same as local builds, so a sideloaded test EXE cannot look newer than the last published `v*-build.N` just because Actions run numbers are shared across workflows.

This check does not wait for release-environment approval. It is one of the required checks in the [pull-request process](pull-requests.md), alongside the AI review and the risk gate. Publication remains the approval-gated job on the main-branch Windows build and release workflow.

## Download, check, then publish

1. Open the **Windows build and release** run in GitHub Actions. Wait for **Build and validate** to finish; **Publish release** will show **Waiting** for approval.
2. Download `windows-release-<build-number>-attempt-<attempt>` from that run's Artifacts section. Extract it and test the setup EXE in `packages/`. The runtime ZIP is beside it; `native/` contains the validated binaries and receipt.
3. Return to the same run and click **Review deployments**. Select **release**, then **Approve and deploy**.

The publish job downloads the exact candidate produced by the build job and checks its commit, build number and binary hashes before publishing to [GitHub Releases](https://github.com/rthoms334/taxi-cam/releases). It does not compile or repackage the application. There is no second workflow or run-ID form. If testing fails, reject the deployment or cancel the run.

The `release` repository environment must have **Required reviewers** enabled with `rthoms334` as reviewer. **Prevent self-review** is disabled so you can approve your own builds, and administrator bypass is disabled. These settings live in **Settings > Environments > release**, not in YAML; keep them enabled to preserve the gate. GitHub pauses the publish job before starting its runner until approval is granted.

Candidates expire after 30 days. If a candidate is missing or expired, build and check a new one. A full rerun creates a new candidate that needs checking and approval again. Rerunning only a failed publish job uses the original build job's artifact name, even though the workflow attempt number changes.

## Build pipeline

1. Check out the exact commit that triggered the run.
2. Download or restore the pinned compiler archive and verify its hash.
3. Compile the native EXE and DLL and run validation.
4. Test installation, rollback and package contents using isolated fixtures.
5. Create the minimal runtime ZIP and compile the Windows installer with pinned Inno Setup.
6. Test the installer using isolated fixtures and retain diagnostic artifacts.
7. Retain a release candidate, then wait for approval before the publish job starts.

The runner is `windows-2025`. Official actions are pinned to commit SHAs. The native compiler and Inno Setup compiler download are pinned in `dependencies.json` and verified before use.

The validation step runs:

~~~powershell
./build.ps1 -Validate -WarpOnly
./smoke-test.ps1
./tests/installer/package_test.ps1
~~~

`-WarpOnly` uses Windows' software Direct3D 12 renderer to exercise GPU capture, composition and PFD drawing. The receipt records WARP as `passed`, hardware GPU validation as `not-run` and `simulatorVerified: false`. Local `build.ps1 -Validate` runs both hardware and WARP tests.

`build.ps1 -Validate` also runs the installer's native checks: the `exe.xml`, prerequisite and settings-migration tests, and `tests/installer/setup_commands_test.ps1` against a test build of the setup commands.

Setup does not run PowerShell. Inno Setup copies the files and runs the companion's own installer commands hidden (`taxi-cam.exe --setup discover|check|configure|uninstall`; see [`src/setup/setup_commands.hpp`](../src/setup/setup_commands.hpp)). `check` refuses a running MSFS or companion and missing prerequisites before any file changes; `configure` adds the `exe.xml` startup entry and writes `installation.json` after the files are installed; the uninstaller's `uninstall` removes this installation's startup entries and, when chosen, saved settings.

After packaging and compiling setup, `tests/installer/test-installer.ps1 -Installer <setup-path>` verifies the exact installer and its receipt in isolated fixtures. Package checks require the complete five-file payload and byte-for-byte copies of the project licence and third-party notices. Installer checks cover those files during installation, update and removal, including default settings preservation, explicit reset/removal, calibration import and preservation, and the manual-startup fallback. Windows shell extraction checks verify the app and setup icons at small and large sizes.

## Saved settings

Installation and uninstallation **keep settings by default**, including unattended runs. Setup's **Keep existing settings** checkbox starts selected on every run. The first start of the companion writes `camera_rate=10` into existing known aircraft profile INIs (the shipped default; range 5–60, minimum 5) and records `camera_rate_revision=2` on `settings.ini`. Earlier versions of Setup applied the same one-shot migration with the same stamp, so people who already received it are not migrated again; people with the force-5 stamp (`camera_rate_revision=1`) are migrated once to 10. Later starts leave a user-changed rate (5/15/30) alone. Profiles reached through a junction or symbolic link are never changed. Calibration, mounts, hotkeys and other keys stay in place. Clearing the checkbox resets camera profiles, reference guides, display preferences, hotkeys and first-launch state, and uses the bundled camera defaults. Known profiles under the former `380 Taxi Cam` name are also cleared so they cannot be imported again. Uninstall offers **Keep settings** first, with **Remove saved settings** as the explicit alternative. Neither choice deletes logs or unrelated files.

For unattended operations, setup accepts `/RESETSETTINGS=1` and the uninstaller accepts `/REMOVESETTINGS=1`. Omitting these parameters preserves settings. Reset/removal requires all Taxi Cam companions and MSFS to be closed. Setup snapshots affected settings and restores them on failure; concurrent changes are retained and reported rather than overwritten by rollback.

## What to download

| Release asset | Contents |
| --- | --- |
| Windows x64 setup EXE | Guided installation, upgrade and uninstallation, with simulator startup integration |
| Windows x64 ZIP | EXE, DLL, camera defaults, GPLv3 licence and third-party runtime notices |
| `SHA256SUMS.txt` | SHA-256 checksums of setup and the ZIP |
| `validation.json` | Test coverage and exact binary hashes |

The runtime ZIP contains exactly five files in its application directory: `taxi-cam.exe`, `taxi-camera-bridge.dll`, `taxi-camera-mounts.cfg`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt`. It has no loose PowerShell scripts, documentation folders, build manifests or validation receipts. Use setup for a normal installation.

Original Taxi Cam code is licensed under GPLv3 only. Release notes link to the corresponding source archive at the exact built commit, including the build and installation scripts; GitHub also provides source archives for the release tag. Keep that source accessible to binary recipients. The installer places `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt` alongside the application, and third-party licensing remains separate.

Provenance and manifests remain beside the ZIP in the ignored build directory as `<package>.build-info.json` and `<package>.manifest.json`, and are retained in workflow artifacts. The installer adds its uninstaller and installation record, which are needed to manage future upgrades and removal. Build, test and installation scripts remain in the repository.

## Tags and release notes

The release version comes from [`changelog.json`](../changelog.json): every build uses the `version` of its first (newest) entry exactly, so the installed version, its What's new notes and the published tag always agree. To release a new version, add a new top entry with the higher version (see [What's new notes](#whats-new-notes)); merging it makes `main` build that version. Commits without a new entry rebuild the same version for testing, but **Publish release** refuses a version that is not higher than the last release tag on `main` (a rerun of an already-tagged commit is still allowed). This is required because the updater compares the version before the build number, and installed binaries carry the workflow run number rather than the tag's `build.<N>`. Git history and tags do not affect the version, so local builds, pull-request builds and GitHub Actions resolve the same value from the checked-out file. The build refuses a changelog whose versions are not strictly descending `major.minor.patch` values with each component at most 65535.

The generated header and Windows manifest live under `build/native/generated/`; the resolved version and build number are recorded in `build/native/version.json`. The app UI, executable version resources, installer and release title all use this resolved semantic version.

The approval-gated **Publish release** job is the only place that creates `v<application-version>-build.<N>` on the exact `main` commit being published (`gh release create` with that tag). `N` is one greater than the last matching tag on that commit's first-parent history, or `1` when `main` has no such tag. A rerun of the same commit reuses its existing tag. Pushes to `main` and pull-request CI must not create tags or increment that published build number. Workflow run numbers still name artifacts (`windows-release-<run>-attempt-<attempt>`), the compiled receipt and the runtime ZIP label; they are not the published tag. Only `main`-branch GitHub Actions runs stamp that workflow run number into the compiled binary and validation receipt. Local builds and pull-request test builds use build number zero. Packaging refuses a `build.N` label that differs from the compiled build number. Publication uses the already-stamped receipt; it does not rebuild or rewrite historical release receipts.

Installer assets use `taxi-cam-<version>-windows-x64-setup.exe`, such as `taxi-cam-0.9.0-windows-x64-setup.exe`. The published tag uses the next `build.<N>` from the last tag on `main`. The runtime ZIP name and validation receipts keep the workflow run number for artifact traceability. Existing published downloads keep their original names.

The updater asks GitHub's unauthenticated `/repos/rthoms334/taxi-cam/releases/latest` endpoint, then compares that published tag with the installed `TAXI_CAM_VERSION` and `TAXI_CAM_BUILD_NUMBER`. It ignores drafts, prereleases, Actions artifacts and unpublished `main` candidates. Version components and build numbers are compared numerically. It selects the exact installer filename for the release from the fixed project repository, prefers `taxi-cam-<version>-windows-x64-setup.exe`, and also recognizes `taxi-cam-<version>-build.<number>-windows-x64-setup.exe`. Downloads are bounded and verified before execution. Automatic checks run off the UI thread, with a manual tray action available; installing requires user confirmation and a closed simulator.

The updater uses GitHub's unauthenticated public release API. The release repository and its installer assets must be publicly readable; private repositories return HTTP 404 to this client. No GitHub credential is embedded in the application. Make the repository public and publish a release containing setup before automatic updates can be used by end users. Local installation and the isolated updater tests work independently of repository visibility.

Notes contain installation guidance, GPLv3 licence information, links to the exact corresponding source and build logs, validation scope, and a rollup of merged pull requests and first-parent headlines after the last release tag on that `main` commit's first-parent history. Unpublished builds after that tag are included. Duplicate pull-request numbers and conventional `ci:` / publish-workflow-only changes are omitted. The first release includes the available first-parent history. The updater continues to see only published GitHub releases for those tags, never pull-request artifacts or an untagged `main` build.

[The publication script](../ci/publish-release.ps1) creates the new tag from the exact built commit on `main`. It marks a release Latest only when that tag is the last `v*-build.N` tag reachable on `origin/main` (or `main`), matching the published GitHub release the updater already follows. Untagged `main` and pull-request CI artifacts are not a publish baseline.

## What's new notes

[`changelog.json`](../changelog.json) at the repository root holds the short, user-facing notes behind the companion's **What's new** window. They are written by hand and are separate from the generated GitHub release notes above.

- **What's new** is a link in the Settings sidebar. It opens by itself the first time Settings is shown after an update: the installed version differs from the last one whose notes were shown, recorded as `[whats_new] seen` in `settings.ini` (a settings reset shows them again).
- Opening it fetches `https://raw.githubusercontent.com/rthoms334/taxi-cam/main/changelog.json` off the UI thread: HTTPS only, no redirects, 64 KiB limit. If the fetch or the file fails, the status line says so. Feature pictures are then fetched one at a time from the same repository path (2 MiB each); a picture that cannot be loaded is left out.
- The window lists the releases at or below the installed version, newest first, so notes for merged but unpublished builds stay hidden. The newest release shows its features as cards with their pictures, then its fixes and other changes; earlier releases follow as compact lists.
- The `--preview` UI reads `changelog.json` and the pictures from the repository checkout it is started in, so new notes can be checked before they reach `main`.

To add notes, put a new object at the top of `releases`:

~~~json
{
  "version": "1.2.0",
  "date": "2026-11-01",
  "features": [
    {
      "title": "Set up any aircraft",
      "text": "One or two sentences on what it does and where to find it.",
      "image": "docs/whats-new/1.2.0/aircraft-setup.png"
    }
  ],
  "fixes": ["One short sentence per fixed problem."]
}
~~~

`version` is `major.minor.patch` and versions must be strictly descending. `date` (`YYYY-MM-DD`) is optional. A release needs at least one entry in `features` (up to 8), `fixes` or `changes` (up to 32 each). A feature has a `title` of at most 80 characters, a `text` of at most 400 and an optional `image`: a `.png` or `.jpg` path under `docs/whats-new/` that exists in the repository (keep pictures under 2 MiB; a window screenshot about 1000 pixels wide fits the card). `fixes` and `changes` hold non-empty strings of at most 400 characters; `changes` is the single list every release before 1.2.0 used. The top entry's `version` is the version every build is stamped with, so adding an entry is how a release version is chosen. Each published release needs its own higher entry; notes for changes merged before that release go into the current top entry.

The repository check (`tests/app/changelog_test.cpp` in `build.ps1 -Validate`) refuses unknown keys and missing pictures. The companion skips keys it does not know, so a field added later does not hide the notes from an installed 1.2.0 or newer. Companions before 1.2.0 refuse the `features` and `fixes` keys and report that What's new could not be read until they are updated.

Because the companion reads the file from `main`, wording can be corrected after a release without reinstalling. `changelog.json` is a build input, so any change to it starts the release workflow; publishing that candidate still needs approval.

## Failures and reruns

Build, validation or packaging failure prevents creation of a publishable candidate. Workflow artifacts retain logs and available package output for 30 days. The publish job depends on a successful build and only runs from `main` after environment approval.

Publication keeps the release in draft until all four assets are uploaded. Use **Re-run failed jobs** to retry a failed publication and finish an incomplete draft; approve the deployment again if prompted. If the release is already published, the script preserves its assets and returns its link. Rebuilding cannot replace an already published tag's assets.

Independent pushes each receive a build; they are not cancelled by a newer push. A newer push can arrive during publication, so the Latest check is not a transactional lock.

The build job uses read-only repository permissions. Only the approval-gated publish job uses `contents: write`; it also uses `actions: read` to retrieve the candidate from the same run. Both use GitHub's built-in token. No personal access token or additional repository secret is required.

CI validates the build and isolated rendering pipeline. Hardware GPU behaviour and live MSFS operation require their own checks.
