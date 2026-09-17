#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace taxi_camera::native_camera {

// After the selected feeds have each had one activation on the current pair,
// a fresh SimConnect ground-speed sample below 0.5 kt holds render gates
// closed so MSFS can drop the extra views. Resume at 1.0 kt, when the sample
// is missing or stale, or when the caller forces resume. Ground speed is a
// refresh signal only; it is never a pose or mount input.
class StillFrameHold {
 public:
  static constexpr double kHoldKnots = 0.5;
  static constexpr double kResumeKnots = 1.0;
  static constexpr std::uint64_t kMaximumAgeMs = 500;

  void reset() noexcept {
    holding_ = false;
    armed_ = {};
  }

  void note_activation(unsigned feed) noexcept {
    if (feed < 2)
      armed_[feed] = true;
  }

  bool update(std::uint64_t now_ms,
              bool speed_valid,
              double knots,
              std::uint64_t sample_ms,
              unsigned feeds,
              bool views_ready,
              bool force_resume) noexcept {
    if (force_resume) {
      reset();
      return false;
    }
    const bool fresh = speed_valid && sample_ms != 0 && sample_ms <= now_ms && now_ms - sample_ms <= kMaximumAgeMs &&
                       std::isfinite(knots) && knots >= 0.0;
    const bool primed = views_ready && armed_[0] && (feeds < 2 || armed_[1]);
    if (!fresh || !primed) {
      holding_ = false;
      return false;
    }
    if (holding_) {
      if (knots >= kResumeKnots)
        holding_ = false;
      return holding_;
    }
    if (knots < kHoldKnots)
      holding_ = true;
    return holding_;
  }

  bool holding() const noexcept { return holding_; }

 private:
  bool holding_ = false;
  std::array<bool, 2> armed_{};
};

}  // namespace taxi_camera::native_camera
