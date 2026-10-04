#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../shared/camera_brightness.hpp"

namespace taxi_camera {

struct DisplayExposureState {
  float applied_ev = -8.8f;
  float target_ev = -8.8f;
  bool lighting_valid = false;
  // ambient_darkness of the latest valid lighting sample (0 until one arrives).
  double darkness = 0;
};

// Taxi Cam's own exposure, used only while the camera images cannot take the
// main view's (no fresh simulator exposure). Display adaptation only: it
// cannot restore light contributions missing from the scene texture. The
// mapping of ambient_darkness to the fixed night boost is a bounded visual
// heuristic. Without lighting data the target is UnlitExposureEv.
class DisplayExposureController {
 public:
  // CameraCompositorD3D12::DefaultExposureEv.
  static constexpr float UnlitExposureEv = -8.8f;
  // The boost that matched the main view best at night.
  static constexpr float NightBoostEv = 4;
  static constexpr std::uint64_t MaximumLightingAgeMs = 1500;
  static constexpr float SlewEvPerSecond = 1;

  const DisplayExposureState& snapshot() const noexcept { return state_; }
  void reset() noexcept {
    state_ = {};
    initialized_ = false;
    previous_ms_ = 0;
  }

  // day_ev: the aircraft profile's daytime exposure.
  const DisplayExposureState& update(std::uint64_t now_ms,
                                     float day_ev,
                                     bool lighting_valid,
                                     double ambient,
                                     std::uint64_t lighting_sample_ms) noexcept {
    day_ev = std::isfinite(day_ev) ? std::clamp(day_ev, -16.f, 4.f) : UnlitExposureEv;
    state_.lighting_valid = lighting_valid && lighting_sample_ms != 0 && now_ms >= lighting_sample_ms &&
                            now_ms - lighting_sample_ms <= MaximumLightingAgeMs && std::isfinite(ambient) && ambient >= 0 && ambient <= 1e7;
    state_.target_ev = UnlitExposureEv;
    if (state_.lighting_valid) {
      state_.darkness = ambient_darkness(ambient);
      state_.target_ev = std::clamp(day_ev + NightBoostEv * static_cast<float>(state_.darkness), -16.f, 4.f);
    }
    if (!initialized_) {
      state_.applied_ev = state_.target_ev;
    } else {
      // No accumulated catch-up jump after a stopped Present loop. A backwards
      // caller clock grants no adaptation step and starts a new elapsed window.
      const auto elapsed = now_ms >= previous_ms_ ? std::min<std::uint64_t>(now_ms - previous_ms_, 1000) : 0;
      const auto step = static_cast<float>(elapsed) * (SlewEvPerSecond / 1000);
      state_.applied_ev += std::clamp(state_.target_ev - state_.applied_ev, -step, step);
      state_.applied_ev = std::clamp(state_.applied_ev, -16.f, 4.f);
    }
    previous_ms_ = now_ms;
    initialized_ = true;
    return state_;
  }

 private:
  DisplayExposureState state_{};
  std::uint64_t previous_ms_ = 0;
  bool initialized_ = false;
};

}  // namespace taxi_camera
