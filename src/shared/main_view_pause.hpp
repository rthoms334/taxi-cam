#pragma once
#include <cstdint>

namespace taxi_camera::standalone {
// Pauses camera rendering while the simulator's main view is not the cockpit.
// The cockpit displays cannot be seen from outside, yet the camera views kept
// rendering there at their scheduled rate (issue 97: switching the PMDG 777 to
// the outside view with the cameras on gave about 12 s of 80-100 ms frames while
// the simulator's graphics memory rose from 9.2 to 12.2 GB; the cameras' share
// of that is not measured).
//
// Input is the public CAMERA STATE SimVar, where 2 is Cockpit:
// https://docs.flightsimulator.com/msfs2024/retail/programming-apis/simvars/camera-variables/
// Every other documented value is a camera outside the cockpit (chase, drone,
// fixed on plane, showcase, replay, world map, transitions). A reading that is
// missing or stale never starts a pause, so a simulator without the SimVar keeps
// today's behaviour. A non-cockpit reading must persist kEnterMs before the
// pause starts, so a passing transition state does not close the cameras; a
// cockpit reading ends it at once. While paused, a telemetry gap keeps the pause
// for kHoldMs after the last outside reading (frames stall while the simulator
// loads the outside view) and then ends it.
class MainViewPause {
 public:
  static constexpr unsigned kCockpit = 2;
  static constexpr std::uint64_t kEnterMs = 500;
  static constexpr std::uint64_t kHoldMs = 2000;

  bool observe(std::uint64_t now_ms, bool valid, unsigned state) noexcept {
    if (valid && state == kCockpit) {
      outside_since_ms_ = 0;
      paused_ = false;
      return false;
    }
    if (!valid) {
      outside_since_ms_ = 0;
      if (paused_ && (now_ms < last_outside_ms_ || now_ms - last_outside_ms_ > kHoldMs))
        paused_ = false;
      return paused_;
    }
    last_outside_ms_ = now_ms;
    if (!outside_since_ms_ || now_ms < outside_since_ms_)
      outside_since_ms_ = now_ms;
    if (!paused_ && now_ms - outside_since_ms_ >= kEnterMs)
      paused_ = true;
    return paused_;
  }
  bool paused() const noexcept { return paused_; }

 private:
  bool paused_ = false;
  std::uint64_t outside_since_ms_ = 0, last_outside_ms_ = 0;
};

// Whether the bridge pauses camera rendering for the main view this tick. Only
// requested cameras (TAXI, manual or the door camera) pause. Background warmup
// shares the render gates and has a single attempt per session, so nothing
// pauses while it runs, a live door camera included. The scene test,
// calibration and display snapshots keep rendering in any view.
struct OutsidePauseInput {
  bool main_view_outside = false;
  bool cameras_requested = false;
  bool demand_suspended = false;
  bool background_warmup = false;
  bool test_scene = false;
  bool calibration = false;
  bool snapshot_pending = false;
};
inline bool outside_view_pause(const OutsidePauseInput& in) noexcept {
  return in.main_view_outside && in.cameras_requested && !in.demand_suspended && !in.background_warmup && !in.test_scene &&
         !in.calibration && !in.snapshot_pending;
}
}  // namespace taxi_camera::standalone
