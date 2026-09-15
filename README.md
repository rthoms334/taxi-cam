# Taxi Cam

Nose-wheel and tail cameras for Microsoft Flight Simulator 2024. Use the aircraft's **TAXI** button or a keyboard shortcut to see both views on the upper part of its Primary Flight Display (PFD).

## Important notice

Taxi Cam is **experimental** and uses an unsupported simulator integration. It is not endorsed by Microsoft or Asobo. Simulator or aircraft updates may cause problems, including crashes. **Use at your own risk.**

If you find this mod useful, please consider a donation. The Windows app also has a **Donate** button above the bug-report icon; it opens the PayPal donation page in your browser.

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
3. Run setup and follow the prompts to confirm your simulator folder and startup settings.

Taxi Cam starts with MSFS after installation. You can also launch it from the Windows Start menu.

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

On **Display**, adjust brightness, automatic night exposure and ground-speed text colour. **Camera frame rate** accepts **15–60**: lower values reduce the workload, while higher values can make the camera views smoother. Choose **Save changes** when finished.

All aircraft start with **−11.5 EV** daytime exposure, **Auto exposure** enabled and the same green ground-speed colour. Saved settings take precedence over these defaults. GS displays whole knots by dropping the fractional part: 12.9 knots displays as 12.

### Identifying the PFDs

If automatic detection stays at a waiting message or selects the wrong screens, open **PFD routing**:

1. Select **Refresh textures**, then choose a candidate for the left or right display.
2. Use **Calibrate left** or **Calibrate right** to identify that screen with an animated pattern. Repeat with another candidate if the pattern appears on the wrong screen. Use **Swap left / right** if the two sides are reversed.
3. Turn both calibration controls off, then activate the cameras with the TAXI buttons or shortcuts. On aircraft with working TAXI buttons, turning the last calibration control off automatically restores button control. The iniBuilds A380 stays in manual control.

Assignments apply immediately and belong to the current flight; texture IDs can change after a reload. **Left preview** and **Right preview** are also available, but these controls turn off **Overview → TAXI buttons**. Re-enable that setting to return to cockpit-button control. Keyboard shortcuts preserve the setting.

## Known issues and limitations

- **Night lighting:** Runway and taxiway lights can look very faint or be difficult to see. Improving their visibility is on the roadmap.
- **iniBuilds A380:** Camera views and reference markers have been calibrated in the simulator. If automatic PFD selection cannot identify both displays, select them manually in **PFD routing**.
- **DLSS camera movement:** A350 testing confirmed stable views with TAA, but slight aircraft movement remains in the lower view while taxiing with DLSS. This result has not been established for every aircraft or system. If the cameras do not recover after changing graphics settings and Taxi Cam asks for a restart, restart MSFS.
- **Frame-rate impact:** Extra camera views cost performance. Taxi Cam is designed to keep this as low as possible, but you may notice a drop in FPS. Try a lower camera frame rate if needed.

Please report unexpected behaviour using **Report a bug**.

## Updates and removal

Use **Check for updates** in the tray menu, or download the latest installer from [Releases](https://github.com/rthoms334/taxi-cam/releases/latest). Close MSFS and Taxi Cam before installing an update. Your saved settings are kept.

Existing shortcut choices are kept too. To adopt **Ctrl + Shift + L / R / B**, open **Overview → Flight-deck control → Keyboard shortcuts…**, select **Reset shortcuts**, then **Save changes** in that editor.

Click the version number at the bottom of the settings sidebar to open the Taxi Cam GitHub repository in your browser.

To remove the mod, close both applications and uninstall **Taxi Cam** from Windows Installed apps.

## Reporting a problem

Click the app's **bug icon** or choose **Report a bug** in the tray menu. Describe what happened and follow the form's prompts for logs and screenshots. **Diagnostics → Open log folder** takes you to the files you may need.

You can also [open a bug report on GitHub](https://github.com/rthoms334/taxi-cam/issues/new?template=bug_report.yml).

## Technical details

Taxi Cam asks MSFS to render two extra camera views and combines them on the GPU for the cockpit display. A Windows tray app manages the settings, and a graphics bridge runs inside the simulator. This relies on undocumented simulator interfaces; it does not change your simulator graphics settings.

- [How it works](docs/architecture.md)
- [Settings, graphics requirements and diagnostics](docs/runtime-reference.md)
- [Aircraft profiles and calibration](docs/aircraft-profiles.md)
- [Builds and releases](docs/releases.md)
- [Source layout](docs/repository-structure.md)

See [Third-party notices](THIRD_PARTY_NOTICES.md).

## Licence

Copyright © 2026 Robert Thomson. Original Taxi Cam code and project files are licensed under the [GNU General Public License, version 3 only](LICENSE) (`GPL-3.0-only`). Commercial use is allowed. If you distribute Taxi Cam or a covered derivative, you must comply with GPLv3, including its licence, notice and corresponding-source requirements. The software comes without warranty.

Release notes link to the exact source revision, including the build and installation scripts. Windows packages and installations include `LICENSE.txt`. Third-party components retain their own licences and [notices](THIRD_PARTY_NOTICES.md).
