#pragma once
#include <cstdint>

namespace taxi_camera::standalone {
// Pauses camera rendering while the simulator's main view is not the cockpit.
// The cockpit displays cannot be seen from outside, yet each camera view still
// cost a render on every frame there (issue 97: switching the PMDG 777 to the
// outside view with the cameras on gave 12 s of 80-100 ms frames while the
// simulator loaded about 3 GB for the view).
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
    if (!paused_ && now_ms - outside_since_ms_ >= kEnterMs) {
      paused_ = true;
      ++pauses_;
    }
    return paused_;
  }
  bool paused() const noexcept { return paused_; }
  // Pauses started since the bridge worker started, for bridge.log.
  std::uint64_t pauses() const noexcept { return pauses_; }

 private:
  bool paused_ = false;
  std::uint64_t outside_since_ms_ = 0, last_outside_ms_ = 0, pauses_ = 0;
};
}  // namespace taxi_camera::standalone
