#include "../../src/camera/taxi_speed_cutoff.hpp"
#include "../../src/graphics/capture_progress.hpp"
#include "../../src/graphics/scene_source_state.hpp"
#include "view_readiness_wait_test.hpp"

#include <cassert>
#include <cstdio>
#include <limits>

int main() {
  test_view_readiness_wait();
  using taxi_camera::native_camera::TaxiSpeedCutoff;
  TaxiSpeedCutoff cutoff;
  assert(cutoff.update(100, true, 60.0, true, 3, 100) == 0 && !cutoff.inhibited());
  assert(cutoff.update(101, false, 100, true, 3, 101) == 0 && !cutoff.inhibited());
  assert(cutoff.update(102, true, std::numeric_limits<double>::quiet_NaN(), true, 3, 102) == 0);
  assert(cutoff.update(103, true, -1, true, 3, 103) == 0);
  assert(cutoff.update(104, true, 60.01, false, 3, 0) == 0 && cutoff.inhibited());
  assert(cutoff.update(105, true, 61, true, 1, 106) == 0);    // Future telemetry cannot toggle.
  assert(cutoff.update(1000, true, 61, true, 1, 105) == 0);   // Nor stale telemetry.
  assert(cutoff.update(1001, true, 61, true, 1, 1001) == 1);  // Only left was ON.
  cutoff.sent(0, true);
  assert(cutoff.pending() == 1);
  for (unsigned t = 1002; t < 6000; ++t)
    assert(cutoff.update(t, true, 65, true, 1, t) == 0);  // No repeated accepted toggle.
  assert(cutoff.update(6000, true, 55, true, 1, 6000) == 0 && cutoff.inhibited());
  assert(cutoff.update(6001, true, 55, true, 0, 6001) == 0 && !cutoff.inhibited());  // OFF acknowledged.
  assert(cutoff.update(6002, true, 55, true, 1, 6002) == 0 && !cutoff.inhibited());  // New low-speed ON allowed.

  TaxiSpeedCutoff both;
  assert(both.update(1, true, 80, true, 3, 1) == 3);
  both.sent(0, true);
  both.sent(1, false);  // Synchronous failure can retry, at most once per second.
  assert(both.update(2, true, 80, true, 3, 2) == 0);
  assert(both.update(1000, true, 80, true, 3, 1000) == 0);
  assert(both.update(1001, true, 80, true, 3, 1001) == 2);
  both.sent(1, true);
  assert(both.update(1002, true, 80, true, 2, 1002) == 0 && both.pending() == 2);
  assert(both.update(1003, true, 80, true, 0, 1003) == 0 && both.pending() == 0 && both.inhibited());
  assert(both.update(1004, true, 80, true, 2, 1004) == 0);  // Previous failed-attempt cooldown.
  assert(both.update(2001, true, 80, true, 2, 2001) == 2);  // User re-enabled right above limit.
  both.sent(1, true);
  assert(both.update(2002, false, 0, false, 0, 0) == 0 && both.inhibited());
  assert(both.update(2003, true, 60, true, 0, 2003) == 0 && !both.inhibited());

  TaxiSpeedCutoff crossing;
  assert(crossing.update(1, true, 61, false, 0, 0) == 0 && crossing.inhibited());
  assert(crossing.update(2, true, 59, true, 1, 2) == 1 && crossing.inhibited());
  crossing.sent(0, true);
  assert(crossing.update(3, true, 59, true, 0, 3) == 0 && !crossing.inhibited());

  TaxiSpeedCutoff manual;
  assert(!manual.update(100, true, 60, false, 0, 0, 60, false) && !manual.inhibited());
  assert(!manual.update(101, true, 60.01, false, 0, 0, 60, false) && manual.inhibited() && !manual.pending());
  assert(!manual.update(102, false, 0, false, 0, 0, 60, false) && manual.inhibited());
  assert(!manual.update(103, true, 10, false, 0, 0, 60, false) && !manual.inhibited() && !manual.pending());
  // Even stray ON telemetry cannot produce aircraft commands in manual mode.
  assert(!manual.update(104, true, 70, true, 3, 104, 60, false) && manual.inhibited() && !manual.pending());

  taxi_camera::CaptureProgress progress;
  assert(!progress.observe(1, false, 4024, 800000, true));
  assert(!progress.observe(100, true, 4024, 800000, true));
  assert(!progress.observe(2099, true, 4024, 850000, true));
  assert(progress.observe(2100, true, 4024, 852344, true));
  assert(progress.stalled());  // Observed time-change failure.
  assert(!progress.observe(2101, true, 4024, 852344, true));
  assert(!progress.observe(5000, false, 4024, 852344, true));  // No retry during cleanup/stale pose.
  assert(!progress.observe(5001, true, 4025, 852345, false));
  assert(!progress.stalled());  // Fresh capture restores progress.
  assert(!progress.observe(9000, true, 4025, 852345, true));
  assert(!progress.observe(12000, true, 4025, 852345, true));  // No source draws: no speculation.
  assert(progress.observe(12001, true, 4025, 852346, true));
  assert(!progress.observe(13000, true, 4025, 852347, true));
  assert(!progress.observe(12999, true, 4025, 852348, true));  // Clock regression resets timer.

  // The watchdog never fabricates RT state. Only a new registered allocation
  // can restore creation-state evidence after an unknown command list.
  namespace ss = taxi_camera::source_state;
  ss::Tracker tracker;
  constexpr ss::Key old_key{100, 1}, new_key{200, 2};
  assert(tracker.register_source(old_key, ss::Model::legacy_rt));
  tracker.invalidate_all();
  assert(tracker.state(old_key).model == ss::Model::unknown);
  assert(tracker.register_source(old_key, ss::Model::legacy_rt));
  assert(tracker.state(old_key).model == ss::Model::unknown);  // Same allocation remains invalid.
  tracker.unregister_source(old_key);
  assert(tracker.register_source(new_key, ss::Model::legacy_rt));
  ss::Recording recording;
  assert(recording.append({new_key, ss::Effect::Kind::draw}));
  assert(tracker.apply(recording) && tracker.state(new_key).drawn);
  assert(tracker.state(old_key).model == ss::Model::unknown);
  std::puts("Speed cutoff and capture-stall recovery: PASS");
}
