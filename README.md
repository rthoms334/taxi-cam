# Taxi Cam

Nose-wheel and tail cameras for Microsoft Flight Simulator 2024. Use the aircraft's **TAXI** button or a keyboard shortcut to see both views on the upper part of its Primary Flight Display (PFD).

This was built out of curiosity - what would it take to actually get this to work.. 
Looking at what the community was doing with reshade and DLSSG, it got me wondering if I could use the same patterns here to transform the output with some MSFS camera views.
Turns out it was and ran reasonably well on my setup (9800x3d and 5070Ti). 
Its not universally compatible with every setup out there so be very aware that it might not work for you. 
If you are up for reporting issues then I can try and help out.

## Compatibility

- **DLSSG and Reshade mods** - There are a vast number of different configs out there for reshade and unsupported DLSSG mods - if you raise an issue I will endeavour to investigate and see what can be done. A best effort has been made to change the archetecture to move the processing downstream of reshade to avoid conflicts but some may remain due to other mods potentially using the same patterns to transform the output.
- **Frame Generation** - On some systems (reported with RTX 50-series cards) the simulator freezes once the cameras open while Frame Generation is on. Taxi Cam now reads the simulator's graphics options and keeps the cameras off while Frame Generation is enabled; turn it off in **Options > Graphics** to use the cameras, or allow it on **Diagnostics** to test.
- **ReShade** - A `dxgi.dll` beside the simulator executable (ReShade) stops camera capture on some systems. Taxi Cam names it on the Overview status line; rename the file (with MSFS closed) to use the cameras, and restore it afterwards.
- **Sim Update 7** - Make sure you have the stable version of MSFS (SU6 - 1.8.16.0) - SU7 is not supported currently.

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

**Keep existing settings** is selected by default. Leave it checked to preserve camera profiles, calibration, reference guides and keyboard shortcuts. This version still writes camera frame rate **10** into existing settings; other saved values stay in place. Clear the checkbox only if you want to start with the bundled defaults. Setup restores your previous settings if installation fails.

If Setup cannot configure automatic startup and can verify the startup file is unchanged, installation completes with a notice explaining how to launch Taxi Cam from the Start menu. Details are saved in `setup-diagnostics.log` in the installation folder. You can rerun Setup to retry automatic startup. See [Automatic startup](docs/runtime-reference.md#automatic-startup) for Steam and Microsoft Store startup file locations and selection behavior.

Start MSFS the usual way rather than **Run as administrator**. If the simulator does hold administrator rights, Taxi Cam cannot attach from a standard launch and reports **MSFS is running as administrator (Windows 5)**; choose **Restart as administrator** in Overview or the tray menu to restart Taxi Cam with the same rights. Automatic startup through `exe.xml` inherits the simulator's rights and needs no action.

For manual launch, start Taxi Cam from the Start menu (or tray) while MSFS is running. You can leave **Auto-connect** on, or turn it off and use **Connect** at the main menu or in a loaded flight. Connect enables camera operation and changes to **Disconnect**, which stops camera output and temporary requests. Choose Connect again to resume or retry an attachment. Disconnect keeps your Auto-connect preference but suspends automatic retries until you choose Connect again. These controls do not stop MSFS or overwrite calibration. After an in-flight connection, wait for the next cockpit draws while the bridge learns existing display bindings. Recovery remains unresolved for already-powered A350 displays whose old views cannot be associated with those draws; a populated texture list alone does not guarantee output. See [startup timing](docs/reshade-compatibility.md#startup-timing).

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

On **Display**, adjust brightness, automatic night exposure and ground-speed text colour. **Camera frame rate** accepts **5–60** (minimum **5**), with a default of **10**. This version's installer writes **10** into existing settings so a saved 15 becomes 10; later changes you make are kept. Higher values update the cameras more often and increase simulator work. The achieved rate also depends on simulator performance. Choose **Save changes** when finished.

On **Reference guides**, **Marking colour** changes the nose squares and tail brackets independently of GS. A350 and A380 both default to magenta square nose markers. Choose **Save changes** to retain the colour for the selected aircraft profile.

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
- [Aircraft profiles and calibration](docs/aircraft-profiles.md)
- [Builds and releases](docs/releases.md)
- [Source layout](docs/repository-structure.md)

See [Third-party notices](THIRD_PARTY_NOTICES.md).

## Licence

Copyright © 2026 Robert Thomson. Original Taxi Cam code and project files are licensed under the [GNU General Public License, version 3 only](LICENSE) (`GPL-3.0-only`). Commercial use is allowed. If you distribute Taxi Cam or a covered derivative, you must comply with GPLv3, including its licence, notice and corresponding-source requirements. The software comes without warranty.

Release notes link to the exact source revision, including the build and installation scripts. Windows packages and installations include `LICENSE.txt`. Third-party components retain their own licences and [notices](THIRD_PARTY_NOTICES.md).
