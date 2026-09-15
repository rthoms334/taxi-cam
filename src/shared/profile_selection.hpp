#pragma once
#include <limits>
#include "protocol.hpp"

namespace taxi_camera::standalone {
inline void reset_aircraft_session(Settings& settings, std::uint64_t epoch) noexcept {
  settings.aircraft_session_epoch = epoch;
  settings.left_id = settings.right_id = settings.route_request = 0;
  settings.manual_mask = settings.calibration_mask = settings.scene_test = 0;
  if (settings.taxi_request && settings.taxi_request != std::numeric_limits<std::uint64_t>::max())
    ++settings.taxi_request;
  settings.taxi_selected_mask = settings.taxi_desired_mask = 0;
}
// A profile selection is an action even when its ID has not changed. Carry
// the action sequence across per-profile loads, never persist runtime IDs.
inline bool prepare_profile_selection(Settings& next, const Settings& previous, bool automatic) noexcept {
  if (previous.profile_request == std::numeric_limits<std::uint64_t>::max() || !valid_settings(next))
    return false;
  next.profile_request = previous.profile_request + 1;
  next.auto_profile = automatic ? 1u : 0u;
  next.taxi_request = previous.taxi_request;
  reset_aircraft_session(next, previous.aircraft_session_epoch);
  return true;
}
}  // namespace taxi_camera::standalone
