#include "../../src/camera/still_frame_hold.hpp"
#include "../../src/camera/render_schedule.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using taxi_camera::native_camera::RenderSchedule;
using taxi_camera::native_camera::StillFrameHold;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void first_pair_then_hold() {
  StillFrameHold hold;
  require(!hold.holding(), "Default constructed hold was already active");
  require(!hold.update(1000, true, 0.0, 1000, 2, true, false), "Hold engaged before either feed activated");
  hold.note_activation(0);
  require(!hold.update(1067, true, 0.1, 1067, 2, true, false), "Hold engaged before the second feed activated");
  hold.note_activation(1);
  require(!hold.update(1134, true, 0.1, 1134, 2, false, false), "Hold engaged before both views were ready");
  require(hold.update(1134, true, 0.1, 1134, 2, true, false), "Parked primed pair did not enter still-frame hold");
  require(hold.holding(), "Holding flag did not match update result");
}

void hysteresis_and_stale_speed() {
  StillFrameHold hold;
  hold.note_activation(0);
  hold.note_activation(1);
  require(hold.update(2000, true, 0.0, 2000, 2, true, false), "Parked pair did not hold");
  require(hold.update(2100, true, 0.7, 2100, 2, true, false), "Hold released in the 0.5–1.0 kt band");
  require(!hold.update(2200, true, 1.0, 2200, 2, true, false), "Hold did not resume at 1.0 kt");
  require(!hold.holding(), "Resume left the holding flag set");
  require(!hold.update(2300, true, 0.7, 2300, 2, true, false), "Re-entered hold above the 0.5 kt arm threshold");
  require(hold.update(2400, true, 0.4, 2400, 2, true, false), "Did not re-enter hold below 0.5 kt");
  require(!hold.update(3000, true, 0.0, 2499, 2, true, false), "Stale ground speed kept the hold");
  require(!hold.holding(), "Stale sample left the holding flag set");
  require(hold.update(3100, true, 0.0, 3100, 2, true, false), "Fresh parked sample did not restore hold");
}

void invalid_speed_never_holds() {
  StillFrameHold hold;
  hold.note_activation(0);
  hold.note_activation(1);
  require(!hold.update(1000, false, 0.0, 1000, 2, true, false), "Invalid speed entered hold");
  require(!hold.update(1000, true, std::numeric_limits<double>::quiet_NaN(), 1000, 2, true, false), "NaN speed entered hold");
  require(!hold.update(1000, true, -0.1, 1000, 2, true, false), "Negative speed entered hold");
  require(!hold.update(1000, true, 0.0, 0, 2, true, false), "Zero sample time entered hold");
  require(!hold.update(1000, true, 0.0, 1001, 2, true, false), "Future sample entered hold");
  require(hold.update(1000, true, 0.0, 1000, 2, true, false), "Valid parked sample did not enter hold");
  require(!hold.update(1501, true, 0.0, 1000, 2, true, false), "Exactly-stale 501 ms sample kept the hold");
  require(hold.update(1500, true, 0.0, 1000, 2, true, false), "500 ms freshness window rejected a just-fresh sample");
}

void force_resume_requires_new_pulses() {
  StillFrameHold hold;
  hold.note_activation(0);
  hold.note_activation(1);
  require(hold.update(4000, true, 0.0, 4000, 2, true, false), "Setup hold missing");
  require(!hold.update(4100, true, 0.0, 4100, 2, true, true), "Force resume left the hold engaged");
  require(!hold.holding(), "Force resume left the holding flag set");
  require(!hold.update(4200, true, 0.0, 4200, 2, true, false), "Force resume reused prior activations");
  hold.note_activation(0);
  hold.note_activation(1);
  require(hold.update(4300, true, 0.0, 4300, 2, true, false), "Re-primed pair did not hold after force resume");
}

void single_feed_and_feed_count_change() {
  StillFrameHold hold;
  hold.note_activation(0);
  require(hold.update(5000, true, 0.0, 5000, 1, true, false), "Single-feed parked pair did not hold");
  require(!hold.update(5100, true, 0.0, 5100, 2, true, false), "Two-feed mode kept a single-feed hold");
  hold.note_activation(1);
  require(hold.update(5200, true, 0.0, 5200, 2, true, false), "Second feed did not re-arm two-feed hold");
}

void reset_clears_arming() {
  StillFrameHold hold;
  hold.note_activation(0);
  hold.note_activation(1);
  require(hold.update(6000, true, 0.0, 6000, 2, true, false), "Reset setup hold missing");
  hold.reset();
  require(!hold.holding(), "Reset left the holding flag set");
  require(!hold.update(6100, true, 0.0, 6100, 2, true, false), "Reset reused prior activations");
}

void parked_default_rate_stops_pulses() {
  RenderSchedule schedule;
  StillFrameHold hold;
  schedule.configure(15, 2);
  unsigned pulses = 0;
  for (std::uint64_t now = 0; now < 10000; now += 10) {
    const bool still = hold.update(now, true, 0.0, now, 2, true, false);
    const auto active = schedule.tick(now, still);
    require(!(active[0] && active[1]), "Still-frame hold opened both gates");
    for (unsigned feed = 0; feed < 2; ++feed) {
      if (!active[feed])
        continue;
      ++pulses;
      hold.note_activation(feed);
    }
  }
  require(pulses == 2, "Parked default-15 path did not pulse each feed once");
  require(hold.holding(), "Parked default-15 path did not stay in still-frame hold");
}

void moving_aircraft_keeps_default_rate() {
  RenderSchedule schedule;
  StillFrameHold hold;
  schedule.configure(15, 2);
  unsigned pulses = 0;
  for (std::uint64_t now = 0; now < 10000; now += 20) {
    const bool still = hold.update(now, true, 8.0, now, 2, true, false);
    const auto active = schedule.tick(now, still);
    for (unsigned feed = 0; feed < 2; ++feed) {
      if (!active[feed])
        continue;
      ++pulses;
      hold.note_activation(feed);
    }
  }
  require(!hold.holding(), "Taxiing 8 kt entered still-frame hold");
  require(pulses == 250, "Taxiing default-15 cadence changed");
}

void resume_from_hold_does_not_burst() {
  RenderSchedule schedule;
  StillFrameHold hold;
  schedule.configure(15, 2);
  require(schedule.tick(0)[0], "Initial pulse missing");
  hold.note_activation(0);
  require(schedule.tick(1) == std::array<bool, 2>{}, "Initial pulse did not close");
  require(schedule.tick(34)[1], "Second feed pulse missing");
  hold.note_activation(1);
  require(hold.update(67, true, 0.0, 67, 2, true, false), "Parked pair did not hold after both pulses");
  for (std::uint64_t now = 68; now < 5000; ++now)
    require(schedule.tick(now, hold.update(now, true, 0.0, now, 2, true, false)) == std::array<bool, 2>{},
            "Still-frame hold left a gate open");
  require(!hold.update(5000, true, 3.0, 5000, 2, true, false), "Rolling start did not release hold");
  require(schedule.tick(5000)[0], "Resume did not continue with the next feed");
  require(schedule.tick(5001) == std::array<bool, 2>{}, "Resume skipped the mandatory closed interval");
  require(schedule.tick(5002) == std::array<bool, 2>{}, "Resume caused a catch-up burst");
}
}  // namespace

int main() {
  try {
    first_pair_then_hold();
    hysteresis_and_stale_speed();
    invalid_speed_never_holds();
    force_resume_requires_new_pulses();
    single_feed_and_feed_count_change();
    reset_clears_arming();
    parked_default_rate_stops_pulses();
    moving_aircraft_keeps_default_rate();
    resume_from_hold_does_not_burst();
    std::printf("PASS: %u still-frame-hold checks; first-pair arming, 0.5/1.0 kt hysteresis, stale resume and no catch-up.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
