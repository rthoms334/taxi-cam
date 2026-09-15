#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace taxi_camera::native_camera {

// The aircraft owns its TAXI latch. Request its push event only for a freshly
// observed ON side; never write the annunciator Lvar or repeat an accepted
// toggle while waiting for OFF acknowledgement.
class TaxiSpeedCutoff {
 public:
  unsigned update(std::uint64_t now,
                  bool speed_valid,
                  double knots,
                  bool buttons_valid,
                  unsigned on,
                  std::uint64_t button_sample,
                  double limit_knots = 60.0,
                  bool aircraft_buttons = true) noexcept {
    if (speed_valid && std::isfinite(knots) && knots >= 0)
      over_ = knots > limit_knots;
    // A manual-only aircraft has no latch to command or acknowledge. Keep the
    // same speed gate, without permanently waiting for a nonexistent OFF.
    if (!aircraft_buttons) {
      pending_ = 0;
      waiting_ = {};
      return 0;
    }
    // Latch the crossing even when button telemetry is briefly missing. A
    // later low-speed sample must not silently cancel the required OFF.
    if (over_)
      pending_ |= 3u;
    unsigned commands = 0;
    if (!buttons_valid || !button_sample || button_sample > now || now - button_sample > 500)
      return commands;
    on &= 3u;
    for (unsigned side = 0; side < 2; ++side) {
      const auto bit = 1u << side;
      if (!(on & bit)) {
        if (!waiting_[side] || button_sample > sent_sample_[side]) {
          pending_ &= ~bit;
          waiting_[side] = false;
        }
        continue;
      }
      if (over_)
        pending_ |= bit;
      if ((pending_ & bit) && !waiting_[side] && now >= next_attempt_[side]) {
        commands |= bit;
        sent_sample_[side] = button_sample;
        next_attempt_[side] = now + 1000;
      }
    }
    return commands;
  }
  void sent(unsigned side, bool accepted) noexcept {
    if (side < 2 && accepted)
      waiting_[side] = true;
  }
  // A correlated SimConnect rejection proves the accepted command did not
  // execute. Keep OFF pending and the retry delay; release only its ACK wait.
  void rejected(unsigned side) noexcept {
    if (side < 2)
      waiting_[side] = false;
  }
  bool inhibited() const noexcept { return over_ || pending_ != 0; }
  unsigned pending() const noexcept { return pending_; }

 private:
  bool over_ = false;
  unsigned pending_ = 0;
  std::array<bool, 2> waiting_{};
  std::array<std::uint64_t, 2> sent_sample_{}, next_attempt_{};
};

}  // namespace taxi_camera::native_camera
