#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "../shared/camera_rate.hpp"
#include "aircraft_mounts.hpp"

namespace taxi_camera::native_camera {

// Opens cameras on the simulator's frames by count, not by clock. These are
// activation opportunities, not measured rendered frames. The caller must
// apply every returned state through freshly validated owned engine entries.
//
// Each frame carries rate × feeds × frame time cameras (the target per camera
// spread over the measured frame rate), or a fixed per_frame count (Auto).
// Fractions carry to the next frame, never more than one camera's worth, so
// the per-frame count only ever takes two neighbouring values: 10 per camera
// with three cameras at 30 fps renders exactly one camera on every frame, 15
// renders one and two in turn. Timing each camera on its own clock instead let
// a long frame owe two cameras and a short one none, and the 2026-10-07 live
// frames swung between about 25 and 50 ms. Cameras are taken round-robin, so
// each gets the same share; a count of every camera renders all of them on
// every frame and holds their gates open. A frame-time estimate ignores
// stalls, and a stall never opens more than one frame's count. Suspension
// closes every gate.
class RenderSchedule {
 public:
  static constexpr double kSnap = 0.05;

  void configure(unsigned rate, unsigned feeds = 2, unsigned per_frame = 0) noexcept {
    rate_ = std::clamp(rate, kMinimumCameraRate, kMaximumCameraRate);
    feeds_ = std::clamp(feeds, 1u, kMaxCameraFeeds);
    per_frame_ = std::min(per_frame, feeds_);
    if (next_ >= feeds_)
      next_ = 0;
  }

  // Only reset after the previous owned entries have been removed. Rate
  // changes on a live pair keep the frame-time estimate and the rotation.
  void reset() noexcept {
    have_time_ = false;
    previous_time_ = 0;
    frame_ms_ = 0;
    credit_ = 0;
    next_ = 0;
  }

  std::array<bool, kMaxCameraFeeds> tick(std::uint64_t now_ms, bool suspended = false) noexcept {
    std::array<bool, kMaxCameraFeeds> active{};
    if (have_time_ && now_ms < previous_time_) {
      previous_time_ = now_ms;
      credit_ = 0;
      return active;
    }
    if (have_time_ && now_ms > previous_time_) {
      const double gap = static_cast<double>(now_ms - previous_time_);
      if (frame_ms_ <= 0)
        frame_ms_ = gap;
      else if (gap <= 4 * frame_ms_)
        frame_ms_ = 0.9 * frame_ms_ + 0.1 * gap;
    }
    have_time_ = true;
    previous_time_ = now_ms;
    if (suspended)
      return active;
    double count = per_frame_ ? per_frame_ : frame_ms_ > 0 ? rate_ * feeds_ * frame_ms_ / 1000.0 : 1.0;
    // A count within kSnap of a whole number of cameras is that number, so
    // frame-time noise around 10 per camera at 30 fps cannot add or drop a
    // camera on an occasional frame; the rate per camera stays within kSnap.
    const double whole = std::round(count);
    if (whole >= 1 && std::abs(count - whole) <= kSnap * whole)
      count = whole;
    credit_ += count;
    const auto opened = std::min(static_cast<unsigned>(std::floor(credit_ + 1e-9)), feeds_);
    credit_ = std::clamp(credit_ - opened, 0.0, 1.0);
    for (unsigned k = 0; k < opened; ++k)
      active[(next_ + k) % feeds_] = true;
    if (opened < feeds_)
      next_ = (next_ + opened) % feeds_;
    return active;
  }

  unsigned rate() const noexcept { return rate_; }
  unsigned feeds() const noexcept { return feeds_; }
  unsigned per_frame() const noexcept { return per_frame_; }

 private:
  unsigned rate_ = kDefaultCameraRate;
  unsigned feeds_ = 2;
  unsigned per_frame_ = 0;  // Fixed cameras per frame (Auto); 0 paces by rate.
  unsigned next_ = 0;       // First camera of the next frame's round-robin.
  bool have_time_ = false;
  std::uint64_t previous_time_ = 0;
  double frame_ms_ = 0;  // Usual interval between updates.
  double credit_ = 0;    // Fraction of a camera carried to the next frame.
};

}  // namespace taxi_camera::native_camera
