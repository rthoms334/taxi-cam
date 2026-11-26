#pragma once

#include <cstdint>

namespace taxi_camera {

// Inputs deciding whether capture-stall recovery should watch for unknown GPU source state.
struct CaptureRecoveryDemand {
  unsigned taxi_mask = 0;
  bool door_render = false;
  bool test_scene = false;
  bool outside_pause = false;
  bool failed = false;
  bool output_failed = false;
  bool pair_active = false;
  std::uint32_t requested_feeds = 0;
  bool pose_waiting = false;
  bool view_waiting = false;
  bool pose_valid = false;
};

// Door-only camera views clear taxi_mask and advance door_pages rather than
// taxi frames, so both recovery eligibility and progress count them.
inline bool capture_recovery_eligible(const CaptureRecoveryDemand& demand) noexcept {
  return (demand.taxi_mask != 0 || demand.door_render || demand.test_scene) && !demand.outside_pause && !demand.failed &&
         !demand.output_failed && demand.pair_active && demand.requested_feeds >= 2 && !demand.pose_waiting && !demand.view_waiting &&
         demand.pose_valid;
}

inline std::uint64_t composed_output_count(std::uint64_t frames, std::uint64_t door_pages) noexcept {
  return frames + door_pages;
}

// A native view can stay ready while an unobserved submission invalidates its
// GPU state. Detect that specific stall without admitting an unknown state as RT.
class CaptureProgress {
 public:
  // ~4× a pair interval at the useful-rate ceiling (15 Hz → ~267 ms), floored at
  // 500 ms so a PassState retirement that kept retained_rt can rearm within 0.5 s.
  static constexpr std::uint64_t StallMs = 500;

  bool observe(std::uint64_t now, bool eligible, std::uint64_t frames, std::uint64_t draws, bool unknown_state) noexcept {
    if (!eligible || !unknown_state || frames != frames_ || now < since_) {
      watching_ = false;
      stalled_ = false;
    }
    frames_ = frames;
    if (!eligible || !unknown_state)
      return false;
    if (!watching_) {
      watching_ = true;
      since_ = now;
      draws_ = draws;
      return false;
    }
    if (now - since_ < StallMs || draws <= draws_)
      return false;
    watching_ = false;
    stalled_ = true;
    return true;
  }

  bool stalled() const noexcept { return stalled_; }
  void reset() noexcept { *this = {}; }

 private:
  bool watching_ = false, stalled_ = false;
  std::uint64_t frames_ = 0, draws_ = 0, since_ = 0;
};

}  // namespace taxi_camera
