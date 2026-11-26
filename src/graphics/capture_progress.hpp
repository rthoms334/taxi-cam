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

inline bool capture_recovery_ready(const CaptureRecoveryDemand& demand) noexcept {
  return !demand.outside_pause && !demand.failed && !demand.output_failed && demand.pair_active && demand.requested_feeds >= 2 &&
         !demand.pose_waiting && !demand.view_waiting && demand.pose_valid;
}

inline bool taxi_capture_recovery_eligible(const CaptureRecoveryDemand& demand) noexcept {
  return (demand.taxi_mask != 0 || demand.test_scene) && capture_recovery_ready(demand);
}

inline bool door_capture_recovery_eligible(const CaptureRecoveryDemand& demand) noexcept {
  return demand.door_render && capture_recovery_ready(demand);
}

// Door-only camera views clear taxi_mask and advance door_pages rather than
// taxi frames, so both recovery eligibility and progress count them.
inline bool capture_recovery_eligible(const CaptureRecoveryDemand& demand) noexcept {
  return taxi_capture_recovery_eligible(demand) || door_capture_recovery_eligible(demand);
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

// Track taxi composition (frames) and door camera pages (door_pages)
// independently so rising door pages cannot hide a stalled taxi capture, nor
// rising taxi frames hide a stalled door capture.
class ForegroundCaptureProgress {
 public:
  struct Observation {
    bool rearm = false;
    bool scene_progress = false;
  };

  Observation observe(std::uint64_t now,
                      const CaptureRecoveryDemand& demand,
                      std::uint64_t frames,
                      std::uint64_t door_pages,
                      std::uint64_t draws,
                      bool unknown_state) noexcept {
    const bool taxi_eligible = taxi_capture_recovery_eligible(demand);
    const bool door_eligible = door_capture_recovery_eligible(demand);
    const bool taxi_advanced = frames != last_frames_;
    const bool door_advanced = door_pages != last_door_pages_;
    last_frames_ = frames;
    last_door_pages_ = door_pages;
    const bool taxi_stalled = taxi_.observe(now, taxi_eligible, frames, draws, unknown_state);
    const bool door_stalled = door_.observe(now, door_eligible, door_pages, draws, unknown_state);
    // Only credit SceneRecovery from door pages when door-only is active;
    // while taxi cameras are demanded, door progress must not mask a taxi stall.
    const bool scene_progress = (taxi_eligible && taxi_advanced) || (!taxi_eligible && door_eligible && door_advanced);
    return {taxi_stalled || door_stalled, scene_progress};
  }

  bool stalled() const noexcept { return taxi_.stalled() || door_.stalled(); }
  bool taxi_stalled() const noexcept { return taxi_.stalled(); }
  bool door_stalled() const noexcept { return door_.stalled(); }
  void reset() noexcept {
    taxi_.reset();
    door_.reset();
  }

 private:
  CaptureProgress taxi_;
  CaptureProgress door_;
  std::uint64_t last_frames_ = 0;
  std::uint64_t last_door_pages_ = 0;
};

}  // namespace taxi_camera
