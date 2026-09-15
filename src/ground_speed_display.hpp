#pragma once

#include <cmath>
#include <cstdint>

namespace taxi_camera {
struct GroundSpeedDisplay {
  std::uint32_t knots = 0;
  bool valid = false;
};

// Display conversion only. Flight controls retain the unquantized speed.
inline GroundSpeedDisplay ground_speed_display(float knots, bool valid) noexcept {
  // Preserve the existing two-digit overflow/invalid-sample admission.
  if (!valid || !std::isfinite(knots) || knots < 0 || std::floor(knots + 0.5f) > 99)
    return {};
  // A nonnegative integer conversion truncates fractional knots: 12.9 -> 12.
  return {static_cast<std::uint32_t>(knots), true};
}
}  // namespace taxi_camera
