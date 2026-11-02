#pragma once
#include <algorithm>
#include <limits>
#include "protocol.hpp"

namespace taxi_camera::standalone {
// Camera actions: 0 left, 1 right, 2 both pilot displays, 3 lower ECAM (SD),
// 4 the door camera. Both never changes the SD; the SD action needs a profile
// with that side. The door camera is not a side of its own: it turns the door
// camera on its display on and off (toggle_door_camera).
enum CameraAction : unsigned { CameraLeft, CameraRight, CameraBoth, CameraSd, CameraDoor, CameraActions };
static_assert(LvarBindingCount == CameraActions, "One L:var binding per camera action");
// The sides an action toggles, or 0 when the selected profile lacks them.
inline unsigned camera_action_sides(const profiles::AircraftProfile& profile, unsigned action) noexcept {
  const unsigned affected = action == CameraBoth ? PilotDisplaySides : action < CameraDoor ? 1u << (action == CameraSd ? 2 : action) : 0u;
  return (affected & profiles::side_mask(profile)) == affected ? affected : 0u;
}
// All affected sides on turns them off; otherwise they all turn on.
inline unsigned toggle_sides(unsigned mask, unsigned affected) noexcept {
  return (mask & affected) == affected ? mask & ~affected : mask | affected;
}
// Only changes session-scoped intent. The bridge still owns service, aircraft
// identity, flight-session and telemetry checks.
inline void toggle_manual_camera(Settings& settings, unsigned action, std::uint32_t automatic_mask = 0) noexcept {
  const auto* profile = profiles::find(settings.profile);
  const auto affected = profile ? camera_action_sides(*profile, action) : 0u;
  if (!affected)
    return;
  const auto mask = (settings.follow_taxi ? automatic_mask : settings.manual_mask) & profiles::side_mask(*profile);
  settings.manual_mask = toggle_sides(mask, affected);
  settings.follow_taxi = 0;
  settings.calibration_mask = 0;
  settings.scene_test = 0;
}

// PMDG 777: the CAM page keeps selecting displays while follow_taxi is on.
// Manual previews and shortcuts add displays on top of it and never turn
// cockpit control off. A display turned on by CAM is turned off with CAM.
inline void toggle_manual_layer(Settings& settings, unsigned action) noexcept {
  const auto* profile = profiles::find(settings.profile);
  const auto affected = profile ? camera_action_sides(*profile, action) : 0u;
  if (!affected)
    return;
  settings.manual_mask = toggle_sides(settings.manual_mask & profiles::side_mask(*profile), affected);
  settings.calibration_mask = 0;
  settings.scene_test = 0;
}

enum class CameraHotkeyResult { manual, aircraft, unavailable };
inline CameraHotkeyResult request_camera_hotkey(Settings& settings, unsigned action, const Status& status, std::uint64_t now) noexcept {
  const auto* profile = profiles::find(settings.profile);
  if (!profile || !camera_action_sides(*profile, action))
    return CameraHotkeyResult::unavailable;
  if (profile->taxi_control == profiles::TaxiControl::pmdg_dsp_cam) {
    toggle_manual_layer(settings, action);
    return CameraHotkeyResult::manual;
  }
  if (profile->taxi_control == profiles::TaxiControl::manual_only) {
    const auto follow = settings.follow_taxi;
    toggle_manual_camera(settings, action);
    settings.follow_taxi = follow;
    return CameraHotkeyResult::manual;
  }
  // A requested camera frame can be absent while the physical button is ON.
  // Use fresh button telemetry, with outstanding commands overlaid, instead
  // of guessing button state from delivered frames.
  if (!settings.enabled || !status.heartbeat || now < status.heartbeat || now - status.heartbeat > 3000 ||
      status.aircraft_session_epoch != settings.aircraft_session_epoch || status.active_profile != settings.profile ||
      status.detected_profile != settings.profile || !status.taxi_buttons_valid ||
      (status.taxi_buttons_mask & ~profiles::side_mask(*profile)) ||
      !status.taxi_buttons_sample_ms || now < status.taxi_buttons_sample_ms || now - status.taxi_buttons_sample_ms > 500 ||
      std::max(settings.taxi_request, status.taxi_request_seen) == std::numeric_limits<std::uint64_t>::max())
    return CameraHotkeyResult::unavailable;
  // Once the worker has seen this request, completed sides belong to the
  // cockpit again even if the other side is still awaiting acknowledgement.
  const unsigned pending = settings.taxi_request > status.taxi_request_seen    ? settings.taxi_selected_mask
                           : settings.taxi_request == status.taxi_request_seen ? settings.taxi_selected_mask & status.taxi_request_pending
                                                                               : 0;
  auto mask = settings.follow_taxi ? status.taxi_buttons_mask : settings.manual_mask;
  if (settings.follow_taxi)
    mask = (mask & ~pending) | (settings.taxi_desired_mask & pending);
  const auto affected = camera_action_sides(*profile, action);
  const auto target = toggle_sides(mask, affected);
  settings.taxi_desired_mask = (settings.taxi_desired_mask & pending & ~affected) | (target & affected);
  settings.taxi_selected_mask = pending | affected;
  settings.taxi_request = std::max(settings.taxi_request, status.taxi_request_seen) + 1;
  settings.manual_mask = target;
  settings.calibration_mask = settings.scene_test = 0;
  return CameraHotkeyResult::aircraft;
}

// A door camera with a display.
inline bool door_camera_set_up(const Settings& settings) noexcept {
  const auto* profile = profiles::find(settings.profile);
  return profile && settings.door_side < MaxDisplaySides && (profiles::side_mask(*profile) & (1u << settings.door_side));
}
// Session-scoped like the manual request. False when there is no door camera.
inline bool toggle_door_camera(Settings& settings) noexcept {
  if (!door_camera_set_up(settings))
    return false;
  settings.door_on ^= 1u;
  settings.calibration_mask = 0;
  settings.scene_test = 0;
  return true;
}
// Whether every display an action affects is requested now, read the way its
// shortcut reads it: the cockpit buttons while they are followed on aircraft
// Taxi Cam can command, otherwise the manual request (the PMDG 777's manual
// layer). A latched L:var binding toggles only when this differs from it.
inline bool camera_action_requested(const Settings& settings, const Status& status, unsigned action) noexcept {
  if (action == CameraDoor)
    return settings.door_on != 0;
  const auto* profile = profiles::find(settings.profile);
  const auto affected = profile ? camera_action_sides(*profile, action) : 0u;
  if (!affected)
    return false;
  const bool cockpit = settings.follow_taxi && profile->taxi_control != profiles::TaxiControl::manual_only &&
                       profile->taxi_control != profiles::TaxiControl::pmdg_dsp_cam;
  const auto mask = cockpit ? status.taxi_buttons_mask : settings.manual_mask;
  return (mask & affected) == affected;
}
}  // namespace taxi_camera::standalone
