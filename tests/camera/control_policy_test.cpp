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

  // Door-only foreground rendering (`taxi_mask == 0`, `door_render == true`) must
  // qualify for capture-stall recovery and count composed `door_pages` as progress.
  const taxi_camera::CaptureRecoveryDemand base_door{0, true, false, false, false, false, true, 4, false, false, true};
  assert(taxi_camera::capture_recovery_eligible(base_door));
  assert(taxi_camera::capture_recovery_eligible({1, false, false, false, false, false, true, 4, false, false, true}));
  assert(taxi_camera::capture_recovery_eligible({0, false, true, false, false, false, true, 4, false, false, true}));
  assert(!taxi_camera::capture_recovery_eligible({0, false, false, false, false, false, true, 4, false, false, true}));
  for (auto bad : {taxi_camera::CaptureRecoveryDemand{0, true, false, true, false, false, true, 4, false, false, true},
                   taxi_camera::CaptureRecoveryDemand{0, true, false, false, true, false, true, 4, false, false, true},
                   taxi_camera::CaptureRecoveryDemand{0, true, false, false, false, true, true, 4, false, false, true},
                   taxi_camera::CaptureRecoveryDemand{0, true, false, false, false, false, false, 4, false, false, true},
                   taxi_camera::CaptureRecoveryDemand{0, true, false, false, false, false, true, 1, false, false, true},
                   taxi_camera::CaptureRecoveryDemand{0, true, false, false, false, false, true, 4, true, false, true},
                   taxi_camera::CaptureRecoveryDemand{0, true, false, false, false, false, true, 4, false, true, true},
                   taxi_camera::CaptureRecoveryDemand{0, true, false, false, false, false, true, 4, false, false, false}})
    assert(!taxi_camera::capture_recovery_eligible(bad));
  assert(taxi_camera::composed_output_count(1, 323) == 324);

  taxi_camera::CaptureProgress door_progress;
  const bool door_eligible = taxi_camera::capture_recovery_eligible(base_door);
  assert(!door_progress.observe(1000, door_eligible, taxi_camera::composed_output_count(0, 0), 100, true));
  assert(door_progress.observe(1000 + taxi_camera::CaptureProgress::StallMs, door_eligible, taxi_camera::composed_output_count(0, 0), 200,
                               true));
  assert(door_progress.stalled());
  assert(!door_progress.observe(1000 + taxi_camera::CaptureProgress::StallMs + 50, door_eligible, taxi_camera::composed_output_count(0, 1),
                                250, true));
  assert(!door_progress.stalled());

  // 1. Rising door_pages must NOT mask a stalled taxi capture when both are active,
  //    nor credit SceneRecovery while taxi composition is stalled.
  {
    taxi_camera::ForegroundCaptureProgress fg;
    const taxi_camera::CaptureRecoveryDemand both{1, true, false, false, false, false, true, 4, false, false, true};
    auto obs = fg.observe(1000, both, 10, 1, 100, true);
    assert(!obs.rearm && !fg.stalled());
    obs = fg.observe(1250, both, 10, 2, 150, true);
    assert(!obs.rearm && !obs.scene_progress && !fg.stalled());
    obs = fg.observe(1000 + taxi_camera::CaptureProgress::StallMs, both, 10, 3, 200, true);
    assert(obs.rearm && !obs.scene_progress && fg.stalled() && fg.taxi_stalled() && !fg.door_stalled());
    obs = fg.observe(1000 + taxi_camera::CaptureProgress::StallMs + 250, both, 11, 4, 250, false);
    assert(!obs.rearm && obs.scene_progress && !fg.stalled() && !fg.taxi_stalled() && !fg.door_stalled());
  }

  // 2. Rising taxi frames must NOT mask a stalled door capture when both are active.
  {
    taxi_camera::ForegroundCaptureProgress fg;
    const taxi_camera::CaptureRecoveryDemand both{1, true, false, false, false, false, true, 4, false, false, true};
    auto obs = fg.observe(2000, both, 20, 5, 300, true);
    assert(!obs.rearm && !fg.stalled());
    obs = fg.observe(2250, both, 21, 5, 350, true);
    assert(!obs.rearm && obs.scene_progress && !fg.stalled());
    obs = fg.observe(2000 + taxi_camera::CaptureProgress::StallMs, both, 22, 5, 400, true);
    assert(obs.rearm && obs.scene_progress && fg.stalled() && !fg.taxi_stalled() && fg.door_stalled());
    obs = fg.observe(2000 + taxi_camera::CaptureProgress::StallMs + 250, both, 23, 6, 450, false);
    assert(!obs.rearm && !fg.stalled() && !fg.taxi_stalled() && !fg.door_stalled());
  }

  // 3. Door-only mode (taxi_mask == 0, door_render == true) still triggers stall
  //    recovery and recovers on rising door_pages, and reset() clears both paths.
  {
    taxi_camera::ForegroundCaptureProgress fg;
    auto obs = fg.observe(3000, base_door, 0, 10, 500, true);
    assert(!obs.rearm && !fg.stalled());
    obs = fg.observe(3000 + taxi_camera::CaptureProgress::StallMs, base_door, 0, 10, 600, true);
    assert(obs.rearm && !obs.scene_progress && fg.stalled() && !fg.taxi_stalled() && fg.door_stalled());
    obs = fg.observe(3000 + taxi_camera::CaptureProgress::StallMs + 50, base_door, 0, 11, 650, true);
    assert(!obs.rearm && obs.scene_progress && !fg.stalled());
    // Stop sequence reset clears an active stall on both paths.
    assert(fg.observe(4000, base_door, 0, 11, 700, true).rearm == false);
    assert(fg.observe(4000 + taxi_camera::CaptureProgress::StallMs, base_door, 0, 11, 750, true).rearm);
    assert(fg.stalled());
    fg.reset();
    assert(!fg.stalled() && !fg.taxi_stalled() && !fg.door_stalled());
  }

  std::puts("Capture-stall recovery: PASS");
}
