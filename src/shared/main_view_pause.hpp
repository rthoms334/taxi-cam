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
// Input is the public CAMERA STATE SimVar, where 2 is Cockpit. The value table
// is MSFS 2020's (the MSFS 2024 page is gone), which says not every value is
// listed: https://docs.flightsimulator.com/html/Programming_Tools/SimVars/Camera_Variables.htm
// Only the exterior cameras listed there are outside (outside_camera_state).
// Any other value counts as no reading: MSFS 2024 reported 7 (listed as "Six
// DoF") for the whole time the user sat in the PMDG 777's cockpit after a fresh
// flight load, and 29 and 34 while loading (2026-11-03), and pausing on 7 froze
// the displays on their first picture. A reading that is missing, stale or not
// an exterior camera never starts a pause, so a simulator without the SimVar
// keeps today's behaviour. An outside reading must persist kEnterMs before the
// pause starts, so a passing view does not close the cameras; a cockpit reading
// ends it at once. While paused, no outside reading (a telemetry gap while
// frames stall loading the outside view, or a state between two exterior
// cameras) keeps the pause for kHoldMs after the last outside reading and then
// ends it.
inline bool outside_camera_state(unsigned state) noexcept {
  switch (state) {
    case 3:   // External/Chase
    case 4:   // Drone
    case 5:   // Fixed on Plane
    case 6:   // Environment
    case 9:   // Showcase
    case 10:  // Drone Aircraft
    case 17:  // Replay
    case 19:  // Drone Top-Down
    case 25:  // Follow Traffic Aircraft
      return true;
    default:
      return false;
  }
}
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
    if (!valid || !outside_camera_state(state)) {
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
