# Keyboard shortcuts

In **Settings → Overview**, select **Keyboard shortcuts…** in the **Flight-deck control** section. The shortcuts work while MSFS has focus and Taxi Cam is hidden to the tray. Taxi Cam must remain running.

| Action | Default shortcut | Behaviour |
| --- | --- | --- |
| Left camera | Ctrl + Shift + L | Toggle the left display |
| Right camera | Ctrl + Shift + R | Toggle the right display |
| Both cameras | Ctrl + Shift + B | Turn both on; if both are already on, turn both off |

Each display contains the nose and tail views. Holding a shortcut does not repeatedly toggle the display.

Click a shortcut field and press the combination you want, then select **Save changes** in the editor. Use Ctrl or Alt with a letter, number or function key; F12 and reserved system combinations are unavailable. **Clear** disables an action's shortcut. **Reset shortcuts** restores Ctrl + Shift + L / R / B and takes effect when saved. Shortcuts must be different for each action. **Close** discards any shortcut changes that have not been saved. Saving shortcuts preserves unfinished camera settings in the main window.

The status below each action reports whether Windows accepted the shortcut. If another app has registered the same combination, choose a different one or close that app and select **Save changes** to retry. A conflicting shortcut does not prevent the other shortcuts from working.

Shortcuts preserve **Overview → TAXI buttons**, whether it is on or off, and end target calibration. On FBW A380 and iniBuilds A350, they update the aircraft's corresponding TAXI button state: turning a display off by shortcut also turns its cockpit button off. With **TAXI buttons** enabled, subsequent cockpit clicks continue to control the displays. With it disabled, the camera preview is controlled manually and shortcuts still synchronize their selected aircraft buttons. The iniBuilds A380's INOP buttons are not used; its cameras use manual keyboard requests or **PFD routing** previews.

Aircraft button control requires current simulator telemetry. A pending toggle waits for acknowledgement before another event can be sent for that side; the speed cutoff takes priority over an ON request. If the aircraft cannot confirm the change, Taxi Cam reports it rather than repeatedly toggling the button.

Shortcuts do not turn on a disabled camera service. Aircraft identity, flight-session checks and the speed cutoff still apply. Display requests reset when the flight or selected aircraft changes and when Taxi Cam restarts. Shortcuts preserve unfinished calibration edits in the settings window.

The key combinations apply to all aircraft and are saved separately from calibration in `%LOCALAPPDATA%\Taxi Cam\hotkeys.ini`. Existing saved shortcuts take precedence over new defaults, including the earlier Ctrl + Shift + F9 / F10 / F11 combinations. To adopt L / R / B, select **Reset shortcuts**, then **Save changes** in the shortcut editor. This does not reset camera mounts, guide positions or display settings. The app's UI preview does not register global shortcuts.
