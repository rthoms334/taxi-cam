#pragma once
#include "protocol.hpp"

namespace taxi_camera::standalone {
// Only changes session-scoped intent. The bridge still owns service, aircraft
// identity, flight-session, telemetry and speed-cutoff checks.
inline void toggle_manual_camera(Settings& settings, unsigned action, std::uint32_t automatic_mask = 0) noexcept {
  if (action > 2)
    return;
  const auto mask = (settings.follow_taxi ? automatic_mask : settings.manual_mask) & 3u;
  settings.manual_mask = action == 2 ? (mask == 3 ? 0u : 3u) : mask ^ (1u << action);
  settings.follow_taxi = 0;
  settings.calibration_mask = 0;
  settings.scene_test = 0;
}
}  // namespace taxi_camera::standalone
