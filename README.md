# Taxi Cam

## Important notice — unsupported integration

Taxi Cam uses an **unsupported method** to add taxi-camera views to Microsoft Flight Simulator 2024. It calls undocumented internal camera and renderer interfaces and installs graphics hooks inside the simulator process. These calls are outside the supported public SDK and are not supported or endorsed by Microsoft or Asobo.

The app does **not** adjust the simulator's graphics-quality or performance settings. It does interact with internal camera and rendering state to produce its views. Compatibility checks cannot guarantee safety: simulator updates or other add-ons may cause failures, instability or crashes.

**Use at your own risk.** The application is provided as is, without a guarantee of stability, compatibility or continued operation. Close the simulator before installing, updating or uninstalling it.

[![Donate with PayPal](https://www.paypalobjects.com/en_US/i/btn/btn_donate_LG.gif)](https://www.paypal.com/donate/?hosted_button_id=EPVELD44P6NXW)

If you enjoy using Taxi Cam, please consider supporting its development with a donation. Your support helps me continue improving the cameras and expanding aircraft compatibility. Donations are entirely optional, and every contribution is appreciated. Thank you for supporting the project!

## Install

Before installing, you need:

- **64-bit Windows 10 or Windows 11**, with Windows updates and a current graphics driver.
- **A Direct3D 12 GPU supporting feature level 12_0 or higher**, with the newer Direct3D 12 interfaces used by MSFS 2024. The code is vendor-neutral and intended for **NVIDIA and AMD**. "DirectX 12 installed" alone does not guarantee compatibility; cross-vendor live simulator testing is still pending.
- **Microsoft Flight Simulator 2024** and one of the [supported aircraft](#supported-aircraft). The simulator installation must include its `SimConnect_internal.dll` client.
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

- **Platform:** Windows x64, MSFS 2024
- **Delivery:** Windows tray application and an in-simulator DLL.

## Supported aircraft

Taxi Cam currently supports the following aircraft in **Microsoft Flight Simulator 2024**:

- **FlyByWire A380X**
- **iniBuilds A350-900**, including the **ULR** variant
- **iniBuilds A350-1000**

All listed aircraft have nose-wheel and tail camera views controlled by their left and right EFIS **TAXI** buttons. Reference marks follow the aircraft profile: magenta on the A380X and amber on the A350.

On **Overview**, leave **Auto aircraft** enabled to select the profile for the loaded aircraft, or choose the matching profile manually. Camera positions, reference guides and display settings are saved per profile. The A350-900 and ULR share one profile; the A350-1000 has its own calibration.

Aircraft support remains experimental. See [Known issues and limitations](#known-issues-and-limitations) for current caveats and [Aircraft integration](docs/aircraft-profiles.md) for profile details.

## Known issues and limitations

Taxi Cam is **experimental**. You may encounter visual glitches, connection problems, instability or crashes, and simulator or aircraft updates can affect compatibility. Please [report any issues on GitHub](https://github.com/rthoms334/taxi-cam/issues), including what happened, how to reproduce it and the versions you were using.

### Night lighting

Runway, taxiway and other small airport lights can be extremely faint or appear missing in the camera views at night. Zooming in may make some light points easier to spot, but it does not reliably resolve their visibility. The current capture and sampling method can weaken small bright details; the full cause of the in-simulator lighting limitation is still being investigated.

**Improving night-light visibility is on the roadmap.** Exposure adjustments can brighten the camera image, but they are not a fix for this limitation.

### Performance

Rendering two additional camera views uses CPU and GPU time and can reduce the simulator's frame rate. Effort has gone into keeping this overhead down: camera images stay on the GPU, the cameras use small render targets, and the camera update rate is configurable. There is still a rendering cost, and its impact varies with hardware, scenery and simulator settings.

If performance is affected, lower **Camera frame rate** on **Overview** or **Display**; **15** is the lowest supported setting. Higher camera rates request more frequent updates and can increase the rendering load. The setting limits requests per camera; it does not guarantee that frame rate will be achieved.

### Other current limitations

- **Aircraft coverage and validation:** Support is limited to the [listed aircraft](#supported-aircraft). A350 integration continues to be validated in the simulator; the A350-1000's default camera and guide alignment still needs a live check.
- **Display routing:** Automatic PFD detection can need manual correction in **PFD routing**. Manual display assignments belong to the current simulator session and may need to be repeated after reloading or changing aircraft.
- **Shared views:** Both PFDs use the same nose and tail cameras; each display cannot have its own camera framing.
- **Reference guides:** The marks are fixed visual references, not measured clearance indicators. Changing camera position or field of view can require realigning them.
- **Taxi-only operation:** Camera delivery is inhibited and active TAXI buttons are commanded off above **60 knots**.

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

1. Start MSFS and load the aircraft. Let **Auto aircraft** select its profile in **Overview**, or choose the matching profile manually.
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
| Reference guides | Nose markers and mirrored tail-bracket positions, live preview and profile reset |

Settings are saved separately for each aircraft profile in `%LOCALAPPDATA%\Taxi Cam\profiles\<aircraft-key>.ini`. Before adjusting anything, check that **Overview** shows the profile matching the loaded aircraft.

### Adjusting camera views

1. With the aircraft parked, enable a TAXI display so you can see the result, then open **Settings → Camera views**.
2. Adjust **Nose-wheel camera** and **Tail camera** independently using the controls below. Make small changes and check the framing in the cockpit.
3. Select **Save changes** to apply entered values to the running cameras and keep them for that aircraft profile.
4. Once the framing is right, check and adjust the **Reference guides** to suit it.

| Control | What it changes |
| --- | --- |
| Right (m) | Sideways position relative to the aircraft's reference origin; positive moves right, negative moves left |
| Up (m) | Height relative to that origin; positive moves up, negative moves down |
| Forward (m) | Position along the aircraft; positive moves forward, negative moves aft |
| Pitch (deg) | Vertical viewing angle; positive looks up, negative looks down |
| Yaw (deg) | Horizontal viewing angle; positive looks right, negative looks left |
| Lens (rad) | Field of view; a larger value gives a wider view, and a smaller value gives a closer view |

The **Lower 0.25 m**, **Raise 0.25 m**, **Aft 1 m** and **Forward 1 m** buttons provide small position adjustments that apply live. Select **Save changes** to keep them. **Reset camera mounts** restores both cameras to the selected profile's defaults; save afterwards to keep the reset. It leaves guide positions unchanged.

### Adjusting reference guides

The nose view uses squares or dots, depending on the aircraft profile. The tail view uses a mirrored pair of brackets. Their positions are relative to each camera image, so set the camera framing first.

1. Keep the aircraft parked with a TAXI display visible, then open **Reference guides**.
2. Set the nose marker's **X** and **Y** positions. For the tail brackets, adjust **Upper endpoint**, **Outside corner** and **Inner endpoint**.
3. Use **X** from **0–50%**, measured from the left edge towards the centre of the relevant camera pane, and **Y** from **0–100%**, measured from its top to its bottom. The right-hand guide mirrors the left automatically.
4. Check the page's mirrored preview, then select **Apply live** to see the marks on the running camera image. Repeat until their placement suits the view.
5. Select **Save changes** to keep the positions for that aircraft profile. **Apply live** alone does not save them.

**Reset guide positions** restores the selected profile's shipped guide positions without changing camera mounts or display settings. Select **Save changes** to keep the reset. Guides do not automatically follow the wheels when camera position, pitch or lens changes, and their placement does not establish measured clearance.

### Brightness, colour and camera rate

On **Display**, adjust **Daytime exposure** for the baseline image brightness. **Auto exposure** gradually adjusts brightness as ambient light falls; **Maximum night boost** sets the additional exposure available at night, from **0 to +8 EV**. **Ground-speed colour** changes the speed text without changing the reference marks. Select **Save changes** to keep your adjustments.

**Camera frame rate**, available on **Overview** and **Display**, accepts **15–60**. Start with a lower rate and increase it if you want smoother camera updates and have performance to spare. Camera render sizes are managed by the application separately from this rate setting. See [Performance](#performance) and [Night lighting](#night-lighting) for the current limits.

## Reporting a problem

Click the **bug icon above the version number** in Taxi Cam's sidebar, or choose **Report a bug** in the tray menu. This opens a GitHub bug report with the app version, current settings and runtime counters filled in. Review it, describe what happened, attach the requested files and submit using your GitHub account. You can also open the [bug report form](https://github.com/rthoms334/taxi-cam/issues/new?template=bug_report.yml) directly.

Use **Diagnostics → Open log folder**, or press **Win+R** and enter `%LOCALAPPDATA%\Taxi Cam`. Attach a ZIP containing `bridge.log`, `settings.ini` and the affected aircraft profile (`profiles/fbw-a380x.ini`, `profiles/ini-a350-900.ini` or `profiles/ini-a350-1000.ini`), where available. For crashes, include the matching Windows application error details from Reliability Monitor or Event Viewer. Note the incident time and time zone. If the app failed before creating a log, include the error and explain which files are missing.

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
