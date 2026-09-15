#pragma once

#include <cstdint>

namespace taxi_camera {

// A native view can stay ready while an unobserved submission invalidates its
// GPU state. Detect that specific stall without admitting an unknown state as RT.
class CaptureProgress {
 public:
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
    if (now - since_ < 2000 || draws <= draws_)
      return false;
    watching_ = false;
    stalled_ = true;
    return true;
  }

  bool stalled() const noexcept { return stalled_; }

 private:
  bool watching_ = false, stalled_ = false;
  std::uint64_t frames_ = 0, draws_ = 0, since_ = 0;
};

}  // namespace taxi_camera
