# Keyboard shortcuts

In **Settings → Overview**, select **Keyboard shortcuts…** in the **Flight-deck control** section. The shortcuts work while MSFS has focus and Taxi Cam is hidden to the tray. Taxi Cam must remain running.

| Action | Default shortcut | Behaviour |
| --- | --- | --- |
| Left camera | Ctrl + Shift + F9 | Toggle the left display |
| Right camera | Ctrl + Shift + F10 | Toggle the right display |
| Both cameras | Ctrl + Shift + F11 | Turn both on; if both are already on, turn both off |

Each display contains the nose and tail views. Holding a shortcut does not repeatedly toggle the display.

Click a shortcut field and press the combination you want, then select **Save changes** in the editor. Use Ctrl or Alt with a letter, number or function key; F12 and reserved system combinations are unavailable. **Clear** disables an action's shortcut. **Reset shortcuts** restores the initial combinations and takes effect when saved. Shortcuts must be different for each action. **Close** discards any shortcut changes that have not been saved. Saving shortcuts preserves unfinished camera settings in the main window.

The status below each action reports whether Windows accepted the shortcut. If another app has registered the same combination, choose a different one or close that app and select **Save changes** to retry. A conflicting shortcut does not prevent the other shortcuts from working.

Using a shortcut selects manual camera control and ends target calibration. On aircraft with working TAXI buttons, turn **Overview → TAXI buttons** back on to return control to those buttons. The iniBuilds A380's INOP buttons are not used; its cameras use the keyboard or **PFD routing** previews. Turning its last calibration target off leaves manual control selected.

Shortcuts do not turn on a disabled camera service. Aircraft identity, flight-session checks and the speed cutoff still apply. Display requests reset when the flight or selected aircraft changes and when Taxi Cam restarts. Shortcuts preserve unfinished calibration edits in the settings window.

The key combinations apply to all aircraft and are saved separately from calibration in `%LOCALAPPDATA%\Taxi Cam\hotkeys.ini`. The app's UI preview does not register global shortcuts.
