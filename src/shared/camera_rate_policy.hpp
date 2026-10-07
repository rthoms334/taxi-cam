#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include "camera_rate.hpp"

namespace taxi_camera {

// Why the cameras run below the mode's target. The saved camera_rate is never
// rewritten; these only describe the rate requested or reached.
enum CameraRateLimit : unsigned {
  kRateLimitNone = 0,
  kRateLimitPfdRefresh = 2,  // Aircraft PFD redraw rate cannot show more images.
  kRateLimitSimulator = 4,   // The simulator updates more slowly than the target.
};

// The target per camera of the fps modes. automatic has no fixed target (it
// renders auto_cameras_per_frame) and reads as Performance here; unknown modes
// fall back to Balanced.
constexpr unsigned camera_mode_target(unsigned mode, unsigned custom_rate) noexcept {
  switch (static_cast<CameraMode>(mode)) {
    case CameraMode::performance:
      return kPerformanceCameraRate;
    case CameraMode::smooth:
      return kSmoothCameraRate;
    case CameraMode::custom:
      return std::clamp(custom_rate, kMinimumCameraRate, kMaximumCameraRate);
    case CameraMode::automatic:
      return kPerformanceCameraRate;
    default:
      return kBalancedCameraRate;
  }
}

struct EffectiveCameraRate {
  unsigned rate = kDefaultCameraRate;            // Requested from the schedule now.
  unsigned useful_maximum = kMaximumCameraRate;  // The aircraft's PFD refresh cap.
  unsigned reasons = kRateLimitNone;             // CameraRateLimit bits.
};

// The target, capped at the aircraft's measured PFD refresh (0 = not measured):
// composing faster than the display redraws cannot reach the screen.
constexpr EffectiveCameraRate effective_camera_rate(unsigned target, unsigned pfd_refresh_hz) noexcept {
  EffectiveCameraRate result;
  result.rate = std::clamp(target, kMinimumCameraRate, kMaximumCameraRate);
  result.useful_maximum = std::clamp(pfd_refresh_hz ? pfd_refresh_hz : kMaximumCameraRate, kMinimumCameraRate, kMaximumCameraRate);
  if (result.rate > result.useful_maximum) {
    result.rate = result.useful_maximum;
    result.reasons |= kRateLimitPfdRefresh;
  }
  return result;
}

// Images per camera per second: every camera renders on every due frame, at
// most one per simulator update. NaN while the update rate is unknown.
inline double reachable_camera_rate(unsigned rate, double update_hz) noexcept {
  return std::isfinite(update_hz) && update_hz > 0 ? std::min<double>(rate, update_hz) : std::numeric_limits<double>::quiet_NaN();
}

constexpr const char* camera_rate_limit_name(unsigned reasons) noexcept {
  if (reasons & kRateLimitPfdRefresh)
    return "pfd_refresh";
  if (reasons & kRateLimitSimulator)
    return "simulator_fps";
  return "target";
}

// Companion status suffix for the rate reached; empty when the target runs.
constexpr const wchar_t* camera_rate_limit_text(unsigned reasons) noexcept {
  if (reasons & kRateLimitPfdRefresh)
    return L" (capped at the PFD refresh rate)";
  if (reasons & kRateLimitSimulator)
    return L" (limited by the simulator frame rate)";
  return L"";
}

// Frame-time spread of one log window from a 1 ms histogram (the last bucket
// holds every longer interval): the median, the 95th percentile and the
// slowest bucket, in ms. An even 20, 20, 60 ms rhythm shows as 20/60/60, a
// steady one as close numbers. Zero counts give zeros.
struct FrameTimeSpread {
  unsigned median = 0, p95 = 0, slowest = 0;
  std::uint64_t frames = 0;
};
template <std::size_t N>
FrameTimeSpread frame_time_spread(const std::array<std::uint32_t, N>& counts) noexcept {
  FrameTimeSpread result;
  for (const auto count : counts)
    result.frames += count;
  if (!result.frames)
    return result;
  std::uint64_t seen = 0;
  bool median = false, p95 = false;
  for (std::size_t ms = 0; ms < N; ++ms) {
    if (!counts[ms])
      continue;
    seen += counts[ms];
    if (!median && seen * 2 >= result.frames) {
      result.median = static_cast<unsigned>(ms);
      median = true;
    }
    if (!p95 && seen * 100 >= result.frames * 95) {
      result.p95 = static_cast<unsigned>(ms);
      p95 = true;
    }
    result.slowest = static_cast<unsigned>(ms);
  }
  return result;
}

// Camera-manager updates per second from the probe's monotonic update count,
// over windows of kWindowMs. NaN until a window completes, and again after a
// window below kMinimumHz (paused or stalled: not a slow simulator) or above
// kMaximumHz (not a frame rate: a counter that jumped).
class UpdateRateMeter {
 public:
  static constexpr std::uint64_t kWindowMs = 2000;
  static constexpr double kMinimumHz = 1, kMaximumHz = 1000;

  double update(std::uint64_t now_ms, std::uint64_t updates) noexcept {
    if (!window_ms_ || now_ms < window_ms_ || updates < window_updates_) {
      window_ms_ = now_ms ? now_ms : 1;
      window_updates_ = updates;
      return rate();
    }
    if (now_ms - window_ms_ < kWindowMs)
      return rate();
    const double hz = static_cast<double>(updates - window_updates_) * 1000 / static_cast<double>(now_ms - window_ms_);
    window_ms_ = now_ms;
    window_updates_ = updates;
    known_ = hz >= kMinimumHz && hz <= kMaximumHz;
    hz_ = hz;
    ++windows_;
    return rate();
  }
  double rate() const noexcept { return known_ ? hz_ : std::numeric_limits<double>::quiet_NaN(); }
  // Completed windows so far; a new value means rate() describes a new window.
  std::uint64_t windows() const noexcept { return windows_; }

 private:
  bool known_ = false;
  double hz_ = 0;
  std::uint64_t window_ms_ = 0, window_updates_ = 0, windows_ = 0;
};

// Auto camera mode. Steps through 1, 2 and all cameras per frame
// (auto_cameras_per_frame) to give the smoothest cameras that keep the
// simulator's update rate within kAllowedLoss of its rate without camera
// work, and never below kMinimumFps. It never goes below one camera per frame. Each completed rate window is judged once:
// - Cameras off (no TAXI demand): the window is the camera-off rate, except
//   the first one, which spans the switch (2026-10-07 live: 11.4 Hz while the
//   cameras stopped for a mode change). The level holds, so turning the cameras
//   on resumes the last level that fitted.
// - The first window after a level change or activation spans both states
//   and is discarded.
// - Below the floor: back off one level at once and keep the level just left
//   off for kCooldownMs, doubling per failure up to kMaximumCooldownMs, so a
//   level that does not fit is not retried every few seconds.
// - After kHoldWindows clean windows with kStepUpHeadroom above the floor,
//   take the next level if it is not cooling down and gives each camera more
//   images (next_helps: there are more cameras than the level renders).
// While the cameras run, the camera-off rate is never below a measured rate,
// and at the first level it follows the measured rate, so a moment of light
// scenery cannot hold the floor up for long. An unknown update rate holds.
class AutoCameraPolicy {
 public:
  static constexpr unsigned kLevels = kAutoCameraLevels;
  static constexpr double kAllowedLoss = 0.10;
  static constexpr double kMinimumFps = 20;
  static constexpr double kStepUpHeadroom = 1.03;
  static constexpr unsigned kHoldWindows = 3;
  static constexpr unsigned kForgiveWindows = 30;  // A level that held this long forgets its failures.
  static constexpr std::uint64_t kCooldownMs = 60000, kMaximumCooldownMs = 480000;

  struct Input {
    std::uint64_t now_ms = 0;
    std::uint64_t window = 0;  // UpdateRateMeter::windows()
    double fps = std::numeric_limits<double>::quiet_NaN();
    bool cameras_active = false;
    bool next_helps = true;
  };

  unsigned update(const Input& in) noexcept {
    if (in.window == window_)
      return level_;
    window_ = in.window;
    if (!std::isfinite(in.fps) || in.fps <= 0)
      return level_;
    if (!in.cameras_active) {
      if (off_settled_) {
        base_ = in.fps;
        base_known_ = true;
      }
      off_settled_ = true;
      settled_ = false;
      held_ = 0;
      return level_;
    }
    off_settled_ = false;
    if (!settled_) {
      settled_ = true;
      return level_;
    }
    if (!base_known_ || in.fps > base_) {
      base_ = in.fps;
      base_known_ = true;
    } else if (level_ == 0) {
      base_ = 0.75 * base_ + 0.25 * in.fps;
    }
    const double floor = this->floor();
    if (level_ > 0 && in.fps < floor) {
      const auto failures = ++failures_[level_];
      const auto cooldown = std::min<std::uint64_t>(kMaximumCooldownMs, kCooldownMs << std::min(failures - 1, 3u));
      blocked_until_[level_] = in.now_ms + cooldown;
      --level_;
      settled_ = false;
      held_ = 0;
      return level_;
    }
    if (++held_ >= kForgiveWindows)
      failures_[level_] = 0;
    if (held_ >= kHoldWindows && level_ + 1 < kLevels && in.now_ms >= blocked_until_[level_ + 1] && in.next_helps &&
        in.fps >= floor * kStepUpHeadroom) {
      ++level_;
      settled_ = false;
      held_ = 0;
    }
    return level_;
  }
  unsigned level() const noexcept { return level_; }
  // Camera-off update rate the floor follows; NaN until measured.
  double base() const noexcept { return base_known_ ? base_ : std::numeric_limits<double>::quiet_NaN(); }
  double floor() const noexcept { return std::max(kMinimumFps, base_known_ ? base_ * (1 - kAllowedLoss) : kMinimumFps); }
  void reset() noexcept { *this = AutoCameraPolicy{}; }

 private:
  unsigned level_ = 0, held_ = 0;
  bool settled_ = false, base_known_ = false, off_settled_ = false;
  double base_ = 0;
  std::uint64_t window_ = 0;
  std::array<unsigned, kLevels> failures_{};
  std::array<std::uint64_t, kLevels> blocked_until_{};
};

}  // namespace taxi_camera
