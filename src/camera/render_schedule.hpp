#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include "../shared/camera_rate.hpp"
#include "aircraft_mounts.hpp"

namespace taxi_camera::native_camera {

// Requests one enabled observer interval at a time, alternating feeds. These
// are activation opportunities, not measured rendered frames. The caller must
// apply every returned state through freshly validated owned engine entries.
class RenderSchedule {
 public:
  // Parked floors go below the moving minimum; see kMinimumParkedCameraRate.
  void configure(unsigned rate, unsigned feeds = 2) noexcept {
    rate_ = std::clamp(rate, kMinimumParkedCameraRate, kMaximumCameraRate);
    feeds_ = std::clamp(feeds, 1u, kMaxCameraFeeds);
    if (next_feed_ >= feeds_)
      next_feed_ = 0;
  }

  // Only reset after the previous owned entries have been removed. Configuration
  // changes on a live pair retain deadlines, so editing the UI cannot burst.
  void reset() noexcept {
    active_ = {};
    seen_ = {};
    last_ = {};
    have_time_ = false;
    previous_time_ = 0;
    last_any_ = 0;
    next_feed_ = 0;
  }

  std::array<bool, kMaxCameraFeeds> tick(std::uint64_t now_ms, bool suspended = false) noexcept {
    bool was_active = false;
    for (bool on : active_)
      was_active = was_active || on;
    active_ = {};
    if (have_time_ && now_ms < previous_time_) {
      // A clock reversal closes both gates and restarts the cooldown without
      // erasing whether either feed has already received an opportunity.
      for (unsigned i = 0; i < feeds_; ++i)
        last_[i] = now_ms;
      last_any_ = now_ms;
      previous_time_ = now_ms;
      return active_;
    }
    have_time_ = true;
    previous_time_ = now_ms;
    // Even after a long stall, close the last pulse for an entire observer
    // interval. Never leave a gate continuously on while trying to catch up.
    if (was_active || suspended)
      return active_;
    const auto per_feed_ms = (1000u + rate_ - 1) / rate_;
    const auto total_rate = rate_ * feeds_;
    const auto between_ms = (1000u + total_rate - 1) / total_rate;
    bool any_seen = false;
    for (unsigned i = 0; i < feeds_; ++i)
      any_seen = any_seen || seen_[i];
    if ((any_seen && now_ms - last_any_ < between_ms) || (seen_[next_feed_] && now_ms - last_[next_feed_] < per_feed_ms))
      return active_;
    active_[next_feed_] = true;
    seen_[next_feed_] = true;
    last_[next_feed_] = now_ms;
    last_any_ = now_ms;
    next_feed_ = (next_feed_ + 1) % feeds_;
    return active_;
  }

  unsigned rate() const noexcept { return rate_; }
  unsigned feeds() const noexcept { return feeds_; }

 private:
  unsigned rate_ = kDefaultCameraRate;
  unsigned feeds_ = 2;
  unsigned next_feed_ = 0;
  bool have_time_ = false;
  std::uint64_t previous_time_ = 0;
  std::uint64_t last_any_ = 0;
  std::array<bool, kMaxCameraFeeds> active_{};
  std::array<bool, kMaxCameraFeeds> seen_{};
  std::array<std::uint64_t, kMaxCameraFeeds> last_{};
};

}  // namespace taxi_camera::native_camera
