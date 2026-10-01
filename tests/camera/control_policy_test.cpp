#include "../../src/graphics/capture_progress.hpp"
#include "../../src/graphics/scene_source_state.hpp"
#include "view_readiness_wait_test.hpp"

#include <cassert>
#include <cstdio>

int main() {
  test_view_readiness_wait();
  taxi_camera::CaptureProgress progress;
  assert(!progress.observe(1, false, 4024, 800000, true));
  assert(!progress.observe(100, true, 4024, 800000, true));
  assert(!progress.observe(100 + taxi_camera::CaptureProgress::StallMs - 1, true, 4024, 850000, true));
  assert(progress.observe(100 + taxi_camera::CaptureProgress::StallMs, true, 4024, 852344, true));
  assert(progress.stalled());  // Observed time-change failure.
  assert(!progress.observe(100 + taxi_camera::CaptureProgress::StallMs + 1, true, 4024, 852344, true));
  assert(!progress.observe(5000, false, 4024, 852344, true));  // No retry during cleanup/stale pose.
  assert(!progress.observe(5001, true, 4025, 852345, false));
  assert(!progress.stalled());  // Fresh capture restores progress.
  assert(!progress.observe(9000, true, 4025, 852345, true));
  assert(!progress.observe(9000 + taxi_camera::CaptureProgress::StallMs, true, 4025, 852345, true));  // No source draws: no speculation.
  assert(progress.observe(9000 + taxi_camera::CaptureProgress::StallMs + 1, true, 4025, 852346, true));
  assert(!progress.observe(13000, true, 4025, 852347, true));
  assert(!progress.observe(12999, true, 4025, 852348, true));  // Clock regression resets timer.
  assert(!progress.observe(20000, true, 4026, 852349, true));
  assert(progress.observe(20000 + taxi_camera::CaptureProgress::StallMs + 1, true, 4026, 852350, true));
  assert(progress.stalled());
  progress.reset();
  assert(!progress.stalled());
  assert(!progress.observe(23000, true, 4026, 852351, true));  // AA restore re-arms the StallMs watch.

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
  assert(tracker.rearm_retained_rt() == 1);
  assert(tracker.state(old_key).model == ss::Model::legacy_rt && !tracker.state(old_key).drawn);
  ss::Recording old_draw;
  assert(old_draw.append({old_key, ss::Effect::Kind::draw}));
  assert(tracker.apply(old_draw) && tracker.state(old_key).drawn);
  tracker.unregister_source(old_key);
  assert(tracker.register_source(new_key, ss::Model::legacy_rt));
  ss::Recording recording;
  assert(recording.append({new_key, ss::Effect::Kind::draw}));
  assert(tracker.apply(recording) && tracker.state(new_key).drawn);
  assert(tracker.state(old_key).model == ss::Model::unknown);
  std::puts("Capture-stall recovery: PASS");
}
