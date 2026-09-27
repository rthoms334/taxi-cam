#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include "camera_rate.hpp"

namespace taxi_camera {

// Why the schedule runs below the saved camera_rate. The saved value is never
// rewritten; these only describe the rate actually requested from the schedule.
enum CameraRateLimit : unsigned {
  kRateLimitNone = 0,
  kRateLimitParked = 1,      // Ground speed held at zero: parked floor applied.
  kRateLimitPfdRefresh = 2,  // Aircraft PFD redraw rate cannot show more pairs.
  kRateLimitManager = 4,     // Camera-manager cadence saturates at this rate.
  // Rolling straight at a low simulator frame rate: the nose keeps its cadence
  // and every other turn of the other feeds is skipped (NosePriorityPolicy).
  kRateLimitNosePriority = 8,
};

struct EffectiveCameraRate {
  unsigned rate = kDefaultCameraRate;            // Requested from the schedule now.
  unsigned useful_maximum = kMaximumCameraRate;  // Cap while moving, per aircraft.
  unsigned reasons = kRateLimitNone;             // CameraRateLimit bits.
};

// Useful maximum for one aircraft: the lower of its PFD refresh (0 = not
// measured) and the manager ceiling, never below the schedule minimum.
constexpr unsigned useful_camera_rate(unsigned pfd_refresh_hz) noexcept {
  const unsigned pfd = pfd_refresh_hz ? pfd_refresh_hz : kMaximumCameraRate;
  return std::clamp(std::min(pfd, kManagerCeilingCameraRate), kMinimumCameraRate, kMaximumCameraRate);
}

// parked_rate 0 disables the floor. The floor only lowers the rate; a parked
// floor above the moving rate leaves the moving rate in place.
constexpr EffectiveCameraRate effective_camera_rate(unsigned user_rate,
                                                    unsigned pfd_refresh_hz,
                                                    bool parked,
                                                    unsigned parked_rate = kDefaultParkedCameraRate) noexcept {
  EffectiveCameraRate result;
  const unsigned requested = std::clamp(user_rate, kMinimumCameraRate, kMaximumCameraRate);
  result.useful_maximum = useful_camera_rate(pfd_refresh_hz);
  result.rate = requested;
  if (requested > result.useful_maximum) {
    result.rate = result.useful_maximum;
    const unsigned pfd = pfd_refresh_hz ? pfd_refresh_hz : kMaximumCameraRate;
    if (pfd < kManagerCeilingCameraRate)
      result.reasons |= kRateLimitPfdRefresh;
    else
      result.reasons |= kRateLimitManager;
  }
  if (parked && parked_rate) {
    const unsigned floor = std::clamp(parked_rate, kMinimumParkedCameraRate, kMaximumCameraRate);
    if (floor < result.rate) {
      result.rate = floor;
      result.reasons |= kRateLimitParked;
    }
  }
  return result;
}

constexpr const char* camera_rate_limit_name(unsigned reasons) noexcept {
  if (reasons & kRateLimitParked)
    return "parked";
  if (reasons & kRateLimitNosePriority)
    return "nose_priority";
  if (reasons & kRateLimitPfdRefresh)
    return "pfd_refresh";
  if (reasons & kRateLimitManager)
    return "manager_ceiling";
  return "user";
}

// Companion status suffix for the rate in use; empty when the saved rate runs.
constexpr const wchar_t* camera_rate_limit_text(unsigned reasons) noexcept {
  if (reasons & kRateLimitParked)
    return L" (parked floor)";
  if (reasons & kRateLimitNosePriority)
    return L" (nose priority: tail every other update while rolling straight)";
  if (reasons & kRateLimitPfdRefresh)
    return L" (capped at the PFD refresh rate)";
  if (reasons & kRateLimitManager)
    return L" (capped at the camera-manager ceiling)";
  return L"";
}

// Ground-speed hysteresis for the parked floor. Parked needs the public GROUND
// VELOCITY below kParkedBelowKnots continuously for kParkedSettleMs (the dwell:
// a rolling aircraft never accumulates park time, and a band sample restarts
// it). Any sample at or above kMovingAboveKnots restores the moving rate at
// once. Between the two thresholds the current state holds: a parked aircraft
// whose GROUND VELOCITY jitters to 0.20–0.34 kt stays at the floor, a moving one
// stays at the saved rate. Missing or stale telemetry is treated as moving so a
// telemetry gap never lowers the rate. A parked live A350 reads 0.00–0.09 kt;
// 0.4 kt is an aircraft still rolling to a stop, which the earlier 0.5/1.0 kt
// band parked at 5 fps while it was visibly moving (0.9.42 low-speed report),
// and the single 0.2 kt edge of 0.9.48–0.9.56 stepped the inset 5<->10 fps
// twice per creep (0.9.56 flash/jump report). Re-parking after any motion needs
// the full settle again.
class ParkedRatePolicy {
 public:
  static constexpr double kParkedBelowKnots = 0.2;
  static constexpr double kMovingAboveKnots = 0.35;
  static constexpr std::uint64_t kParkedSettleMs = 3000;

  bool update(std::uint64_t now_ms, bool speed_valid, double knots) noexcept {
    if (!speed_valid || !std::isfinite(knots) || knots < 0 || knots >= kMovingAboveKnots) {
      reset();
      return parked_;
    }
    if (knots >= kParkedBelowKnots) {
      // Hysteresis band: hold the state; a moving aircraft restarts its settle.
      still_ = false;
      still_since_ = 0;
      return parked_;
    }
    if (!still_ || now_ms < still_since_) {
      still_ = true;
      still_since_ = now_ms;
    }
    if (!parked_ && now_ms - still_since_ >= kParkedSettleMs)
      parked_ = true;
    return parked_;
  }

  bool parked() const noexcept { return parked_; }
  void reset() noexcept {
    parked_ = false;
    still_ = false;
    still_since_ = 0;
  }

 private:
  bool parked_ = false;
  bool still_ = false;
  std::uint64_t still_since_ = 0;
};

// Signed change of heading, in degrees, from previous_forward to forward about
// the current up axis. Both forward vectors are projected onto the plane at
// right angles to up, so pitch changes on the ground do not count as turning.
// Returns NaN for degenerate input.
inline double heading_change_degrees(const std::array<double, 3>& previous_forward,
                                     const std::array<double, 3>& forward,
                                     const std::array<double, 3>& up) noexcept {
  const auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const auto flatten = [&](const std::array<double, 3>& v) {
    const double along = dot(v, up);
    return std::array<double, 3>{v[0] - along * up[0], v[1] - along * up[1], v[2] - along * up[2]};
  };
  const auto a = flatten(previous_forward), b = flatten(forward);
  const std::array<double, 3> cross{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  const double cosine = dot(a, b), sine = dot(cross, up);
  if (!std::isfinite(cosine) || !std::isfinite(sine) || (cosine == 0 && sine == 0))
    return std::numeric_limits<double>::quiet_NaN();
  return std::atan2(sine, cosine) * 57.29577951308232;
}

// Dynamic tail rate. While the aircraft rolls straight and the simulator runs
// slowly, the nose feed keeps its cadence and every other turn of the other
// feeds becomes an idle slot (nose, tail, nose, idle): about a quarter fewer
// extra renders with two feeds. Below about 20 fps the one-closed-update rule
// already caps the pulses, so a lower tail rate alone would change nothing.
// Turning (wing, tail and main-gear clearance), parked (its own floor), a fast
// simulator (both feeds already refresh often), a disabled setting and missing
// or stale telemetry all keep every feed equal. A turn is entered at once and
// left only after a continuous straight hold; the frame rate uses hysteresis.
class NosePriorityPolicy {
 public:
  static constexpr double kTurnEnterDegreesPerSecond = 3.0;
  static constexpr double kTurnExitDegreesPerSecond = 1.5;
  static constexpr std::uint64_t kStraightHoldMs = 2000;
  // Heading rate is measured between pose samples at least this far apart; a
  // pose older than kHeadingStaleMs, or a longer gap, restarts the estimate.
  static constexpr std::uint64_t kHeadingWindowMs = 250;
  static constexpr std::uint64_t kHeadingStaleMs = 1000;
  // SIMCONNECT_PERIOD_SIM_FRAME samples per second over each window.
  static constexpr std::uint64_t kFrameWindowMs = 2000;
  static constexpr double kPriorityBelowFps = 24;
  static constexpr double kEqualAboveFps = 27;
  static constexpr double kMinimumMeasuredFps = 1;

  struct Input {
    std::uint64_t now_ms = 0;
    bool enabled = false;
    bool moving = false;  // Fresh ground speed and not held at the parked floor.
    bool pose_valid = false;
    std::uint64_t pose_sample_ms = 0;
    std::array<double, 3> forward{}, up{};
    std::uint64_t sim_frames = 0;  // Monotonic accepted SIM_FRAME samples.
  };

  bool update(const Input& in) noexcept {
    observe_heading(in);
    observe_frames(in.now_ms, in.sim_frames);
    priority_ = in.enabled && in.moving && heading_known_ && !turning_ && frames_known_ && slow_;
    return priority_;
  }
  bool priority() const noexcept { return priority_; }
  bool turning() const noexcept { return turning_; }
  // Last measured magnitudes; NaN until measured.
  double turn_rate() const noexcept { return heading_known_ ? turn_rate_ : std::numeric_limits<double>::quiet_NaN(); }
  double frame_rate() const noexcept { return frames_known_ ? frame_rate_ : std::numeric_limits<double>::quiet_NaN(); }

 private:
  void forget_heading() noexcept {
    heading_known_ = false;
    have_reference_ = false;
    turning_ = true;  // Unknown counts as turning: every feed stays equal.
    straight_since_ = 0;
  }
  void observe_heading(const Input& in) noexcept {
    if (!in.pose_valid || !in.pose_sample_ms || in.pose_sample_ms > in.now_ms || in.now_ms - in.pose_sample_ms > kHeadingStaleMs) {
      forget_heading();
      return;
    }
    if (have_reference_ && in.pose_sample_ms == reference_ms_)
      return;
    if (!have_reference_ || in.pose_sample_ms < reference_ms_ || in.pose_sample_ms - reference_ms_ > kHeadingStaleMs) {
      if (have_reference_)
        forget_heading();
      have_reference_ = true;
      reference_ms_ = in.pose_sample_ms;
      reference_forward_ = in.forward;
      return;
    }
    if (in.pose_sample_ms - reference_ms_ < kHeadingWindowMs)
      return;
    const double change = heading_change_degrees(reference_forward_, in.forward, in.up);
    const double seconds = static_cast<double>(in.pose_sample_ms - reference_ms_) / 1000;
    reference_ms_ = in.pose_sample_ms;
    reference_forward_ = in.forward;
    if (!std::isfinite(change)) {
      forget_heading();
      return;
    }
    turn_rate_ = std::abs(change) / seconds;
    heading_known_ = true;
    if (turn_rate_ >= kTurnEnterDegreesPerSecond) {
      turning_ = true;
      straight_since_ = 0;
    } else if (turn_rate_ < kTurnExitDegreesPerSecond) {
      if (!straight_since_)
        straight_since_ = in.pose_sample_ms;
      if (turning_ && in.pose_sample_ms - straight_since_ >= kStraightHoldMs)
        turning_ = false;
    } else if (turning_) {
      straight_since_ = 0;  // Still curving gently: the straight hold restarts.
    }
  }
  void observe_frames(std::uint64_t now_ms, std::uint64_t frames) noexcept {
    if (!window_ms_ || now_ms < window_ms_ || frames < window_frames_) {
      window_ms_ = now_ms ? now_ms : 1;
      window_frames_ = frames;
      return;
    }
    if (now_ms - window_ms_ < kFrameWindowMs)
      return;
    const double fps = static_cast<double>(frames - window_frames_) * 1000 / static_cast<double>(now_ms - window_ms_);
    window_ms_ = now_ms;
    window_frames_ = frames;
    if (fps < kMinimumMeasuredFps) {
      frames_known_ = false;  // Paused or telemetry stalled: not a slow simulator.
      return;
    }
    frame_rate_ = fps;
    if (!frames_known_)
      slow_ = fps < kPriorityBelowFps;
    else if (fps < kPriorityBelowFps)
      slow_ = true;
    else if (fps > kEqualAboveFps)
      slow_ = false;
    frames_known_ = true;
  }

  bool priority_ = false;
  bool heading_known_ = false, have_reference_ = false, turning_ = true;
  std::uint64_t reference_ms_ = 0, straight_since_ = 0;
  std::array<double, 3> reference_forward_{};
  double turn_rate_ = 0;
  bool frames_known_ = false, slow_ = false;
  std::uint64_t window_ms_ = 0, window_frames_ = 0;
  double frame_rate_ = 0;
};

}  // namespace taxi_camera
