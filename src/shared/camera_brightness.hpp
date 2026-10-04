#pragma once

#include <algorithm>
#include <cmath>

namespace taxi_camera {

// The user's camera brightness, in EV on top of the exposure the camera images
// take (the main view's, or Taxi Cam's fallback). 0 shows them exactly as the
// main view exposes them. Saved per aircraft profile, by day and at night.
// Like any exposure it applies only to R11G11B10_FLOAT camera inputs; the
// compositor passes other admitted formats' sampled colour through unchanged.
inline constexpr float kMinimumCameraBrightnessEv = -4;
inline constexpr float kMaximumCameraBrightnessEv = 2;
inline constexpr float kCameraBrightnessStepEv = 0.25f;
inline bool valid_camera_brightness(float ev) noexcept {
  return std::isfinite(ev) && ev >= kMinimumCameraBrightnessEv && ev <= kMaximumCameraBrightnessEv &&
         std::nearbyint(ev / kCameraBrightnessStepEv) * kCameraBrightnessStepEv == ev;
}
// Into range and onto a step, so an odd hand edit cannot reject a whole profile.
inline float snap_camera_brightness(double ev) noexcept {
  if (!std::isfinite(ev))
    return 0;
  const double clamped = std::clamp<double>(ev, kMinimumCameraBrightnessEv, kMaximumCameraBrightnessEv);
  return static_cast<float>(std::nearbyint(clamped / kCameraBrightnessStepEv) * kCameraBrightnessStepEv);
}
// darkness: 0 by day, 1 at night (ambient_darkness).
inline float camera_brightness_ev(float day_ev, float night_ev, double darkness) noexcept {
  const double t = std::isfinite(darkness) ? std::clamp(darkness, 0.0, 1.0) : 0.0;
  return static_cast<float>(day_ev + (night_ev - day_ev) * t);
}
// A:AMBIENT LIGHT SENSOR from 4000 down to 1 on a log scale, as 0 (day) to 1
// (night). 1..4000 follows the official Asobo display-lighting template range.
inline double ambient_darkness(double ambient) noexcept {
  return std::clamp(std::log2(4000.0 / std::max(ambient, 1.0)) / std::log2(4000.0), 0.0, 1.0);
}

}  // namespace taxi_camera
