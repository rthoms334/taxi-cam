# Taxi Cam

## Important notice — unsupported integration

Taxi Cam uses an **unsupported method** to add taxi-camera views to Microsoft Flight Simulator 2024. It calls undocumented internal camera and renderer interfaces and installs graphics hooks inside the simulator process. These calls are outside the supported public SDK and are not supported or endorsed by Microsoft or Asobo.

The app does **not** adjust the simulator's graphics-quality or performance settings. It does interact with internal camera and rendering state to produce its views. Compatibility checks cannot guarantee safety: simulator updates or other add-ons may cause failures, instability or crashes.

**Use at your own risk.** The application is provided as is, without a guarantee of stability, compatibility or continued operation. Close the simulator before installing, updating or uninstalling it.

## Install

Before installing, you need:

- **64-bit Windows 10 or Windows 11**, with Windows updates and a current graphics driver.
- **A Direct3D 12 GPU supporting feature level 12_0 or higher**, with the newer Direct3D 12 interfaces used by MSFS 2024. The code is vendor-neutral and intended for **NVIDIA and AMD**. "DirectX 12 installed" alone does not guarantee compatibility; cross-vendor live simulator testing is still pending.
- **Microsoft Flight Simulator 2024** and the **FlyByWire A380X**. The simulator installation must include its `SimConnect_internal.dll` client.
- **[Microsoft Visual C++ v14 Redistributable (x64)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)**, required by the simulator's SimConnect client. MSFS normally installs it; install or repair the [x64 runtime](https://aka.ms/vc14/vc_redist.x64.exe) if setup reports it missing.
- **Windows PowerShell 5.1**, normally included with Windows, for installation and update checks. Internet access to GitHub is needed for automatic updates.

Setup checks Windows, required system components, the simulator executable, SimConnect client and Visual C++ runtime files before changing application files or startup configuration. If a required component is missing or has the wrong architecture, setup explains how to repair it. GPU and simulator compatibility are checked when the app connects to MSFS. You do not need the MSFS SDK or build tools to use Taxi Cam.

Download the Windows x64 **setup EXE** from [Releases](https://github.com/rthoms334/taxi-cam/releases/latest). Close MSFS and exit any running Taxi Cam instance, then open setup.

Choose the folder containing `FlightSimulator2024.exe` and the simulator's `exe.xml` launch configuration. Setup attempts to find the configuration and reuses existing installation choices on updates.

The installer places the EXE and bridge DLL together in `%LOCALAPPDATA%\Taxi Cam\app`; the wizard lets you choose another folder. Open `taxi-cam.exe` to launch the app; the DLL is loaded by the app and is not launched directly.

The installer adds automatic startup to `exe.xml` and creates a Start menu shortcut. It backs up the launch configuration and preserves other add-ons.

Release files contain no loose PowerShell scripts, documentation folders or license folders. A single `THIRD_PARTY_NOTICES.txt` retains the notices required by the statically linked runtime. The optional ZIP contains the runtime files only; use setup to configure startup and uninstallation.

## About the application

Nose-wheel and tail cameras for Microsoft Flight Simulator 2024, controlled by the aircraft's EFIS **TAXI** buttons.

The camera image occupies the upper part of the Primary Flight Display (PFD): nose-wheel view above, tail view below, with ground speed, aircraft-specific reference marks and a black divider. The lower trim display remains visible. A380 nose markers are 14-by-14-pixel magenta squares; A350 nose markers are 12-pixel-diameter amber circles.

- **Aircraft:** FlyByWire A380X; iniBuilds A350-900 / ULR and A350-1000 adapters under live validation
- **Platform:** Windows x64, MSFS 2024
- **Delivery:** Windows tray application and an in-simulator DLL.

## How it works

Taxi Cam asks MSFS to render two additional views of the aircraft and its surroundings. It then draws those images into the texture that the cockpit model uses for its PFD screen.

1. **MSFS starts the tray app.** An `exe.xml` entry launches `taxi-cam.exe`, which loads `taxi-camera-bridge.dll` into the simulator.
2. **The bridge reads the aircraft.** SimConnect supplies TAXI-button state, aircraft position and orientation, ground speed and ambient lighting.
3. **MSFS renders the cameras.** The bridge calls the simulator's internal camera functions to position independent nose and tail views relative to the aircraft.
4. **The GPU combines the images.** The bridge copies the rendered views and adds the divider, reference marks and ground speed. It updates the enabled PFD through a verified texture copy or a camera draw at the end of the aircraft's graphics command recording.

Left TAXI enables the left PFD; right TAXI enables the right PFD. Both displays share the same pair of cameras. While a supported aircraft is stationary on the ground, Taxi Cam prepares that pair in the background and then parks it for the first button press. Switching TAXI off retains the pair with rendering stopped. Images remain on the GPU throughout capture and display.

Compatibility is checked against the required private code and memory layout. Simulator updates are allowed when these checks pass. If a required signature changes or moves, the camera stays disabled and the app reports the failed check; an updated Taxi Cam integration may be needed.

See [How the camera reaches the PFD](docs/architecture.md) for the complete explanation.

## Updates

The app checks GitHub Releases in the background and offers a downloaded update when a newer version or release build is available. Use **Check for updates** in the tray menu to check manually.

Updates require publicly readable releases with a setup EXE. Live update distribution is unavailable while the repository is private; no GitHub credentials are stored in the app.

The download must pass SHA-256 verification before setup can launch. Accepting the prompt exits the companion and starts setup for the current installation folder. Close MSFS first: its loaded bridge DLL cannot be replaced while the simulator is running. Declining the update leaves the app running. Updates preserve saved camera settings.

## Use

1. Start MSFS and load the aircraft. Select its matching profile in **Overview**.
2. Allow a few seconds for the app to identify the PFD textures.
3. Press the left or right EFIS **TAXI** button to enable that display. Press it again to switch the camera off.

Above **60 knots**, camera delivery is inhibited and active TAXI buttons are commanded off.

Right-click the tray icon and choose **Settings** to adjust the cameras. Closing the settings window leaves the app running. **Exit** stops delivery; the DLL remains loaded until MSFS exits.

If the camera appears on the wrong display, open **PFD routing** to identify, assign or swap the left and right targets. Automatic detection uses display draw activity, so it may require manual correction.

## Settings

| Page | Controls |
| --- | --- |
| Overview | Automatic aircraft selection, camera service, TAXI-button control and camera rate |
| Camera views | Independent position, pitch, yaw and field of view for each camera |
| Display | Manual exposure, automatic night adjustment and ground-speed colour |
| PFD routing | Left/right display assignment, preview and target identification |
| Diagnostics | Scene test, single-camera test and runtime counters |
| Reference guides | Nose-dot and mirrored tail-bracket positions, live preview and profile reset |

Use **Apply live** on Reference guides to align the markers while the camera runs. Select **Save changes** to keep adjustments for that aircraft profile. Settings are stored in `%LOCALAPPDATA%\Taxi Cam\profiles\<aircraft-key>.ini`.

The camera rate can be set from **15 to 60**. It limits how often each camera is requested to render; achieved frame rate depends on simulator updates and rendering load. Each camera renders at the pane size specified by its aircraft profile.

## Reporting a problem

Click the **bug icon above the version number** in Taxi Cam's sidebar, or choose **Report a bug** in the tray menu. This opens a GitHub bug report with the app version, current settings and runtime counters filled in. Review it, describe what happened, attach the requested files and submit using your GitHub account. You can also open the [bug report form](https://github.com/rthoms334/taxi-cam/issues/new?template=bug_report.yml) directly.

Use **Diagnostics → Open log folder**, or press **Win+R** and enter `%LOCALAPPDATA%\Taxi Cam`. Attach a ZIP containing `bridge.log` and `profiles/fbw-a380x.ini`, where available. For crashes, include the matching Windows application error details from Reliability Monitor or Event Viewer. Note the incident time and time zone. If the app failed before creating a log, include the error and explain which files are missing.

The form asks for Taxi Cam/MSFS/aircraft versions, Windows and GPU details, graphics settings, reproduction steps and screenshots where useful. Review attachments for personal information such as usernames and local paths before sharing them. The app opens a draft; attachments and submission are completed on GitHub.

## Uninstall

With MSFS and Taxi Cam closed, uninstall **Taxi Cam** from Windows Installed apps. Setup removes its startup entry and application files. Saved settings and calibration are retained.

## Build and releases

To build locally with the pinned compiler and run validation:

~~~powershell
.\build.ps1 -Bootstrap -Validate
.\smoke-test.ps1
.\installer\package.ps1
~~~

To build a local setup after validation:

~~~powershell
.\installer\bootstrap.ps1
$zip = .\installer\package.ps1 -BuildLabel build.0 -SourceCommit (git rev-parse HEAD)
.\installer\build.ps1 -Package $zip
~~~

Local builds use build number zero; release builds use the GitHub workflow run number. Packages are immutable, so move an earlier local candidate out of the output location before building another with the same label. PowerShell scripts for development, validation and installation remain in the source repository; setup embeds the integration scripts it needs internally.

The application version advances automatically: each new commit on `main` increments the patch version, for example `0.8.1` to `0.8.2`. Rerunning the same commit keeps its version. The app, Windows installer and release title share that semantic version; workflow build numbers identify the release artifacts.

Every push to `main` runs the [Windows release workflow](.github/workflows/release.yml). A successful run publishes an installer, runtime ZIP, checksums and release notes. Hosted checks use software D3D12 (WARP); live simulator verification is a separate check.

## Documentation

| Guide | What it explains |
| --- | --- |
| [Architecture](docs/architecture.md) | How the app creates camera views and puts them on the PFD |
| [Runtime reference](docs/runtime-reference.md) | Setting values, timing, IPC and diagnostics |
| [Aircraft integration](docs/aircraft-profiles.md) | Aircraft controls, display identification, camera geometry and profile selection |
| [Releases](docs/releases.md) | Automated builds, package contents and publication |
| [Repository structure](docs/repository-structure.md) | Source modules, tests, tools and build entry points |

This project is maintained independently of the aircraft package. Dependency notices are in [Third-party notices](THIRD_PARTY_NOTICES.md).
