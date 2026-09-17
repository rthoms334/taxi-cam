# Taxi Cam

Nose-wheel and tail cameras for Microsoft Flight Simulator 2024. Use the aircraft's **TAXI** button or a keyboard shortcut to see both views on the upper part of its Primary Flight Display (PFD).

## Compatibility

- **ReShade compatibility is experimental.** Taxi Cam now handles ReShade's graphics-hook ordering. If camera output is missing, include the bridge log in your bug report. See [startup order, compatibility and validation details](docs/reshade-compatibility.md).
- **Steam and Microsoft Store:** Taxi Cam checks the loaded simulator's camera interfaces automatically. Simulator updates that change those interfaces can still prevent loading. See [simulator build compatibility](docs/dynamic-camera-compatibility.md).

## Important notice

Taxi Cam is **experimental** and uses an unsupported simulator integration. It is not endorsed by Microsoft or Asobo. Simulator or aircraft updates may cause problems, including crashes. **Use at your own risk.**

If you find Taxi Cam useful, please consider donating.

[![Donate with PayPal](https://www.paypalobjects.com/en_US/i/btn/btn_donate_LG.gif)](https://www.paypal.com/donate/?hosted_button_id=EPVELD44P6NXW)

## Supported aircraft

- **FlyByWire A380X**
- **iniBuilds A350-900**, including **ULR**
- **iniBuilds A350-1000**
- **iniBuilds A380** — use keyboard shortcuts or manual previews because its TAXI buttons are marked INOP.

## Installation

For **Windows 10/11 (64-bit)** and **MSFS 2024**, with one of the aircraft above. Keep Windows and your graphics driver up to date. Setup checks required components and explains if anything is missing. [Detailed graphics requirements](docs/runtime-reference.md#graphics-requirements).

1. Download the **Windows x64 setup EXE** from [the latest release](https://github.com/rthoms334/taxi-cam/releases/latest).
2. Close MSFS and exit Taxi Cam if it is already running.
3. Run setup, confirm your simulator folder, and choose **Configure automatic startup with MSFS** or **Launch manually**. Setup remembers this choice on updates. Manual launch skips startup configuration and leaves any existing startup entry unchanged.

**Keep existing settings** is selected by default. Leave it checked to preserve camera profiles, calibration, reference guides and keyboard shortcuts. Clear it only if you want to start with the bundled defaults. Setup restores your previous settings if installation fails.

If Setup cannot configure automatic startup and can verify the startup file is unchanged, installation completes with a notice explaining how to launch Taxi Cam from the Start menu. Details are saved in `setup-diagnostics.log` in the installation folder. You can rerun Setup to retry automatic startup. See [Automatic startup](docs/runtime-reference.md#automatic-startup) for Steam and Microsoft Store startup file locations and selection behavior.

For manual launch, start Taxi Cam while MSFS is at the main menu, before loading a flight. Starting after the cockpit displays already exist can leave PFD detection incomplete. If this happens, keep Taxi Cam running and restart the flight; this recovered the observed iniBuilds A380 case. See [startup timing](docs/reshade-compatibility.md#startup-timing).

On its first launch, **Settings** opens so you can explore the controls. Close the window or select **Hide to tray** to keep the app running in the system tray. Later automatic starts stay in the tray; opening Taxi Cam from the Windows Start menu shows Settings again. The first-launch choice is remembered across updates.

## Using Taxi Cam

1. Load a supported aircraft. Leave **Auto aircraft** enabled in **Overview** to select the matching settings automatically.
2. Allow a few seconds for the cameras to become ready.
3. Press the left or right EFIS **TAXI** button to show the cameras on that side's PFD. On the iniBuilds A380, use **Ctrl + Shift + B** to turn both displays on. Repeat the control to turn them off.

The cameras switch off above **60 knots**. If a view appears on the wrong display, use **PFD routing** in Settings to correct it.

Keyboard controls work while Taxi Cam is hidden: **Ctrl + Shift + L** toggles the left display, **Ctrl + Shift + R** the right and **Ctrl + Shift + B** both. In **Settings → Overview → Flight-deck control**, select **Keyboard shortcuts…** to configure or disable them. On aircraft with working TAXI buttons, shortcuts also update the cockpit button state and preserve the **TAXI buttons** setting. See [Keyboard controls](docs/keyboard-shortcuts.md) for details.

## Adjusting the views

Right-click the Taxi Cam tray icon and open **Settings**. Adjustments are saved separately for each aircraft profile.

### Camera views

With the aircraft parked and the cameras on, open **Camera views**. Adjust the nose and tail cameras' position, angle and **Lens** (zoom). A smaller lens value gives a closer view; a larger value shows more of the surroundings.

Choose **Save changes** to apply and keep your adjustments. **Reset camera mounts** restores the profile's default views; save afterwards to keep the reset.

### Reference guides

Set your camera views first, then open **Reference guides** to position the nose markers and tail brackets. **X** moves them sideways and **Y** moves them up or down; the right side mirrors the left.

Choose **Apply live** to preview the positions, then **Save changes** to keep them. To restore the defaults, choose **Reset guide positions** and save. The guides may need realigning if you change the camera view.

### Brightness and smoothness

On **Display**, adjust brightness, automatic night exposure and ground-speed text colour. **Camera frame rate** accepts **5–60**, with a default of **15**. Try **5** or **10** to reduce camera workload at the cost of less frequent image updates. The achieved rate also depends on simulator performance. Choose **Save changes** when finished.

All aircraft start with **−11.5 EV** daytime exposure, **Auto exposure** enabled, **8 EV** maximum night boost and the same green ground-speed colour. This update sets existing profiles' maximum night boost to **8** once, when each profile is first loaded. Other saved preferences are preserved, including the Auto exposure choice. Later changes to the night boost remain saved normally. GS displays whole knots by dropping the fractional part: 12.9 knots displays as 12.

### Identifying the PFDs

If automatic detection stays at a waiting message or selects the wrong screens, open **PFD routing**:

1. Select **Refresh textures**, then choose a candidate for the left or right display.
2. Use **Calibrate left** or **Calibrate right** to identify that screen with an animated pattern. Repeat with another candidate if the pattern appears on the wrong screen. Use **Swap left / right** if the two sides are reversed.
3. Turn both calibration controls off, then activate the cameras with the TAXI buttons or shortcuts. On aircraft with working TAXI buttons, turning the last calibration control off automatically restores button control. The iniBuilds A380 stays in manual control.

Assignments apply immediately and belong to the current flight; texture IDs can change after a reload. **Left preview** and **Right preview** are also available, but these controls turn off **Overview → TAXI buttons**. Re-enable that setting to return to cockpit-button control. Keyboard shortcuts preserve the setting.

## Known issues and limitations

- **Night lighting:** Runway and taxiway lights can look very faint or be difficult to see. Improving their visibility is on the roadmap.
- **DLSS camera movement:** Slight aircraft movement can remain in the A350 lower view while taxiing with DLSS. TAA does not exhibit this movement. If the cameras do not recover after changing graphics settings and Taxi Cam asks for a restart, restart MSFS.
- **Frame-rate impact:** Extra camera views cost performance. Taxi Cam is designed to keep this as low as possible, but you may notice a drop in FPS. Try a lower camera frame rate if needed.

Please report unexpected behaviour using **Report a bug**.

## Updates and removal

Use **Check for updates** in the tray menu, or download the latest installer from [Releases](https://github.com/rthoms334/taxi-cam/releases/latest). Close MSFS and Taxi Cam before installing an update. Your saved settings are kept by default; setup offers an explicit reset if you want to start again.

Existing shortcut choices are kept too. To adopt **Ctrl + Shift + L / R / B**, open **Overview → Flight-deck control → Keyboard shortcuts…**, select **Reset shortcuts**, then **Save changes** in that editor.

Click the version number at the bottom of the settings sidebar to open the Taxi Cam GitHub repository in your browser.

To remove the mod, close both applications and uninstall **Taxi Cam** from Windows Installed apps. **Keep settings** is the default choice so a later installation can reuse them. Choose **Remove saved settings** to clear camera profiles, calibration, guides, keyboard shortcuts and first-launch preferences instead. This also clears known profiles retained under the former **380 Taxi Cam** name, preventing them from being imported again. Logs and unrelated files are kept.

## Reporting a problem

Click the app's **bug icon** or choose **Report a bug** in the tray menu. Describe what happened and follow the form's prompts for logs and screenshots. **Diagnostics → Open log folder** takes you to the files you may need.

You can also [open a bug report on GitHub](https://github.com/rthoms334/taxi-cam/issues/new?template=bug_report.yml).

## Technical details

Taxi Cam asks MSFS to render two extra camera views and combines them on the GPU for the cockpit display. A Windows tray app manages the settings, and a graphics bridge runs inside the simulator. This relies on undocumented simulator interfaces; it does not change your simulator graphics settings.

- [How it works](docs/architecture.md)
- [Settings, graphics requirements and diagnostics](docs/runtime-reference.md)
- [0.9.8 versus this tree](docs/version-benchmark.md)
- [Aircraft profiles and calibration](docs/aircraft-profiles.md)
- [Builds and releases](docs/releases.md)
- [Source layout](docs/repository-structure.md)

See [Third-party notices](THIRD_PARTY_NOTICES.md).

## Licence

Copyright © 2026 Robert Thomson. Original Taxi Cam code and project files are licensed under the [GNU General Public License, version 3 only](LICENSE) (`GPL-3.0-only`). Commercial use is allowed. If you distribute Taxi Cam or a covered derivative, you must comply with GPLv3, including its licence, notice and corresponding-source requirements. The software comes without warranty.

Release notes link to the exact source revision, including the build and installation scripts. Windows packages and installations include `LICENSE.txt`. Third-party components retain their own licences and [notices](THIRD_PARTY_NOTICES.md).
