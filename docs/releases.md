# Automated Windows releases

The [Windows release workflow](../.github/workflows/release.yml) runs on pushes to `main` that change application source, tests, diagnostic tools, installer or CI code, the root build scripts, dependency or version configuration, camera defaults, bundled runtime notices, or the release workflow itself. A successful run builds a Windows x64 package and publishes it with release notes at [GitHub Releases](https://github.com/rthoms334/taxi-cam/releases).

README, documentation, issue-template and other repository-only changes do not trigger an automatic release. Markdown files and formatting/ignore files are excluded even inside the code directories. A push containing both documentation and a qualifying code change still runs. Keep the workflow's `paths` list current when adding a new build input outside the listed directories.

The workflow can also be started manually from GitHub Actions on `main`.

## Build pipeline

1. Check out the exact commit that triggered the run.
2. Download or restore the pinned compiler archive and verify its hash.
3. Compile the native EXE and DLL and run validation.
4. Test installation, rollback and package contents using isolated fixtures.
5. Create the minimal runtime ZIP and compile the Windows installer with pinned Inno Setup.
6. Test the installer using isolated fixtures and retain diagnostic artifacts.
7. Create a draft release, upload its assets, then publish it.

The runner is `windows-2025`. Official actions are pinned to commit SHAs. The native compiler archive and Inno Setup compiler download are pinned in `dependencies.json` and verified before use.

The validation step runs:

~~~powershell
./build.ps1 -Validate -WarpOnly
./smoke-test.ps1
./tests/installer/install_test.ps1
./tests/installer/package_test.ps1
~~~

`-WarpOnly` uses Windows' software Direct3D 12 renderer to exercise GPU capture, composition and PFD drawing. The receipt records WARP as `passed`, hardware GPU validation as `not-run` and `simulatorVerified: false`. Local `build.ps1 -Validate` runs both hardware and WARP tests.

## What to download

| Release asset | Contents |
| --- | --- |
| Windows x64 setup EXE | Guided installation, upgrade and uninstallation, with simulator startup integration |
| Windows x64 ZIP | EXE, DLL, camera defaults and one required runtime notice file |
| `SHA256SUMS.txt` | SHA-256 checksums of setup and the ZIP |
| `validation.json` | Test coverage and exact binary hashes |

The runtime ZIP contains exactly four files in its application directory: `taxi-cam.exe`, `taxi-camera-bridge.dll`, `taxi-camera-mounts.cfg` and `THIRD_PARTY_NOTICES.txt`. It has no loose PowerShell scripts, documentation, license folders, build manifests or validation receipts. Use setup for a normal installation.

Provenance and manifests remain beside the ZIP in the ignored build directory as `<package>.build-info.json` and `<package>.manifest.json`, and are retained in workflow artifacts. The installer adds its uninstaller and installation record, which are needed to manage future upgrades and removal. Build, test and installation scripts remain in the repository.

## Tags and release notes

`version.json` defines the semantic version baseline, starting at `0.8.1`. The commit introducing that value uses the baseline exactly. Each subsequent first-parent commit on `main` adds one to its patch component: `0.8.1`, `0.8.2`, `0.8.3`, and so on. A merge counts once; a push containing several direct commits advances by their count. Documentation commits still count towards version calculation, but no longer trigger a release; the next code release may therefore skip patch numbers. Formatting the configuration without changing its version value does not reset the baseline.

For a minor or major release, deliberately change the baseline in `version.json` to the required version, such as `0.9.0` or `1.0.0`. That commit establishes a new baseline. Local builds and GitHub Actions calculate the same semantic version from the same complete Git history. A local, uncommitted baseline change uses its configured version. Shallow checkouts are refused because they cannot establish the version reliably.

The generated header and Windows manifest live under `build/native/generated/`; the resolved version and baseline commit are recorded in `build/native/version.json`. The app UI, executable version resources, installer and release title all use this resolved semantic version.

Tags retain `v<application-version>-build.<workflow-run-number>`, such as `v0.8.1-build.13`, so existing updaters continue to recognize releases. The build suffix identifies an artifact; it does not replace the advancing semantic version. The workflow run number is recorded in the validation receipt, while local builds use build number zero. Packaging refuses a `build.N` label that differs from the compiled build number.

Installer assets use `taxi-cam-<version>-build.<number>-windows-x64-setup.exe`. The updater compares version components and build numbers numerically, ignores drafts and prereleases, and requires the matching installer asset from the fixed project release repository. Downloads are bounded and verified before execution. Automatic checks run off the UI thread, with a manual tray action available; installing requires user confirmation and a closed simulator.

The updater uses GitHub's unauthenticated public release API. The release repository and its installer assets must be publicly readable; private repositories return HTTP 404 to this client. No GitHub credential is embedded in the application. Make the repository public and publish a release containing setup before automatic updates can be used by end users. Local installation and the isolated updater tests work independently of repository visibility.

Notes contain installation guidance, validation scope, a link to build logs and commits since the most recent published ancestor build. GitHub's generated notes add pull-request and contributor information. The first release includes the available commit history.

[The publication script](../ci/publish-release.ps1) targets the exact built commit. It marks a release Latest only when that commit is still the current `main` at publication time.

## Failures and reruns

Build, validation or packaging failure prevents release publication. Workflow artifacts retain logs and available package output for 30 days.

Publication keeps the release in draft until all four assets are uploaded. A rerun can finish an incomplete draft. If the release is already published, the script preserves its assets and returns its link. Reruns keep the same workflow run number and tag.

Independent pushes each receive a build; they are not cancelled by a newer push. A newer push can arrive during publication, so the Latest check is not a transactional lock.

The release job uses `contents: write` and GitHub's built-in token. No personal access token or additional repository secret is required.

CI validates the build and isolated rendering pipeline. Hardware GPU behaviour and live MSFS operation require their own checks.
